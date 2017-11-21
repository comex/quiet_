#include "types.h"
#include "logging.h"
#include "misc.h"
#include "usbducks.h"
#include "circ_buf.h"
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct relay_connection {
    bool active;
    struct circ_buf tcp_to_usb_cb;
    struct circ_buf usb_to_tcp_cb;
    char tcp_to_usb_backing[1048576];
    char usb_to_tcp_backing[1048576];
    int conn_fd;
};

struct relay_remote_listener {
    bool active;
    int server_fd;
    uint16_t port;
};

struct relay_listener {
    uint16_t src_port;
    uint16_t dst_port;
};

struct relay {
    int wakeup_relay_main_pipe[2];

    struct relay_connection relay_connections[USBDUCKS_MAX_CONNECTIONS];
    struct relay_remote_listener relay_remote_listeners[USBDUCKS_MAX_LISTENERS];
    struct relay_listener relay_listeners[USBDUCKS_MAX_LISTENERS];
};

static void
wakeup_relay_main(struct relay *relay) {
    char c = '!';
    ensure_errno(1 == write(relay->wakeup_relay_main_pipe[1], &c, 1));
}

static struct relay_connection *init_relay_conn(struct relay *relay,
                                                struct usbducks_connection *conn);

static void
send_if_necessary(struct usbducks_connection *conn) LOCKED {
    struct relay_connection *rc = (struct relay_connection *)conn->user;
    const void *in;
    size_t cap;
    if (rc->tcp_to_usb_cb.len != 0 && conn->pending_send_len == 0
        && conn->clear_to_send > 0) {
        circ_buf_shift_start(&rc->tcp_to_usb_cb, &in, &cap);
        ensure(cap);
        usbducks_send_async(conn, in, cap);
    }
}

static void
relay_on_send_complete(struct usbducks_connection *conn, size_t actual, uint64_t time) LOCKED {
    struct relay_connection *rc = (struct relay_connection *)conn->user;
    if (USBDUCKS_VERBOSE >= 1)
        log("relay_on_send_complete(%p, %zu, time=%lluus)\n", conn, actual, time);
    bool was_full = circ_buf_avail_space(&rc->tcp_to_usb_cb) == 0;
    circ_buf_shift_finish(&rc->tcp_to_usb_cb, (size_t)actual);
    if (was_full)
        wakeup_relay_main((struct relay *)conn->ud->user);
    send_if_necessary(conn);
}

static void
relay_on_clear_to_send(struct usbducks_connection *conn) LOCKED {
    send_if_necessary(conn);
}

static void
relay_on_receive(struct usbducks_connection *conn, const void *buf, size_t len) LOCKED {
    struct relay_connection *rc = (struct relay_connection *)conn->user;
    if (USBDUCKS_VERBOSE >= 1)
        log("relay_on_receive(%p, %zu)\n", buf, len);
    if (circ_buf_push_bytes(&rc->usb_to_tcp_cb, buf, len) != len)
        panic("relay_on_receive: sent me too much data");
    wakeup_relay_main((struct relay *)conn->ud->user);
}

static void
relay_on_disconnect(struct usbducks_connection *conn) LOCKED {
    struct relay_connection *rc = (struct relay_connection *)conn->user;
    log("relay_on_disconnect(%p)\n", conn);
    ensure(rc->active);
    rc->active = false;
    if (rc->conn_fd != -1)
        close(rc->conn_fd);
}

static void
relay_on_remote_listen(struct usbducks *ud, uint16_t port) LOCKED {
    struct relay *relay = (struct relay *)ud->user;
    for (size_t i = 0; i < USBDUCKS_MAX_LISTENERS; i++) {
        struct relay_remote_listener *rl = &relay->relay_remote_listeners[i];
        if (rl->active && rl->port == port) {
            log("relay_on_remote_listen: duplicate remote listen on port %d\n", port);
            return;
        }
    }
    for (size_t i = 0; i < USBDUCKS_MAX_LISTENERS; i++) {
        struct relay_remote_listener *rl = &relay->relay_remote_listeners[i];
        if (!rl->active) {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            ensure_errno(sock >= 0);
            int one = 1;
            ensure_errno(
                !setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)));
            struct sockaddr_in sa = {
                .sin_family = AF_INET,
                .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                .sin_port = htons(port),
            };
            log("bind to 127.0.0.1:%d\n", port);
            ensure_errno(!bind(sock, (struct sockaddr *)&sa, sizeof(sa)));
            ensure_errno(!listen(sock, 5));
            ensure_errno(0 <= fcntl(sock, F_SETFL, O_NONBLOCK));

            rl->active = true;
            rl->port = port;
            rl->server_fd = sock;
            wakeup_relay_main(relay);
            return;
        }
    }
    log("relay_on_remote_listen: no free listeners!\n");
}

static void
relay_on_incoming_conn(struct usbducks_listener *listener,
                       struct usbducks_connection *conn) LOCKED {
    uint16_t port = listener->port;
    log("incoming connection from USB on port %d (conn_id %u)\n", port, conn->conn_id);
    struct relay_listener *rel = (struct relay_listener *)listener->user;
    struct relay *relay = (struct relay *)conn->ud->user;
    struct relay_connection *rc = init_relay_conn(relay, conn);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    ensure_errno(0 <= fcntl(sock, F_SETFL, O_NONBLOCK));
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(rel->dst_port),
    };
    int ret = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    if (ret < 0 && errno != EINPROGRESS) {
        log("...failed to connect to 127.0.0.1:%d: %s\n", rel->dst_port, strerror(errno));
        close(sock);
        usbducks_disconnect(conn);
        return;
    }
    rc->conn_fd = sock;
    wakeup_relay_main(relay);
}

struct relay_connection *
init_relay_conn(struct relay *relay, struct usbducks_connection *conn) LOCKED {
    struct relay_connection *rc = &relay->relay_connections[conn->conn_id];
    ensure(!rc->active);
    rc->active = true;
    circ_buf_init(&rc->usb_to_tcp_cb, rc->usb_to_tcp_backing,
                  sizeof(rc->usb_to_tcp_backing));
    circ_buf_init(&rc->tcp_to_usb_cb, rc->tcp_to_usb_backing,
                  sizeof(rc->tcp_to_usb_backing));
    rc->conn_fd = -1;

    conn->user = rc;
    conn->on_send_complete = relay_on_send_complete;
    conn->on_clear_to_send = relay_on_clear_to_send;
    conn->on_receive = relay_on_receive;
    conn->on_disconnect = relay_on_disconnect;

    usbducks_clear_to_recv(conn, circ_buf_avail_space(&rc->usb_to_tcp_cb));
    return rc;
}

static void
relay_init(struct relay *relay) {
    for (size_t i = 0; i < USBDUCKS_MAX_LISTENERS; i++)
        relay->relay_remote_listeners[i].active = false;
    ensure_errno(!pipe(relay->wakeup_relay_main_pipe));
    for (size_t i = 0; i < 2; i++)
        ensure_errno(0 <= fcntl(relay->wakeup_relay_main_pipe[i], F_SETFL, O_NONBLOCK));
}

static void
relay_main(struct relay *relay, struct usbducks *ud) {
    while (1) {
        usbducks_backend_lock(ud);
        static const size_t pollfds_max
            = 1 + USBDUCKS_MAX_CONNECTIONS + USBDUCKS_MAX_LISTENERS;
        struct pollfd pollfds[pollfds_max];
        enum t { T_WAKEUP, T_CONN, T_LISTENER };
        struct ref {
            enum t type;
            union {
                struct usbducks_connection *conn;
                struct relay_remote_listener *rl;
            };
        } ref[pollfds_max];
        size_t pfdi = 0;
        ref[pfdi].type = T_WAKEUP;
        pollfds[pfdi++] = (struct pollfd){
            .fd = relay->wakeup_relay_main_pipe[0],
            .events = POLLIN,
        };
        for (size_t i = 0; i < USBDUCKS_MAX_CONNECTIONS; i++) {
            struct usbducks_connection *conn = &ud->connections[i];
            if (conn->active) {
                struct relay_connection *rc = (struct relay_connection *)conn->user;
                if (rc->conn_fd != -1) {
                    ref[pfdi] = (struct ref){.type = T_CONN, .conn = conn};
                    // log("POLLING on CONNECTION %d\n", rc->conn_fd);
                    pollfds[pfdi++] = (struct pollfd){
                        .fd = rc->conn_fd,
                        .events = (short)(//
                            (circ_buf_avail_space(&rc->tcp_to_usb_cb) ? POLLIN : 0) | //
                            (rc->usb_to_tcp_cb.len > 0 ? POLLOUT : 0)),
                    };
                }
            }
        }
        for (size_t i = 0; i < USBDUCKS_MAX_LISTENERS; i++) {
            struct relay_remote_listener *rl = &relay->relay_remote_listeners[i];
            if (rl->active) {
                // log("POLLING on LISTENER %d\n", rl->server_fd);
                ref[pfdi] = (struct ref){.type = T_LISTENER, .rl = rl};
                pollfds[pfdi++] = (struct pollfd){
                    .fd = rl->server_fd,
                    .events = POLLIN,
                };
            }
        }
        usbducks_backend_unlock(ud);
        nfds_t pfd_count = (nfds_t)pfdi;
        ensure_errno(0 <= poll(pollfds, pfd_count, -1));
        usbducks_backend_lock(ud);
        bool should_wakeup_usb = false;
        for (pfdi = 0; pfdi < pfd_count; pfdi++) {
            if (pollfds[pfdi].revents) {
                /*
                log("EVENT revents %d reftype %d fd %d\n", pollfds[pfdi].revents,
                    ref[pfdi].type, pollfds[pfdi].fd);
                */
                switch (ref[pfdi].type) {
                case T_WAKEUP: {
                    while (1) {
                        char c;
                        ssize_t actual = read(relay->wakeup_relay_main_pipe[0], &c, 1);
                        ensure_errno(
                            actual > 0
                            || (actual < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)));
                        if (actual <= 0)
                            break;
                    }
                    break;
                }
                case T_CONN: {
                    struct usbducks_connection *conn = ref[pfdi].conn;
                    struct relay_connection *rc = (struct relay_connection *)conn->user;
                    // try receiving
                    while (1) {
                        void *out;
                        size_t cap;
                        circ_buf_push_start(&rc->tcp_to_usb_cb, &out, &cap);
                        // log("recv:out=%p cap=%zu\n", out, cap);
                        if (!cap)
                            break;
                        ssize_t actual = read(pollfds[pfdi].fd, out, cap);
                        // log("read actual=%zd\n", actual);
                        if (actual == 0
                            || (actual < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                            log("read(): actual=%zd err=%s\n", actual, strerror(errno));
                            usbducks_disconnect(conn);
                            goto skip;
                        }
                        if (actual <= 0)
                            break;
                        circ_buf_push_finish(&rc->tcp_to_usb_cb, (size_t)actual);
                        send_if_necessary(conn);
                        should_wakeup_usb = true;
                    }
                    // try sending
                    while (1) {
                        const void *in;
                        size_t cap;
                        circ_buf_shift_start(&rc->usb_to_tcp_cb, &in, &cap);
                        if (!cap)
                            break;
                        ssize_t actual = write(pollfds[pfdi].fd, in, cap);
                        if (USBDUCKS_VERBOSE >= 1)
                            log("write actual=%zd\n", actual);
                        if (actual == 0
                            || (actual < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                            log("write(): actual=%zd err=%s\n", actual, strerror(errno));
                            usbducks_disconnect(conn);
                            break;
                        }
                        if (actual <= 0)
                            break;
                        circ_buf_shift_finish(&rc->usb_to_tcp_cb, (size_t)actual);
                        usbducks_clear_to_recv(conn, (size_t)actual);
                        should_wakeup_usb = true;
                    }
                skip:
                    break;
                }
                case T_LISTENER: {
                    struct relay_remote_listener *rl = ref[pfdi].rl;
                    while (1) {
                        int conn_fd = accept(pollfds[pfdi].fd, NULL, NULL);
                        if (conn_fd < 0) {
                            if (errno != EAGAIN && errno != EWOULDBLOCK)
                                log("accept(): conn_fd=%d err=%s\n", conn_fd,
                                    strerror(errno));
                            break;
                        }
                        log("accepted local connection on port %d\n", rl->port);
                        ensure_errno(0 <= fcntl(conn_fd, F_SETFL, O_NONBLOCK));
                        struct usbducks_connection *conn = usbducks_connect(ud, rl->port);
                        if (!conn) {
                            log("...but out of 'usbducks_connection's\n");
                            close(conn_fd);
                            break;
                        }

                        struct relay_connection *rc = init_relay_conn(relay, conn);
                        rc->conn_fd = conn_fd;
                        should_wakeup_usb = true;
                    }
                    break;
                }
                }
            }
        }
        if (should_wakeup_usb)
            usbducks_start_transfers_if_necessary(ud);
        usbducks_backend_unlock(ud);
    }
}

static long
to_integer(const char *str, long min, long max, bool *failp) {
    errno = 0;
    char *ep;
    long val = strtol(str, &ep, 0);
    if (errno || str[0] == '\0' || ep[0] != '\0' || val < min || val > max) {
        *failp = true;
        return 0;
    }
    return val;
}

int
main(int argc, char **argv) {
    log_init(0, 0, nullptr);

    bool bad = false;
    static struct usbducks the_ud;
    static struct relay the_relay;
    size_t num_listeners = 0;
    for (char **argp = &argv[1]; *argp; argp++) {
        char *arg = *argp;

        if (num_listeners >= USBDUCKS_MAX_LISTENERS) {
            log("too many listeners (max: %u)\n", (int)USBDUCKS_MAX_LISTENERS);
            bad = true;
            break;
        }
        struct relay_listener *rel = &the_relay.relay_listeners[num_listeners++];

        char *src_port = arg, *dst_port;
        char *colon = strchr(arg, ':');
        if (colon) {
            *colon = 0;
            dst_port = colon + 1;
        } else {
            dst_port = src_port;
        }
        bool fail = false;
        rel->src_port = (uint16_t)to_integer(src_port, 0, 65535, &fail);
        rel->dst_port = (uint16_t)to_integer(dst_port, 0, 65535, &fail);
        if (fail) {
            if (colon)
                *colon = ':';
            log("invalid argument [%s], expected src_port:dst_port (e.g. 1234:5678)",
                arg);
            bad = true;
            num_listeners--;
        }
    }
    if (bad)
        return 1;

    relay_init(&the_relay);
    usbducks_init(&the_ud);
    usbducks_backend_lock(&the_ud);
    the_ud.user = &the_relay;
    the_ud.on_remote_listen = relay_on_remote_listen;

    for (size_t i = 0; i < num_listeners; i++) {
        struct relay_listener *rel = &the_relay.relay_listeners[i];
        log("listening over USB on port %d, forwarding to 127.0.0.1:%d\n", rel->src_port,
            rel->dst_port);
        struct usbducks_listener *list = usbducks_listen(&the_ud, rel->src_port);
        if (!list)
            panic("usbducks_listen returned NULL");
        list->user = &the_relay.relay_listeners[i];
        list->on_incoming_conn = relay_on_incoming_conn;
    }

    usbducks_start(&the_ud);
    usbducks_backend_unlock(&the_ud);
    relay_main(&the_relay, &the_ud);
}

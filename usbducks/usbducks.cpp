#include "usbducks.h"
#include "decls.h"
#include "logging.h"
#include "misc.h"

#undef THIS_C_FILE_IS_USBDUCKS_RELATED
#define THIS_C_FILE_IS_USBDUCKS_RELATED 1

constexpr bool USBDUCKS_PING_TEST_MODE = DUMMY ? false : false;
constexpr int USBDUCKS_PING_INTERVAL_US = USBDUCKS_PING_TEST_MODE ? 50000 : 750000;

static uint8_t
flip_conn_id(uint8_t conn_id) {
    // flip so that each side's local connections are
    // 0..(USBDUCKS_MAX_CONNECTIONS_PER_DIRECTION-1):
    return (uint8_t)(USBDUCKS_MAX_CONNECTIONS - 1 - conn_id);
}

static void *
usbducks_add_packet(struct usbducks *ud, struct usbducks_usbtransfer *ut,
                    uint8_t pkt_type, uint8_t conn_id, size_t body_len) LOCKED {
    if (USBDUCKS_VERBOSE >= 2 || (USBDUCKS_VERBOSE >= 1 && pkt_type != 0))
        log("sending pkt_type=%u seq=%u len=%u\n", pkt_type, ud->next_seq_num,
            (int)body_len);

    size_t pktlen
        = sizeof(struct usbducks_pkthdr) + body_len + sizeof(struct usbducks_pkttrailer);
    ensure(pktlen >= sizeof(struct usbducks_pkthdr) + sizeof(struct usbducks_pkttrailer));
    size_t off = ut->buf_len;
    ensure(ut->buf_cap - off >= pktlen);
    ut->buf_len = off + pktlen;

    struct usbducks_pkthdr *ph = (struct usbducks_pkthdr *)((char *)ut->buf + off);
    ph->magic = bswap(USBDUCKS_MAGIC);
    ph->version = bswap(USBDUCKS_VERSION);
    ph->pkt_type = bswap(pkt_type);
    ph->conn_id = bswap(conn_id);
    ph->seq_num = bswap(ud->next_seq_num++);
    ph->client_id = bswap((uint32_t)ud->my_client_id);
    ph->body_len = bswap((uint32_t)body_len);

    char *body = (char *)ph->body;
    struct usbducks_pkttrailer *pt = (struct usbducks_pkttrailer *)(body + body_len);
    pt->client_id = ph->client_id;
    pt->seq_num = ph->seq_num;

    return body;
}

static bool
usbducks_handle_incoming_packet(struct usbducks *ud, const struct usbducks_pkthdr *ph,
                                size_t body_len) LOCKED {
    uint8_t conn_id = ph->conn_id;
    uint8_t seq_num = ph->seq_num;
    uint32_t client_id = bswap(ph->client_id);

    ensure(conn_id < USBDUCKS_MAX_CONNECTIONS);
    conn_id = flip_conn_id(conn_id);

    if (USBDUCKS_VERBOSE >= 2)
        log("hip<- %d seq=%u ss=%u\n", ph->pkt_type, seq_num, ud->sync_state);

    if (ud->have_peer_client_id && client_id != ud->peer_client_id) {
        log("packet from new client ID %x (expected %x), resetting!\n", client_id,
            ud->peer_client_id);
        usbducks_reset_peer(ud);
        return false;
    }
    ud->have_peer_client_id = true;
    ud->peer_client_id = client_id;

    if (ud->have_peer_last_seq_num && seq_num != (uint8_t)(ud->peer_last_seq_num + 1)) {
        panic("SEQ_NUM OUT OF ORDER: got seq_num=%u, last=%u", seq_num,
              ud->peer_last_seq_num);
    }
    ud->have_peer_last_seq_num = true;
    ud->peer_last_seq_num = seq_num;

    switch (ph->pkt_type) {
    case USBDUCKS_PKTTYPE_PING: {
        if (USBDUCKS_VERBOSE >= 1)
            log("incoming ping: %x (mine=%x)\n", ud->peer_client_id, ud->my_client_id);
        ud->need_pong_len = body_len;
        ud->need_pong_seq_num = seq_num;
        ud->need_pong_ping_time = cur_time_us();
        return true;
    }
    case USBDUCKS_PKTTYPE_PONG: {
        if (USBDUCKS_VERBOSE >= 1)
            log("incoming pong: len=%zu", body_len);
        if (body_len >= sizeof(struct usbducks_pong_data)) {
            const struct usbducks_pong_data *pong = (struct usbducks_pong_data *)ph->body;
            uint32_t pong_client_id = bswap(pong->client_id);
            uint8_t pong_seq_num = bswap(pong->seq_num);
            if (USBDUCKS_VERBOSE >= 1) {
                log(" client_id=%x(mine=%x) seq=%#x(expected=%#x",
                    pong_client_id, ud->my_client_id,
                    pong_seq_num, ud->last_outgoing_ping_seq_num);
                if (ud->have_last_outgoing_ping && pong_seq_num == ud->last_outgoing_ping_seq_num) {
                    int64_t diff = (int64_t)(cur_time_us() - ud->last_outgoing_ping_time);
                    log(", rtt %lldus)", diff);
                } else {
                    log(", mismatch)");
                }
                log("\n");
            }
            if (ud->sync_state == USS_PING_PONG && pong_client_id == ud->my_client_id) {
                ud->sync_state = USS_NORMAL; // yay
                if (USBDUCKS_VERBOSE >= 1)
                    log("   now state => normal\n");
            }
        } else {
            log("\n");
        }
        return true;
    }
    case USBDUCKS_PKTTYPE_IGNORE:
        return true;
    case USBDUCKS_PKTTYPE_CONNECT: {
        ensure(conn_id < USBDUCKS_MAX_CONNECTIONS_PER_DIRECTION);
        struct usbducks_connection *conn = &ud->connections[conn_id];
        ensure(body_len == 2);
        uint16_t port = bswap(*(uint16_t *)ph->body);
        log("<- incoming connection, conn_id %u port %u\n", conn_id, port);
        if (conn->active) {
            log("! duplicate connection\n");
            return true;
        }
        conn->active = true;
        conn->port = port;
        conn->told_remote = true;
        conn->epoch = ud->epoch;

        struct usbducks_listener *list;
        for (size_t i = 0; i < USBDUCKS_MAX_LISTENERS; i++) {
            list = &ud->listeners[i];
            if (list->port == port) {
                if (list->on_incoming_conn)
                    list->on_incoming_conn(list, conn);
                return true;
            }
        }
        log("! no listener\n");
        conn->active = false;
        return true;
    }
    case USBDUCKS_PKTTYPE_DATA: {
        if (USBDUCKS_VERBOSE >= 1)
            log("<- data conn_id %u len %u\n", conn_id, (int)body_len);
        struct usbducks_connection *conn = &ud->connections[conn_id];
        if (!conn->active) {
            log("discarding %u-byte packet from inactive connection %u\n", (int)body_len,
                conn_id);
            return true;
        }
        if (conn->on_receive)
            conn->on_receive(conn, ph->body, body_len);
        return true;
    }
    case USBDUCKS_PKTTYPE_CLEAR_TO_SEND: {
        struct usbducks_connection *conn = &ud->connections[conn_id];
        ensure(body_len == 4);
        uint32_t val = bswap(*(uint32_t *)ph->body);
        if (USBDUCKS_VERBOSE >= 1)
            log("<- cts (conn_id %u) %u\n", conn_id, val);
        if (!conn->active) {
            log("discarding clear-to-send from inactive connection %u\n", conn_id);
            return true;
        }
        conn->clear_to_send += val;
        if (conn->on_clear_to_send)
            conn->on_clear_to_send(conn);
        return true;
    }
    case USBDUCKS_PKTTYPE_DISCONNECT: {
        log("<- disconnect conn_id %u\n", conn_id);
        struct usbducks_connection *conn = &ud->connections[conn_id];
        conn->told_remote = false;
        usbducks_disconnect(conn);
        return true;
    }
    case USBDUCKS_PKTTYPE_I_AM_LISTENING: {
        ensure(body_len == 2);
        uint16_t port = bswap(*(uint16_t *)ph->body);
        log("<- i_am_listening port %u\n", port);
        if (ud->on_remote_listen)
            ud->on_remote_listen(ud, port);
        return true;
    }
    default:
        panic("unknown incoming pkttype %d", ph->pkt_type);
    }
}

static void
usbducks_handle_incoming(struct usbducks *ud,
                         struct usbducks_usbtransfer *restrict ut_new) LOCKED {
    ut_new->buf_off = 0;

    if (USBDUCKS_VERBOSE >= 2)
        log("handle_incoming %p,%u state=%d\n", ut_new->buf, (int)ut_new->buf_len, ud->sync_state);

    if (ud->sync_state == USS_SYNC) {
        // handle syncing

        char *buf = (char *)ut_new->buf;
        size_t buf_len = ut_new->buf_len;

        uint64_t magic = bswap(USBDUCKS_MAGIC);
        size_t sync_off_into_magic = ud->sync_off_into_magic;
        size_t sync_muggle_len = ud->sync_muggle_len;
        char next_char = ((char *)&magic)[sync_off_into_magic];
        for (size_t i = 0; i < buf_len;) {
            char c = buf[i++];
            sync_muggle_len++;
            if (unlikely(c == next_char)) {
                if (++sync_off_into_magic == 8) {
                    if (sync_muggle_len >= USBDUCKS_USBTRANSFER_RECV_BUF_CAP) {
                        ut_new->buf_off = i;
                        ut_new->buf_len = buf_len - i;
                        goto sync_ok;
                    }
                    sync_muggle_len = 0;
                    sync_off_into_magic = 0;
                }
                next_char = ((char *)&magic)[sync_off_into_magic];
            }
        }
        ud->sync_off_into_magic = sync_off_into_magic;
        ud->sync_muggle_len = sync_muggle_len;
        return;
    sync_ok:
        log("usbducks_handle_incoming: sync OK\n");
        ud->sync_state = USS_PING_PONG;
        ud->skip_next_magic = true;
    }

    struct usbducks_usbtransfer_list *list = &ud->transfer_lists[UDD_RECV];
    while (1) {
        bool appending_to_partial = list->partial_transfer_idx != (uint8_t)-1;
        struct usbducks_usbtransfer *restrict ut;
        if (appending_to_partial)
            ut = &list->transfers[list->partial_transfer_idx];
        else
            ut = ut_new;

        size_t ut_buf_off = ut->buf_off, ut_buf_len = ut->buf_len;

        if (ut_buf_len == 0)
            return; // no more data, all good

        size_t skipped_magic_len = ud->skip_next_magic ? 8 : 0;

        if (USBDUCKS_VERBOSE >= 2)
            log("usbducks_handle_incoming: ut={buf=%p,off=%u,len=%u} ut_new={buf=%p,off=%u,len=%u} partial=%u sync=%u sml=%u\n",
                ut->buf, (int)ut->buf_off, (int)ut->buf_len,
                ut_new->buf, (int)ut_new->buf_off, (int)ut_new->buf_len,
                (int)appending_to_partial, ud->sync_state, (int)skipped_magic_len);

        size_t needed_len = sizeof(struct usbducks_pkthdr) - skipped_magic_len;
        bool have_header = ut_buf_len >= needed_len;
        size_t body_len;
        if (have_header) {
            struct usbducks_pkthdr *ph = (struct usbducks_pkthdr *)((char *)ut->buf + ut_buf_off - skipped_magic_len);
            if (!skipped_magic_len)
                ensure_eq(bswap(ph->magic), USBDUCKS_MAGIC);
            ensure_eq(bswap(ph->version), USBDUCKS_VERSION);
            body_len = bswap(ph->body_len);
            needed_len = sizeof(*ph) + body_len + sizeof(struct usbducks_pkttrailer);
            ensure(needed_len >= sizeof(*ph) + sizeof(struct usbducks_pkttrailer));
            ensure_op(needed_len, <=, USBDUCKS_USBTRANSFER_RECV_BUF_CAP);
            needed_len -= skipped_magic_len;
        }

        if (USBDUCKS_VERBOSE >= 2)
            log("usbducks_handle_incoming: have_header=%u needed_len=%u\n", have_header,
                (int)needed_len);

        if (needed_len > ut_buf_len && appending_to_partial) {
            size_t fill_len = min(ut_new->buf_len, needed_len - ut_buf_len);
            if (fill_len > ut->buf_cap - (ut_buf_off + ut_buf_len)) {
                ensure(fill_len <= ut->buf_cap - ut_buf_len);
                memmove(ut->buf, (char *)ut->buf + ut_buf_off, ut_buf_len);
                ut_buf_off = 0;
            }
            memcpy((char *)ut->buf + ut_buf_off + ut_buf_len,
                   (char *)ut_new->buf + ut_new->buf_off,
                   fill_len);
            ut_buf_len += fill_len;
            ut_new->buf_off += fill_len;
            ut_new->buf_len -= fill_len;
        }

        ut->buf_off = ut_buf_off; ut->buf_len = ut_buf_len;

        if (needed_len > ut_buf_len) {
            if (appending_to_partial)
                ensure(ut_new->buf_len == 0); // we consumed all of the latest chunk
                                              // but still don't have a full packet
            atomic_store_explicit(&ut->state, UTS_PARTIAL, memory_order_relaxed);
            list->partial_transfer_idx = (uint8_t)(ut - list->transfers);
            return;
        } else if (have_header) {
            // ok, we have the whole packet!
            list->partial_transfer_idx = (uint8_t)-1;
            struct usbducks_pkthdr *ph = (struct usbducks_pkthdr *)((char *)ut->buf + ut_buf_off - skipped_magic_len);
            char *body = (char *)ph->body;
            struct usbducks_pkttrailer *pt = (struct usbducks_pkttrailer *)(body + body_len);
            ensure_eq(bswap(pt->client_id), bswap(ph->client_id));
            ensure_eq(bswap(pt->seq_num), bswap(ph->seq_num));
            ut_buf_off += needed_len;
            ut_buf_len -= needed_len;

            ut->buf_off = ut_buf_off; ut->buf_len = ut_buf_len;
            atomic_store_explicit(&ut->state, UTS_FREE, memory_order_relaxed);
            ud->skip_next_magic = false;

            if (!usbducks_handle_incoming_packet(ud, ph, body_len))
                return; // drop the rest
        }
    }
}

static void
usbducks_usbtransfer_finished(struct usbducks *ud,
                              struct usbducks_usbtransfer *ut) LOCKED {
    enum usbducks_transfer_retcode rc = ut->rc;
    size_t actual = ut->actual_len;
    enum usbducks_direction direction = ut->direction;
    struct usbducks_usbtransfer_list *list = &ud->transfer_lists[direction];

    int64_t diff = (int64_t)(cur_time_us() - ut->start_time);
    if (USBDUCKS_VERBOSE >= 2)
        log("usbducks_usbtransfer_finished(%p, dir=%s, rc=%d, actual=%d/%d) after "
            "%lldus\n",
            ut, direction == UDD_SEND ? "send" : "recv", rc, (int)actual,
            (int)ut->buf_len, diff);

    list->bytes_xferred += ut->buf_len;

    if (rc == UTR_ERROR || (rc == UTR_TIMED_OUT && direction == UDD_SEND)) {
        if (ud->has_sent_successfully) {
            log("failz, reset time\n");
            usbducks_reset_peer(ud);
        }
    } else if (rc == UTR_OK && direction == UDD_RECV) {
        // success
        ut->buf_len = actual;
        usbducks_handle_incoming(ud, ut);
    } else if (rc == UTR_OK && direction == UDD_SEND) {
        if (actual < ut->buf_len) {
            panic("actual=%u buf_len=%u", (int)actual, (int)ut->buf_len);
        }
        ud->has_sent_successfully = true;
        // xxx
    }

    list->first_pending_transfer_idx = ut->next_pending_transfer_idx;
    if (list->first_pending_transfer_idx == (uint8_t)-1)
        list->last_pending_transfer_idx = (uint8_t)-1;
    if (UTS_PARTIAL != atomic_load_explicit(&ut->state, memory_order_relaxed))
        atomic_store_explicit(&ut->state, UTS_FREE, memory_order_relaxed);
}

void
usbducks_check_finished_usbtransfers(struct usbducks *ud) {
    for (int direction = UDD_SEND; direction <= UDD_RECV; direction++) {
        struct usbducks_usbtransfer_list *list = &ud->transfer_lists[direction];
        while (list->first_pending_transfer_idx != (uint8_t)-1) {
            struct usbducks_usbtransfer *ut
                = &list->transfers[list->first_pending_transfer_idx];
            enum usbducks_transfer_state state
                = atomic_load_explicit(&ut->state, memory_order_acquire);
            if (state == UTS_FINISHED) {
                list->pending_count--;
                usbducks_usbtransfer_finished(ud, ut);
                break;
            } else {
                ensure_eq(state, UTS_PENDING);
                break;
            }
        }
    }
}

static struct usbducks_usbtransfer *
usbducks_get_next_usbtransfer(struct usbducks *ud, enum usbducks_direction direction) LOCKED {
    struct usbducks_usbtransfer_list *list = &ud->transfer_lists[direction];
    for (size_t i = 0, count = list->total_count; i < count; i++) {
        struct usbducks_usbtransfer *ut = &list->transfers[i];
        if (UTS_FREE == atomic_load_explicit(&ut->state, memory_order_acquire))
            return ut;
    }
    return NULL;
}

static void
usbducks_usbtransfer_start(struct usbducks_usbtransfer *ut) LOCKED {
    // log("usbducks_usbtransfer_start: buf_len=%x dir=%d\n", (int)ut->buf_len,
    // ut->direction);
    struct usbducks *ud = ut->ud;
    ensure(ut == usbducks_get_next_usbtransfer(ud, ut->direction));
    ensure(ut->buf_len <= ut->buf_cap);
    ensure(UTS_FREE == atomic_load_explicit(&ut->state, memory_order_acquire));
    atomic_store_explicit(&ut->state, UTS_PENDING, memory_order_release);

    struct usbducks_usbtransfer_list *list = &ud->transfer_lists[ut->direction];
    uint8_t idx = (uint8_t)(ut - list->transfers);
    if (list->first_pending_transfer_idx == (uint8_t)-1)
        list->first_pending_transfer_idx = idx;
    else
        list->transfers[list->last_pending_transfer_idx].next_pending_transfer_idx = idx;
    ut->next_pending_transfer_idx = (uint8_t)-1;
    list->last_pending_transfer_idx = idx;
    list->pending_count++;

    usbducks_backend_usbtransfer_start(ut);
    uint64_t now = cur_time_us();
    ut->start_time = now;
    if (ut->direction == UDD_SEND) {
        if (USBDUCKS_VERBOSE >= 2)
            log("starting send(%p, len=%zu)\n", ut, ut->buf_len);
    } else {
        if (USBDUCKS_VERBOSE >= 2)
            log("starting recv(%p) @ %llu\n", ut, now);
    }
}

void
usbducks_start_transfers_if_necessary(struct usbducks *ud) {
    struct usbducks_usbtransfer *ut;
    while ((ut = usbducks_get_next_usbtransfer(ud, UDD_RECV)) != NULL) {
        // log("starting receive %p\n", ut);
        ut->buf_len = ut->buf_cap;
        usbducks_usbtransfer_start(ut);
    }

    if ((ut = usbducks_get_next_usbtransfer(ud, UDD_SEND)) == NULL)
        return;
    ut->buf_len = 0;

    uint64_t now = cur_time_us();
    if (ud->need_pong_len) {
        ensure(ud->sync_state >= USS_PING_PONG);
        size_t pong_len = ud->need_pong_len;
        char *p = (char *)usbducks_add_packet(ud, ut, USBDUCKS_PKTTYPE_PONG, 0, pong_len);
        memset(p, 'a', pong_len);
        if (pong_len >= sizeof(struct usbducks_pong_data)) {
            struct usbducks_pong_data *pong = (struct usbducks_pong_data *)p;
            pong->client_id = bswap(ud->peer_client_id);
            pong->seq_num = ud->need_pong_seq_num;
        }
        if (USBDUCKS_VERBOSE >= 1)
            log("sending pong client_id=%x len=%zu, time since ping %lluus\n", ud->peer_client_id, pong_len, now - ud->need_pong_ping_time);
        ud->need_pong_len = 0;
        if (!USBDUCKS_PING_TEST_MODE)
            ud->last_outgoing_pingpong_time = now;
        goto no_more;
    } else if ((int64_t)(now - ud->last_outgoing_pingpong_time) >= USBDUCKS_PING_INTERVAL_US) {
        size_t ping_len = ut->buf_cap - sizeof(struct usbducks_pkthdr) - sizeof(struct usbducks_pkttrailer);
        if (USBDUCKS_PING_TEST_MODE && ud->sync_state == USS_NORMAL) {
            ping_len = 1 << (usbducks_random_u32() % 12);
        }
        void *p = usbducks_add_packet(ud, ut, USBDUCKS_PKTTYPE_PING, 0, ping_len);
        memset(p, 'a', ping_len);

        ud->last_outgoing_pingpong_time = now;

        ud->have_last_outgoing_ping = true;
        ud->last_outgoing_ping_seq_num = (uint8_t)(ud->next_seq_num - 1);
        ud->last_outgoing_ping_time = now;

        if (USBDUCKS_VERBOSE >= 1)
            log("sending ping len %zu seq %#x\n", ping_len, ud->last_outgoing_ping_seq_num);

        goto no_more;
    }

    if (ud->sync_state == USS_NORMAL) {
        for (size_t i = 0; i < USBDUCKS_MAX_LISTENERS; i++) {
            struct usbducks_listener *list = &ud->listeners[i];
            if (list->active && !list->told_remote) {
                uint16_t val = bswap(list->port);
                void *p = usbducks_add_packet(ud, ut, USBDUCKS_PKTTYPE_I_AM_LISTENING, 0,
                                              sizeof(uint16_t));
                memcpy(p, &val, sizeof(val));
                list->told_remote = true;
                log("-> telling remote I am listening on port %d\n", list->port);
            }
        }

        size_t num_pending_conns = 0;
        for (size_t i = 0; i < USBDUCKS_MAX_CONNECTIONS; i++) {
            struct usbducks_connection *conn = &ud->connections[i];
            if (conn->active != conn->told_remote) {
                uint16_t val = bswap(conn->port);
                void *p = usbducks_add_packet(
                    ud, ut,
                    conn->active ? USBDUCKS_PKTTYPE_CONNECT : USBDUCKS_PKTTYPE_DISCONNECT,
                    flip_conn_id(conn->conn_id), sizeof(uint16_t));
                memcpy(p, &val, sizeof(val));
                conn->told_remote = conn->active;
            }

            if (conn->active && conn->clear_to_recv > 0) {
                uint32_t val = bswap((uint32_t)conn->clear_to_recv);
                void *p
                    = usbducks_add_packet(ud, ut, USBDUCKS_PKTTYPE_CLEAR_TO_SEND,
                                          flip_conn_id(conn->conn_id), sizeof(uint32_t));

                memcpy(p, &val, sizeof(val));
                conn->clear_to_recv = 0;
            }
            if (conn->pending_send_len && conn->clear_to_send) {
                ensure(conn->active);
                num_pending_conns++;
            }
        }

        if (num_pending_conns) {
            size_t avail = ut->buf_cap - ut->buf_len
                - num_pending_conns
                    * (sizeof(struct usbducks_pkthdr)
                       + sizeof(struct usbducks_pkttrailer));
            ensure(avail < ut->buf_cap);
            size_t used = 0;
            // try to split evenly
            for (size_t i = 0; i < USBDUCKS_MAX_CONNECTIONS; i++) {
                struct usbducks_connection *conn = &ud->connections[i];
                conn->len_to_send_tmp
                    = min(min(conn->pending_send_len, conn->clear_to_send),
                          avail / num_pending_conns);
                used += conn->len_to_send_tmp;
            }
            // and add any slack
            for (size_t i = 0; i < USBDUCKS_MAX_CONNECTIONS; i++) {
                struct usbducks_connection *conn = &ud->connections[i];
                size_t extra = min(avail - used,
                                   min(conn->pending_send_len, conn->clear_to_send)
                                       - conn->len_to_send_tmp);
                used += extra;
                conn->len_to_send_tmp += extra;
            }
            ensure(used <= avail);
            for (size_t i = 0; i < USBDUCKS_MAX_CONNECTIONS; i++) {
                struct usbducks_connection *conn = &ud->connections[i];
                if (conn->len_to_send_tmp) {
                    void *body = usbducks_add_packet(ud, ut, USBDUCKS_PKTTYPE_DATA,
                                                     flip_conn_id(conn->conn_id),
                                                     conn->len_to_send_tmp);
                    memcpy(body, conn->pending_send_buf, conn->len_to_send_tmp);
                }
            }
        }
    }

no_more:
#if 0
    {
        // xxxxxxx
        size_t wtf = sizeof(struct usbducks_pkthdr) + sizeof(struct usbducks_pkttrailer);
        if (ut->buf_len && ut->buf_cap - ut->buf_len >= wtf) {
            size_t remaining = ut->buf_cap - ut->buf_len - wtf;
            void *buf = usbducks_add_packet(ud, ut, USBDUCKS_PKTTYPE_IGNORE, 0, remaining);
            memset(buf, 'X', remaining);
        }
    }
#endif

    if (ut->buf_len)
        usbducks_usbtransfer_start(ut);

    for (size_t i = 0; i < USBDUCKS_MAX_CONNECTIONS; i++) {
        struct usbducks_connection *conn = &ud->connections[i];
        if (conn->len_to_send_tmp) {
            size_t actual = conn->len_to_send_tmp;
            conn->pending_send_buf = NULL;
            conn->pending_send_len = 0;
            conn->clear_to_send -= conn->len_to_send_tmp;
            conn->len_to_send_tmp = 0;
            // this really shouldn't be null
            conn->on_send_complete(conn, actual, cur_time_us() - conn->pending_send_time);
        }
    }
}

uint64_t
usbducks_get_wait_timeout_us(struct usbducks *ud) {
    if (usbducks_get_next_usbtransfer(ud, UDD_SEND) != NULL) {
        // interval for ping
        int64_t diff = (int64_t)(cur_time_us() - ud->last_outgoing_pingpong_time);
        return (uint64_t)max((USBDUCKS_PING_INTERVAL_US + 10000) - diff, (int64_t)0);
    } else {
        return 10000;
    }
}

void
usbducks_disconnect(struct usbducks_connection *conn) {
    if (conn->active) {
        if (conn->pending_send_len)
            conn->on_send_complete(conn, 0, cur_time_us() - conn->pending_send_time);
        if (conn->on_disconnect)
            conn->on_disconnect(conn);
    }
    conn->active = false;
    conn->pending_send_buf = NULL;
    conn->pending_send_len = 0;
    conn->clear_to_send = 0;
    conn->clear_to_recv = 0;
    conn->on_send_complete = NULL;
    conn->on_clear_to_send = NULL;
    conn->on_receive = NULL;
    conn->on_disconnect = NULL;
}

void
usbducks_reset_peer(struct usbducks *ud) {
    ud->sync_state = USS_SYNC;
    ud->sync_off_into_magic = 0;
    ud->sync_muggle_len = 0;
    ud->skip_next_magic = false;

    ud->my_client_id = usbducks_random_u32();
    ud->have_peer_last_seq_num = false;
    ud->have_peer_client_id = false;
    ud->need_pong_len = 0;
    ud->have_last_outgoing_ping = false;
    ud->transfer_lists[UDD_RECV].partial_transfer_idx = (uint8_t)-1;
    uint32_t old_epoch = ud->epoch++;
    ud->has_sent_successfully = false;
    for (size_t i = 0; i < USBDUCKS_MAX_CONNECTIONS; i++) {
        if (ud->connections[i].epoch == old_epoch) {
            log("disconnecting connection %u due to epoch mismatch\n", (int)i);
            usbducks_disconnect(&ud->connections[i]);
            ud->connections[i].told_remote = false;
        }
    }
    for (size_t i = 0; i < USBDUCKS_MAX_LISTENERS; i++)
        ud->listeners[i].told_remote = false;
}

void
usbducks_init(struct usbducks *ud) {
    ensure(!ud->did_init);
    ud->did_init = true;
    usbducks_backend_init(ud);
    usbducks_backend_lock(ud);
    ud->last_outgoing_pingpong_time = cur_time_us();
    ud->next_seq_num = 0;
    ud->on_remote_listen = NULL;
    ud->epoch = 0;

    for (size_t i = 0; i < USBDUCKS_MAX_CONNECTIONS; i++) {
        struct usbducks_connection *conn = &ud->connections[i];
        conn->active = false;
        conn->told_remote = false;
        conn->ud = ud;
        conn->conn_id = (uint8_t)i;
        conn->len_to_send_tmp = 0;
    }

    for (size_t i = 0; i < USBDUCKS_MAX_LISTENERS; i++) {
        struct usbducks_listener *list = &ud->listeners[i];
        list->active = false;
        list->told_remote = false;
    }

    for (int direction = UDD_SEND; direction <= UDD_RECV; direction++) {
        struct usbducks_usbtransfer_list *list = &ud->transfer_lists[direction];
        list->pending_count = 0;
        list->first_pending_transfer_idx = (uint8_t)-1;
        list->last_pending_transfer_idx = (uint8_t)-1;
        list->partial_transfer_idx = (uint8_t)-1;
        list->bytes_xferred = 0;
    }

    ud->transfer_lists[UDD_SEND].total_count = USBDUCKS_NUM_SEND_USBTRANSFERS;
    ud->transfer_lists[UDD_SEND].transfers = ud->send_transfers;
    ud->transfer_lists[UDD_RECV].total_count = USBDUCKS_NUM_RECV_USBTRANSFERS;
    ud->transfer_lists[UDD_RECV].transfers = ud->recv_transfers;
    for (int direction = UDD_SEND; direction <= UDD_RECV; direction++) {
        struct usbducks_usbtransfer_list *list = &ud->transfer_lists[direction];
        for (size_t i = 0; i < list->total_count; i++) {
            struct usbducks_usbtransfer *ut = &list->transfers[i];
            ut->ud = ud;
            ut->direction = (enum usbducks_direction)direction;
            ut->ep
                = direction == UDD_SEND ? USBDUCKS_SEND_EP_ADDR : USBDUCKS_RECV_EP_ADDR;
            ut->buf_cap = direction == UDD_SEND ? USBDUCKS_USBTRANSFER_SEND_BUF_CAP
                                                : USBDUCKS_USBTRANSFER_RECV_BUF_CAP;
            atomic_store_explicit(&ut->state, UTS_FREE, memory_order_release);
        }
    }

    usbducks_reset_peer(ud);
    usbducks_backend_unlock(ud);
}

void
usbducks_start(struct usbducks *ud) {
    struct usbducks_backend_intf_list il;
    usbducks_backend_intf_list_init(ud, &il);
    struct usbducks_intf_list_entry ile;
    bool had_any_devices = false;
    while (usbducks_backend_intf_list_next(ud, &il, &ile)) {
        bool got_send = false, got_recv = false;
        if (ile.intf_desc->bInterfaceClass != 255)
            continue;
        had_any_devices = true;
        ensure_eq(ile.intf_desc->bInterfaceSubClass, 255);
        ensure_eq(ile.intf_desc->bNumEndpoints, 2);
        for (size_t epidx = 0; epidx < 2; epidx++) {
            const usbducks_backend_ep_desc_t *ep = ile.ep_descs[epidx];
            if (ep->bEndpointAddress == USBDUCKS_SEND_EP_ADDR)
                got_send = true;
            else if (ep->bEndpointAddress == USBDUCKS_RECV_EP_ADDR)
                got_recv = true;
        }
        ensure(got_send);
        ensure(got_recv);

        log("usbducks_start: trying to connect to %s...", ile.bus_port);

        if (!usbducks_backend_intf_list_open(ud, &il)) {
            log(" fail\n");
            continue;
        }

        log(" connected\n");
        goto connected;
    }
    usbducks_backend_intf_list_fini(ud, &il);
    if (had_any_devices)
        panic("no more usbducks devices\n");
    else
        panic("no usbducks devices");

connected:
    usbducks_backend_intf_list_fini(ud, &il);

    for (int direction = UDD_SEND; direction <= UDD_RECV; direction++) {
        struct usbducks_usbtransfer_list *list = &ud->transfer_lists[direction];
        for (size_t i = 0; i < list->total_count; i++) {
            struct usbducks_usbtransfer *ut = &list->transfers[i];
            usbducks_backend_usbtransfer_init(ut);
        }
    }

#if 0 // dump status?
    void *tmp_buf = ud->transfer_lists[UDD_SEND].transfers[0].buf;
    memset(tmp_buf, 0xee, 2);
    usbducks_backend_control_req(ud, 0xc0, 0xf1, 0, 0, tmp_buf, 2, 500);
    for (int i = 0; i < 2; i++) {
        uint8_t byte = ((uint8_t *)tmp_buf)[i];
        log("%s status: %x", i ? "remote" : "local", byte);
        if (byte & 1)
            log(" attached");
        if (byte & 2)
            log(" superspeed");
        if (byte & 4)
            log(" suspend");
        if (byte & ~7)
            log(" +unknown");
        log("\n");
    }
#endif
    usbducks_backend_start_main_loop_thread(ud);
}

struct usbducks_connection *
usbducks_connect(struct usbducks *ud, uint16_t port) {
    for (size_t i = 0; i < USBDUCKS_MAX_CONNECTIONS_PER_DIRECTION; i++) {
        struct usbducks_connection *conn = &ud->connections[i];
        if (!conn->active && !conn->told_remote) {
            conn->active = true;
            conn->port = port;
            conn->told_remote = false;
            conn->epoch = ud->epoch;
            return conn;
        }
    }
    return NULL;
}

struct usbducks_listener *
usbducks_listen(struct usbducks *ud, uint16_t port) {
    for (size_t i = 0; i < USBDUCKS_MAX_LISTENERS; i++) {
        struct usbducks_listener *list = &ud->listeners[i];
        if (!list->active && !list->told_remote) {
            list->active = true;
            list->port = port;
            list->on_incoming_conn = NULL;
            return list;
        }
    }
    return NULL;
}

// Unlike regular BSD send() in non-blocking mode, we want to avoid an extra
// copy, so buf should stay valid until it's actually ready to send.

// Must call usbducks_start_transfers_if_necessary after.

void
usbducks_send_async(struct usbducks_connection *conn, const void *buf, size_t len) {
    if (!conn->active)
        panic("send_async: sending to inactive connection");
    if (conn->pending_send_len)
        panic("send_async: should only try to queue one message at a time");
    if (len == 0) {
        conn->on_send_complete(conn, 0, 0);
        return;
    }
    conn->pending_send_buf = buf;
    conn->pending_send_len = len;
    conn->pending_send_time = cur_time_us();
}

void
usbducks_clear_to_recv(struct usbducks_connection *conn, size_t len) {
    conn->clear_to_recv += len;
}

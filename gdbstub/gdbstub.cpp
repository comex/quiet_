// TODO: name change can change may_suspend and that's totally broken
// TODO: direct recv
#define WANT_UTRIE 1
#define GDBSTUB_CPP 1
#include "containers.h"
#include "decls.h"
#include "kern_garbage.h"
#include "logging.h"
#include "ssscanf.h"
#include "types.h"
#include "loader.h"
#include "misc.h"
#include "trace.h"
#include "gdbstub.h"

BEGIN_LOCAL_DECLS

// #pragma GCC diagnostic error "-Wunused-function"
// TODO: bug report?

#define GDBSTUB_USE_USBDUCKS (1 && ENABLE_USBDUCKS)
#if GDBSTUB_USE_USBDUCKS
#include "usbducks.h"
#endif

#if DUMMY
    #define OSCreateThread_orig OSCreateThread
#else
    #define OSCreateThread_orig (ensure(did_install_gdbstub_hooks), FUNC_HOOK_TY(OSCreateThread)::orig)
#endif

#if DUMMY
struct OSContext {
    uint32_t gpr[32];
    double fpr[32];
    uint32_t cr;
    uint32_t lr;
    uint32_t ctr;
    uint32_t xer;
    uint32_t srr0;
    uint32_t srr1;
    uint32_t fpscr;
    uint32_t ex0, ex1;
};
struct target_OSThread {
    OSContext context;
    const char *name;
};
static const char *
get_thread_name(struct target_OSThread *thread) {
    return thread->name;
}
#else // DUMMY
typedef OSThread target_OSThread;
static const char *
get_thread_name(const OSThread *thread) {
    return OSGetThreadName(thread) ?: "(null)";
}
#endif


#if DUMMY
static size_t
priv_try_memcpy_in(volatile void *dst, const volatile void *src, size_t len) {
    memset((void *)dst, 'a', len);
    return len;
}
static size_t
priv_try_memcpy_out(volatile void *dst, const volatile void *src, size_t len) {
    return len;
}
#else // DUMMY
#define priv_try_memcpy_in priv_try_memcpy
#define priv_try_memcpy_out priv_try_memcpy
#endif // DUMMY


extern struct func_hook_info gdbstub_hooks_list[];
static bool did_install_gdbstub_hooks;

#define WOULDBLOCK_RET -6
#define CONN_RESET_RET -1000

enum sock_wrapper_state {
    SWS_INIT,
    SWS_WAITING,
    SWS_DONE,
    SWS_NEEDS_FREE,
};

struct sock_wrapper {
#if GDBSTUB_USE_USBDUCKS
    struct usbducks_connection *conn GUARD;

    char *recv_buf GUARD;
    size_t recv_len GUARD;
    size_t finished_recv_len GUARD;
    OSEvent *recv_event;
#else // GDBSTUB_USE_USBDUCKS
    struct atomic_u32 state;
    int sock;
    struct ioctl_select_params ioctl_params;
    void (*callback)(void *);
    void *callback_arg;
    void (*free_callback)(void *);
    void *free_callback_arg;
    #if DUMMY
        std::future<void> *future;
    #endif
#endif
};

struct async_listener_thread;

struct async_listener {
    uint16_t port;
#if GDBSTUB_USE_USBDUCKS
    struct usbducks_listener *list;
#else
    int listen_sock;
    struct sock_wrapper wrapper;
#endif
    int (*thread_func)(int _, struct async_listener_thread *alt);
    size_t ctx_size;
    void *user;
    const char *thread_name;
};

struct async_listener_thread {
    char stack[0x8000] __attribute__((aligned(16)));
    OSThread thread;
    struct sock_wrapper wrapper;
    struct async_listener *al;
    char ctx[0];
};

static inline void *
alignup(void *ptr, size_t align) {
    return (void *)(((uintptr_t)ptr + (align - 1)) & ~(align - 1));
}

static bool sock_wrapper_recv_all(struct sock_wrapper *wrapper, void *buf, size_t len);
static ssize_t sock_wrapper_recv_awkward(struct sock_wrapper *wrapper, void *buf,
                                         size_t len, OSEvent *event);

static ssize_t sock_wrapper_send_sync(struct sock_wrapper *wrapper, const void *buf,
                                      size_t len);

static void sock_wrapper_disconnect(struct sock_wrapper *wrapper);

static bool
sock_wrapper_send_all(struct sock_wrapper *wrapper, const void *data, size_t len) {
    while (len > 0) {
#if !GDBSTUB_USE_USBDUCKS
        /*
        Doesn't work properly, I blame IOS.
        if (!gdbstub_wait(gs, gs->connected_sock, WAIT_FOR_WRITE))
            continue;
        */
#endif
        ssize_t r = sock_wrapper_send_sync(wrapper, data, len);
        if (r == WOULDBLOCK_RET)
            continue;
        else if (r < 0)
            return false;
        data = (char *)data + r;
        len -= (size_t)r;
    }
    return true;
}

static SDKCALL void
async_listener_thread_deallocator(OSThread *thread, void *_) {
    struct async_listener_thread *alt
        = (struct async_listener_thread *)((char *)thread - offsetof(struct async_listener_thread, thread));
    sock_wrapper_disconnect(&alt->wrapper);
#if GDBSTUB_USE_USBDUCKS
    MEMFreeToDefaultHeap(alt);
#else
    alt->wrapper.free_callback = (void (*)(void *))MEMFreeToDefaultHeap;
    alt->wrapper.free_callback_arg = alt;
    OSMemoryBarrier();
    if (!OSCompareAndSwapAtomic(&alt->wrapper.state, SWS_WAITING, SWS_NEEDS_FREE))
        MEMFreeToDefaultHeap(alt);
#endif
}

static struct async_listener_thread *
async_listener_thread_create(struct async_listener *al) {
    struct async_listener_thread *alt
        = (struct async_listener_thread *)MEMAllocFromDefaultHeapEx(sizeof(*alt) + al->ctx_size, __alignof(*alt));
    if (!alt) {
        log("failed to alloc async_listener_thread (%s)\n", al->thread_name);
        return nullptr;
    }
    alt->al = al;
    ensure(OSCreateThread_orig(&alt->thread, (void *)al->thread_func, 0, alt,
                          alt->stack + sizeof(alt->stack), sizeof(alt->stack),
                          16, // prio
                          8)); // detach
    OSSetThreadName(&alt->thread, al->thread_name);
    OSSetThreadDeallocator(&alt->thread, async_listener_thread_deallocator);
    // caller resumes
    return alt;
}

#if GDBSTUB_USE_USBDUCKS
static void
sock_wrapper_on_receive(struct usbducks_connection *conn, const void *buf,
                        size_t len) LOCKED {
    struct sock_wrapper *wrapper = (struct sock_wrapper *)conn->user;
    ensure_op(wrapper->recv_len, >=, len);
    memcpy(wrapper->recv_buf, buf, len);
    wrapper->recv_buf += len;
    wrapper->recv_len -= len;
    wrapper->finished_recv_len += len;
    OSSignalEvent_mb(wrapper->recv_event);
}

static void
sock_wrapper_on_disconnect(struct usbducks_connection *conn) LOCKED {
    struct sock_wrapper *wrapper = (struct sock_wrapper *)conn->user;
    wrapper->conn = nullptr;
    if (wrapper->recv_len)
        OSSignalEvent_mb(wrapper->recv_event);
}

static ssize_t
sock_wrapper_send_sync(struct sock_wrapper *wrapper, const void *buf, size_t len) {
    usbducks_backend_lock(&g_usbducks);
    if (!wrapper->conn) {
        usbducks_backend_unlock(&g_usbducks);
        return -1;
    }
    return (ssize_t)usbducks_send_sync(wrapper->conn, buf, len);
}

static bool
sock_wrapper_recv_all(struct sock_wrapper *wrapper, void *buf, size_t len) {
    bool ret;
    _log(LOG_NOUSBDUCKS, "sock_wrapper_recv_all(%p, %u):\n", buf, (int)len);
    usbducks_backend_lock(&g_usbducks);
    ensure(!wrapper->finished_recv_len);
    ensure(!wrapper->recv_len);
    if (!wrapper->conn) {
        ret = false;
        goto end;
    }
    usbducks_clear_to_recv(wrapper->conn, len);
    usbducks_start_transfers_if_necessary(wrapper->conn->ud);
    wrapper->recv_buf = (char *)buf;
    wrapper->recv_len = len;
    OSEvent event;
    OSInitEvent(&event, false, true);
    wrapper->recv_event = &event;

    do {
        usbducks_backend_unlock(&g_usbducks);
        OSWaitEvent_mb(&event);
        usbducks_backend_lock(&g_usbducks);

        _log(LOG_NOUSBDUCKS, " => wrapper->conn=%p wrapper->recv_len=%u\n", wrapper->conn,
             (int)wrapper->recv_len);
        if (!wrapper->conn) {
            ret = false;
            goto end;
        }
    } while (wrapper->recv_len);

    ret = true;
    _log(LOG_NOUSBDUCKS, "sock_wrapper_recv_all done\n");
end:
    wrapper->recv_event = nullptr;
    wrapper->finished_recv_len = 0;
    usbducks_backend_unlock(&g_usbducks);
    return ret;
}

// /!\ The awkward part is that the rest of buf has to remain valid forever. /!\ //
static ssize_t
sock_wrapper_recv_awkward(struct sock_wrapper *wrapper, void *buf, size_t len,
                          OSEvent *event) {
    usbducks_backend_lock(&g_usbducks);
    _log(LOG_NOUSBDUCKS, "sock_wrapper_recv_awkward(%p, %u):\n", buf, (int)len);

    size_t arlen = wrapper->finished_recv_len;
    _log(LOG_NOUSBDUCKS, "arlen=%u\n", (int)arlen);
    if (arlen) {
        memmove(buf, wrapper->recv_buf - arlen, arlen);
        wrapper->finished_recv_len = 0;
        usbducks_backend_unlock(&g_usbducks);
        return (ssize_t)arlen;
    }
    if (!wrapper->conn) {
        usbducks_backend_unlock(&g_usbducks);
        return -1;
    }
    if (!wrapper->recv_len) {
        wrapper->recv_buf = (char *)buf;
        wrapper->recv_len = len;
        wrapper->recv_event = event;
        usbducks_clear_to_recv(wrapper->conn, len);
        usbducks_start_transfers_if_necessary(wrapper->conn->ud);
    }
    usbducks_backend_unlock(&g_usbducks);
    OSWaitEvent_mb(event);
    return WOULDBLOCK_RET;
}

static void
sock_wrapper_disconnect(struct sock_wrapper *wrapper) {
    _log(LOG_NOUSBDUCKS, "sock_wrapper_disconnect(wrapper=%p)\n", wrapper);
    usbducks_backend_lock(&g_usbducks);
    if (wrapper->conn) {
        usbducks_disconnect(wrapper->conn);
        wrapper->conn = nullptr;
    }
    usbducks_backend_unlock(&g_usbducks);
}

static void
sock_wrapper_init(struct sock_wrapper *wrapper, struct usbducks_connection *conn) {
    memclr(wrapper, sizeof(*wrapper));

    usbducks_backend_lock(&g_usbducks);
    wrapper->conn = conn;
    conn->user = wrapper;
    conn->on_receive = sock_wrapper_on_receive;
    conn->on_disconnect = sock_wrapper_on_disconnect;
    usbducks_backend_unlock(&g_usbducks);
}

static void
async_listener_on_incoming_conn(struct usbducks_listener *list,
                                struct usbducks_connection *conn) LOCKED {
    struct async_listener *al = (struct async_listener *)list->user;
    struct async_listener_thread *alt = async_listener_thread_create(al);
    _log(LOG_NOUSBDUCKS, "%s> async_listener_on_incoming_conn(%p, %p) alt=%p tn=%s\n",
         get_thread_name(OSGetCurrentThread()), list, conn, alt,
         alt->al->thread_name);
    if (!alt) {
        usbducks_disconnect(conn);
    } else {
        sock_wrapper_init(&alt->wrapper, conn);
        _ensure(LOG_NOUSBDUCKS, OSResumeThread(&alt->thread));
    }
}

#else // GDBSTUB_USE_USBDUCKS
static SDKCALL void sock_wrapper_ioctl_cb(int ret, void *ctx);

static void
sock_wrapper_wait_internal(struct sock_wrapper *wrapper) {
    memclr(&wrapper->ioctl_params, sizeof(wrapper->ioctl_params));
    FD_SET(wrapper->sock, &wrapper->ioctl_params.readfds);
    FD_SET(wrapper->sock, &wrapper->ioctl_params.errfds);
    wrapper->ioctl_params.nfds = wrapper->sock + 1;
    #if DUMMY
        //log("deleting wrapper %p's future...\n", wrapper);
        delete wrapper->future;
        //log("...done\n");
        wrapper->future = new std::future(std::async([=]() {
            int ret = select(wrapper->ioctl_params.nfds, &wrapper->ioctl_params.readfds,
                             &wrapper->ioctl_params.writefds, &wrapper->ioctl_params.errfds, nullptr);
            sock_wrapper_ioctl_cb(ret, wrapper);
        }));
    #else
        int ret;
        if ((ret
             = IOS_IoctlAsync(get_socket_rm_fd(), 0x27, &wrapper->ioctl_params,
                              sizeof(wrapper->ioctl_params), &wrapper->ioctl_params,
                              sizeof(wrapper->ioctl_params), sock_wrapper_ioctl_cb, wrapper)))
            panic("IOS_IoctlAsync: %d", ret);
    #endif
    // log("did call IOS_IoctlAsync\n");
}

static SDKCALL void
sock_wrapper_ioctl_cb(int ret, void *ctx) {
    struct sock_wrapper *wrapper = (struct sock_wrapper *)ctx;
    if (GDBSTUB_VERBOSE >= 2) {
    #if !DUMMY
        log("sock_wrapper_ioctl_cb: ret=%d wrapper=%p fdsets={%#x, %#x, %#x}\n", ret, wrapper, wrapper->ioctl_params.readfds, wrapper->ioctl_params.writefds, wrapper->ioctl_params.errfds);
    #else
        log("sock_wrapper_ioctl_cb: ret=%d wrapper=%p state=%d\n", ret, wrapper, load_acquire_atomic_u32(&wrapper->state));
    #endif
    }
    // log("I am on %s lr=%p\n", OSGetThreadName(OSGetCurrentThread()),
    // __builtin_return_address(0));
    if (ret) {
        // done
    retry:;
        uint32_t state = load_acquire_atomic_u32(&wrapper->state);
        switch (state) {
        case SWS_INIT:
            break;
        case SWS_WAITING:
            if (!OSCompareAndSwapAtomic(&wrapper->state, SWS_WAITING, SWS_DONE))
                goto retry;
            // starting at this point, wait could be called again
            wrapper->callback(wrapper->callback_arg);
            break;
        case SWS_DONE:
        default:
            panic("sock_wrapper_ioctl_cb: shouldn't be in state %d", state);
            break;
        case SWS_NEEDS_FREE:
            wrapper->free_callback(wrapper->free_callback_arg);
            break;
        }
    } else {
        sock_wrapper_wait_internal(wrapper);
    }
}

static bool
sock_wrapper_wait(struct sock_wrapper *wrapper, void (*callback)(void *),
                  void *callback_arg) {
    uint32_t state = load_acquire_atomic_u32(&wrapper->state);
    if (state == SWS_INIT) {
        store_release_atomic_u32(&wrapper->state, SWS_WAITING);
        wrapper->callback = callback;
        wrapper->callback_arg = callback_arg;
        sock_wrapper_wait_internal(wrapper);
        return false;
    } else {
        ensure(wrapper->callback == callback);
        ensure(wrapper->callback_arg == callback_arg);
        return state == SWS_DONE;
    }
}

static bool
sock_wrapper_check_and_reset(struct sock_wrapper *wrapper) {
    uint32_t state = load_acquire_atomic_u32(&wrapper->state);
    switch (state) {
    case SWS_INIT:
    case SWS_NEEDS_FREE:
    default:
        panic("sock_wrapper_check_and_reset: shouldn't be in state %d", state);
    case SWS_WAITING:
        return false;
    case SWS_DONE:
        store_release_atomic_u32(&wrapper->state, SWS_INIT);
        return true;
    }
}

static ssize_t
sock_wrapper_recv(struct sock_wrapper *wrapper, void *buf, size_t len) {
    if (wrapper->sock == -1)
        return -1;
    ssize_t ret = recv(wrapper->sock, buf, len, 0);
    if (ret < 0) {
        ret = -socketlasterr();
        ensure(ret < 0);
    }
    if (ret == 0 && len != 0) {
        ret = CONN_RESET_RET;
    }
    //log("recv(%p, %zu) -> %zd\n", buf, len, ret);
    return ret;
}

static ssize_t
sock_wrapper_recv_awkward(struct sock_wrapper *wrapper, void *buf, size_t len,
                          OSEvent *event) {
    if (!sock_wrapper_wait(wrapper, (void (*)(void *))OSSignalEvent_mb, event))
        OSWaitEvent_mb(event);
    bool ready = sock_wrapper_check_and_reset(wrapper);
    //log("sock_wrapper_recv_awkward: ready(%p)=%d\n", wrapper, ready);
    if (!ready)
        return WOULDBLOCK_RET;
    ssize_t ret = sock_wrapper_recv(wrapper, buf, len);
    //log("sock_wrapper_recv_awkward: ret %zd\n", ret);
    return ret;
}

static bool
sock_wrapper_recv_all(struct sock_wrapper *wrapper, void *data, size_t len) {
    OSEvent event;
    OSInitEvent(&event, false, true);
    while (len > 0) {
        ssize_t r = sock_wrapper_recv_awkward(wrapper, data, len, &event);
        if (r == WOULDBLOCK_RET)
            continue;
        else if (r < 0)
            return false;
        data = (char *)data + r;
        len -= (size_t)r;
    }
    return true;
}

static ssize_t
sock_wrapper_send_sync(struct sock_wrapper *wrapper, const void *buf, size_t len) {
    // not actually sync, xxx
    if (wrapper->sock == -1)
        return -1;
    ssize_t ret = send(wrapper->sock, buf, len, 0);
    if (ret < 0) {
        ret = -socketlasterr();
        ensure(ret < 0);
    }
    return ret;
}

static void
sock_wrapper_disconnect(struct sock_wrapper *wrapper) {
    if (wrapper->sock != -1)
        socketclose(wrapper->sock);
    wrapper->sock = -1;
    log("sock_wrapper_disconnect\n");
#if DUMMY
    delete wrapper->future;
    wrapper->future = nullptr;
#endif
}

static void
sock_wrapper_init(struct sock_wrapper *wrapper, int sock) {
    memclr(wrapper, sizeof(*wrapper));
    store_release_atomic_u32(&wrapper->state, SWS_INIT);
    wrapper->sock = sock;
}

static bool
set_sock_nonblock(int sock) {
    int ret;
    #if DUMMY
        if ((ret = fcntl(sock, F_SETFL, O_NONBLOCK))) {
            log("fcntl(F_SETFL O_NONBLOCK): %d [%d]\n", ret, socketlasterr());
            return false;
        }
    #else
        int one = 1;
        if ((ret = setsockopt(sock, SOL_SOCKET, SO_NBIO, &one, sizeof(one)))) {
            log("setsockopt(SO_NBIO): %d [%d]\n", ret, socketlasterr());
            return false;
        }
    #endif
    return true;
}

static int
make_listen_sock(uint16_t port) {
    socket_lib_init_wrapper();
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        panic("socket() [listen]: %d [%d]", sock, socketlasterr());

    int ret;
    #if DUMMY
    int one = 1;
    if ((ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))))
        panic("setsockopt(SO_REUSEADDR): %d [%d]\n", ret, socketlasterr());
    #endif

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    if ((ret = bind(sock, (struct sockaddr *)&sa, sizeof(sa))))
        panic("bind(): %d [%d]", ret, socketlasterr());

    if ((ret = listen(sock, 5)))
        panic("listen(): %d [%d]", ret, socketlasterr());

    ensure(set_sock_nonblock(sock));

    return sock;
}

static void
async_listener_cb(void *_al) {
    struct async_listener *al = (struct async_listener *)_al;
    int connected_sock;
    struct sockaddr_in client_sa;
    socklen_t client_sa_len = sizeof(client_sa);
    ensure(sock_wrapper_check_and_reset(&al->wrapper));
    if (0 >= (connected_sock = accept(al->listen_sock, (struct sockaddr *)&client_sa, &client_sa_len))) {
        if (connected_sock == WOULDBLOCK_RET)
            log("accept() (async_listener port %d) failed: %d [%d]\n", al->port,
                connected_sock, socketlasterr());
        goto end;
    }
    log("accepted sock=%d (async_listener port %d)\n", connected_sock, al->port);
    ensure(set_sock_nonblock(connected_sock));
    struct async_listener_thread *alt;
    alt = async_listener_thread_create(al);
    if (!alt) {
        socketclose(connected_sock);
    } else {
        sock_wrapper_init(&alt->wrapper, connected_sock);
        //log("OSResumeThread====>\n");
        //log_flush();
        ensure(OSResumeThread(&alt->thread));
        //log("OSResumeThread out\n");
        //log_flush();
    }
end:
    sock_wrapper_wait(&al->wrapper, async_listener_cb, al);
}
#endif // GDBSTUB_USE_USBDUCKS

static void
async_listener_start(struct async_listener *al, uint16_t port, const char *thread_name,
                     int (*thread_func)(int _, struct async_listener_thread *alt),
                     size_t ctx_size, size_t ctx_align, void *user) {
    al->port = port;
    al->thread_func = thread_func;
    al->ctx_size = ctx_size + ctx_align - 1;
    al->user = user;
    al->thread_name = thread_name;
#if GDBSTUB_USE_USBDUCKS
    usbducks_backend_lock(&g_usbducks);
    struct usbducks_listener *list = usbducks_listen(&g_usbducks, port);
    ensure(list);
    al->list = list;
    list->on_incoming_conn = async_listener_on_incoming_conn;
    list->user = al;
    usbducks_backend_unlock(&g_usbducks);
#else
    al->listen_sock = make_listen_sock(port);
    sock_wrapper_init(&al->wrapper, al->listen_sock);
    sock_wrapper_wait(&al->wrapper, async_listener_cb, al);
#endif
}

static const char target_xml[] =
    /*
The following applies just to the XML file:
<!-- Copyright (C) 2007-2016 Free Software Foundation, Inc.

     Copying and distribution of this file, with or without modification,
     are permitted in any medium without royalty provided the copyright
     notice and this notice are preserved.  -->
*/
    "<target version=\"1.0\">\n"
    "<architecture>powerpc:common</architecture>\n"
    "<feature name=\"org.gnu.gdb.power.core\">\n"
    "<reg name=\"r0\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r1\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r2\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r3\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r4\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r5\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r6\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r7\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r8\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r9\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r10\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r11\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r12\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r13\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r14\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r15\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r16\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r17\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r18\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r19\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r20\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r21\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r22\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r23\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r24\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r25\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r26\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r27\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r28\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r29\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r30\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"r31\" bitsize=\"32\" type=\"uint32\"/>\n"

    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\" regnum=\"64\"/>\n"
    "<reg name=\"msr\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"cr\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"lr\" bitsize=\"32\" type=\"code_ptr\"/>\n"
    "<reg name=\"ctr\" bitsize=\"32\" type=\"uint32\"/>\n"
    "<reg name=\"xer\" bitsize=\"32\" type=\"uint32\"/>\n"
    "</feature>\n"

    "<feature name=\"org.gnu.gdb.power.fpu\">\n"
    "<reg name=\"f0\" bitsize=\"64\" type=\"ieee_double\" regnum=\"32\"/>\n"
    "<reg name=\"f1\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f2\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f3\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f4\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f5\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f6\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f7\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f8\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f9\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f10\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f11\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f12\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f13\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f14\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f15\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f16\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f17\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f18\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f19\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f20\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f21\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f22\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f23\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f24\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f25\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f26\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f27\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f28\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f29\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f30\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "<reg name=\"f31\" bitsize=\"64\" type=\"ieee_double\"/>\n"

    "<reg name=\"fpscr\" bitsize=\"32\" group=\"float\" regnum=\"70\"/>\n"
    "</feature>\n"

    "</target>";

enum {
    PPC_LINUX_SIGINT = 2,
    PPC_LINUX_SIGILL = 4,
    PPC_LINUX_SIGTRAP = 5,
    PPC_LINUX_SIGSEGV = 11
};

enum gdbstub_bp_type {
    GDBSTUB_BP_TYPE_SW,
    GDBSTUB_BP_TYPE_HW,
    GDBSTUB_BP_TYPE_WATCH_W,
    GDBSTUB_BP_TYPE_WATCH_R,
    GDBSTUB_BP_TYPE_WATCH_RW,
    GDBSTUB_BP_TYPE_MAX = GDBSTUB_BP_TYPE_WATCH_RW,
};

enum gdbstub_bp_action { GDBSTUB_BP_SET, GDBSTUB_BP_CLEAR };

#define GDBSTUB_MAX_REQUEST_LEN_ENCODED ((size_t)0x4000)
#define GDBSTUB_MAX_REQUEST_LEN_RAW ((GDBSTUB_MAX_REQUEST_LEN_ENCODED - 5) / 2)

#define GDBSTUB_MAX_RESPONSE_LEN_ENCODED ((size_t)0x4000)
#define GDBSTUB_MAX_RESPONSE_LEN_RAW ((size_t)((GDBSTUB_MAX_RESPONSE_LEN_ENCODED - 5) / 2))
#define GDBSTUB_MAX_MEMRW_LEN ((size_t)((GDBSTUB_MAX_RESPONSE_LEN_RAW - 1) / 2))

enum gdbstub_thread_state {
    TS_RUNNING,
    TS_SUSPENDED_NEW,
    TS_SUSPENDED_FOR_AUTOCONT,
    TS_SUSPENDED,
    TS_DSI,
    TS_ISI,
    TS_PROGRAM,
    TS_IAB, // hardware break
    TS_DEAD,
    TS_TRACE, // single step (should be last)
};

static const char *gdbstub_thread_state_names[] = {
    "TS_RUNNING", "TS_SUSPENDED_NEW", "TS_SUSPENDED_FOR_AUTOCONT",
    "TS_SUSPENDED", "TS_DSI",   "TS_ISI", "TS_PROGRAM", "TS_IAB",
    "TS_DEAD",      "TS_TRACE",
};

static const char *
gdbstub_thread_state_name(uint32_t state) {
    return state > TS_TRACE ? "TS_!INVALID!" : gdbstub_thread_state_names[state];
}

struct gdbstub_thread {
    struct _utrie_leaf leaf;
    #if DUMMY
    target_OSThread *thread;
    #else
    OSThread *thread;
    #endif
    struct gdbstub_thread *next_nascent_thread;
    size_t id;
    struct atomic_u32 exited;

    // write locked by garbo_lock; value of type gdbstub_thread_state
    struct atomic_u32 lowlevel_state;
    // locked by garbo_lock
    uint32_t my_suspend_count;
    bool pc_munged;
    bool sent_stopped_packet_since_continue;
    bool checked_conds_actions_since_continue;
    bool may_suspend;
};

struct gdbstub_bp {
    union {
        struct _utrie_leaf leaf;
        uint32_t addr;
    };
    enum gdbstub_bp_type type;
    uint8_t size;
    bool staged;
    uint32_t orig_insn;
    uarray<struct gdbstub_agent_expr *> cond_exprs, action_exprs;
};

typedef void (*xfer_generator_t)(struct gdbstub *gs, char *buf, size_t *sizep, bool init,
                                 bool *donep);

struct gdbstub : gdbstub_base {
    OSThread *handler_thread;
    OSThread run_queue_dummy_thread;
    char run_queue_dummy_thread_stack[0x1000] __attribute__((aligned(16)));
    bool dummy_allowed_to_be_active; // just a double check

    OSEvent wakeup_thread_event;
    OSMutex everything_mutex;

    struct atomic_u32 initialized;

    struct atomic_u32 garbo_lock;
    void *last_garbo_locker;

    // protected by garbo lock:
    struct club_heap threads_heap;
    utrie<struct gdbstub_thread> threads_trie;
    struct gdbstub_thread *first_nascent_thread;

    // these are owned by the gdbstub thread:
    uarray<uintptr_t> threads_by_id;
    uintptr_t next_thread_id;

    size_t thread_info_next_id;
    size_t vstopped_next_id;
    size_t xferthreads_next_id;
    // struct gdbstub_thread *last_continued_thread; <-- is this useful?
    struct gdbstub_thread *cur_thread_c;
    struct gdbstub_thread *cur_thread_g;

    struct club_heap sw_bps_heap;
    utrie<struct gdbstub_bp> sw_bps_trie;

    bool dabr_bp_inuse;
    struct gdbstub_bp dabr_bp;
    bool iabr_bp_inuse;
    struct gdbstub_bp iabr_bp;

    size_t num_bps_with_exprs;

    char response_buf[GDBSTUB_MAX_RESPONSE_LEN_RAW];
    char escaped_response_buf[GDBSTUB_MAX_RESPONSE_LEN_ENCODED];
    char memrw_raw_buf[GDBSTUB_MAX_MEMRW_LEN] __attribute__((aligned(0x40)));
    char cmd_buf[GDBSTUB_MAX_REQUEST_LEN_RAW];

    struct sock_wrapper *client_wrapper;
    char recv_buf[4096];
    size_t recv_buf_off;
    size_t recv_buf_size;

    struct atomic_u32 have_client;

    // modes that the client can toggle
    bool non_stop;
    bool no_ack;
    bool thread_events;

    bool allstop_is_stopped;

    xfer_generator_t cur_xfer_generator;
    size_t cur_xfer_off;

    struct async_listener handler_async_listener;
    struct async_listener dumper_async_listener;
    struct async_listener memrw_async_listener;

    struct gdbstub_trace_global_ctx trace_global;

    struct rpl_info *qxfer_libraries_rpl_info;
    size_t qxfer_libraries_rpl_info_idx;
    size_t qxfer_libraries_rpl_info_count;
};

static manually_construct<struct gdbstub> the_gdbstub;

enum did_send_reply {
    DIDNT_SEND_SYNC_REPLY,
    SENT_SYNC_REPLY,
};

static void gdbstub_handle_all_stopped_threads(struct gdbstub *gs);
static void gdbstub_thread_remove(struct gdbstub *gs, struct gdbstub_thread *gthread);
UNUSED static struct gdbstub *get_gdbstub(void);

struct gdbstub_thread_dbgname_buf { char buf[128]; };
#define gdbstub_thread_dbgname(gs, gthread)                                              \
    gdbstub_thread_dbgname_(gs, gthread, gdbstub_thread_dbgname_buf{})
static const char *gdbstub_thread_dbgname_(struct gdbstub *gs,
                                           const struct gdbstub_thread *gthread,
                                           struct gdbstub_thread_dbgname_buf &&buf);
static struct gdbstub_thread *gdbstub_find_thread_by_id(struct gdbstub *gs,
                                                        size_t id);
UNUSED static struct gdbstub_thread *gdbstub_find_thread_locked(struct gdbstub *gs,
                                                         target_OSThread *thread);
static struct gdbstub_thread *gdbstub_find_or_create_thread_locked(struct gdbstub *gs,
                                                                   target_OSThread *thread,
                                                                   bool or_create);

static inline auto gdbstub_iter_known_threads(struct gdbstub *gs) {
    size_t id = 0;
    return make_iterable([=]() mutable -> maybe<struct gdbstub_thread *> {
        while (1) {
            //COSError(2, "gik [%p/%p]: i=%u count=%u\n", OSGetCurrentThread(), __builtin_return_address(0), (int)id, (int)gs->threads_by_id.count());
            if (id >= gs->threads_by_id.count())
                return nothing;
            uintptr_t maybe_gthread = gs->threads_by_id[id++];
            if (maybe_gthread & 1)
                continue;
            // STUPID:
            if (!maybe_gthread)
                panic("*%p = 0 (id=%zu)\n", &gs->threads_by_id[id], id - 1);
            return just((struct gdbstub_thread *)maybe_gthread);
        }
    });
}

#if !DUMMY
// may be called from exception context
static bool
gdbstub_may_suspend_thread(struct gdbstub *gs, OSThread *thread) {
    const char *name = get_thread_name(thread);
    return ( //
               memcmp(name, "{SYS", 4) && //
               strcmp(name, "TCLInterrupt") && memcmp(name, "gdbstub", 7));
}
#endif

static struct gdbstub_thread *
gdbstub_get_any_suspendable_thread(struct gdbstub *gs) {
    for (struct gdbstub_thread *gthread : gdbstub_iter_known_threads(gs)) {
        if (gthread->may_suspend)
            return gthread;
    }
    panic("gdbstub_get_any_suspendable_thread: no threads?");
}

static struct gdbstub_thread *
gdbstub_find_next_thread_by_vcont_id(struct gdbstub *gs, unsigned int id,
                                     struct gdbstub_thread *last) {
    if (id == -1u) {
        size_t real_id = last ? last->id : 0;
        while (1) {
            if (real_id >= gs->threads_by_id.count())
                return nullptr;
            struct gdbstub_thread *gthread = gdbstub_find_thread_by_id(gs, real_id++);
            if (!gthread || !gthread->may_suspend)
                continue;
            return gthread;
        }
    } else if (id == 0) {
        return last ? nullptr : gs->cur_thread_g; /*gdbstub_get_any_suspendable_thread(gs);*/
        // ^- gdb likes to send Hc0 for some reason... before stepping on (from
        // the user's perspective) a different thread!
    } else {
        return last ? nullptr : gdbstub_find_thread_by_id(gs, id);
    }
}

static void gdbstub_lowlevel_suspend(struct gdbstub *gs, struct gdbstub_thread *gthread,
                                     enum gdbstub_thread_state target_state);

// these suck and may not be necessary, but are used for simplicity while
// allowing calls from exception context...

static int
gdbstub_garbo_lock(struct gdbstub *gs) {
    OSThread *thread = OSGetCurrentThread();
    ensure(thread);
    while (1) {
        if (load_acquire_atomic_u32(&gs->garbo_lock) == (uint32_t)(uintptr_t)thread)
            panic("double garbo lock (last from %p)", gs->last_garbo_locker);
        #if DUMMY
        int old = 1;
        #else
        int old = OSDisableInterrupts();
        #endif
        if (!OSCompareAndSwapAtomic(&gs->garbo_lock, 0, (uint32_t)(uintptr_t)thread)) {
            #if !DUMMY
            OSRestoreInterrupts(old);
            #endif
            continue;
        }
        gs->last_garbo_locker = __builtin_return_address(0);
        OSMemoryBarrier();
        return old;
    }
}
static void
gdbstub_garbo_unlock(struct gdbstub *gs, int old) {
    if (load_acquire_atomic_u32(&gs->garbo_lock) != (uint32_t)(uintptr_t)OSGetCurrentThread())
        panic("bad garbo unlock (lock from %p)", gs->last_garbo_locker);
    store_release_atomic_u32(&gs->garbo_lock, 0);
    gs->last_garbo_locker = nullptr;
    #if !DUMMY
    OSRestoreInterrupts(old);
    #endif
}

#if !DUMMY
static bool
gdbstub_exc_handler_main(OSContext *context, uint32_t type) {
    OSThread *thread = OSGetCurrentThread();
    struct gdbstub *gs = get_gdbstub();
    // log("context=%p\n", context);
    context->exception_type = type;
    // return panic_exc_handler(context);
    if (context != &thread->context) {
        // log("panicking because context(%p) != &thread->context(%p)\n", context,
        // &thread->context);
        return panic_exc_handler(context);
    }
    if (!(context->srr1 & (1 << 15))) {
        // log("noints (srr1=%x)\n", context->srr1);
        return panic_exc_handler(context);
    }

    if (!gs //
        || !load_acquire_atomic_u32(&gs->have_client)
        || !gdbstub_may_suspend_thread(gs, thread)
        || load_acquire_atomic_u32(&gs->garbo_lock) == (uint32_t)thread) {
        // log("weird\n");
        return panic_exc_handler(context);
    }

    // crash if we accidentally return here before expected
    context->srr0 ^= 0x80000000;

    // lol
    // context->srr0 = (uint32_t)OSFatal;
    // context->gpr[3] = (uint32_t)"very cute";

    // log("@%u\n", type);
    enum gdbstub_thread_state new_state;
    switch (type) {
    case DSI:
        if (thread->context.ex0 == 1) {
            // fake DSI is actually trace
            new_state = TS_TRACE;
            thread->context.srr1 &= ~(1u << 10);
        } else if (thread->context.ex0 == 2) {
            // fake DSI is actually hardware break
            new_state = TS_IAB;
        } else {
            new_state = TS_DSI;
        }
        break;
    case ISI:
        new_state = TS_ISI;
        break;
    case PROGRAM:
        new_state = TS_PROGRAM;
        break;
    default:
        panic("unexpected exception type %d", type);
    }

    // Initially I thought both OSSuspendThread and OSSignalEvent might
    // reschedule us, and since 'context' is marked as an exception context, it
    // won't be overwritten when switching out, so either of them could just
    // never return (in favor of going back to the original code).
    // But actually, the function that calls exception handlers disables the
    // scheduler (same variable as __OSDisableScheduler), so it should be safe.

    // Suspending is necessary both to stop us from executing, and to ensure
    // that an OSSuspendThread from the gdbstub thread doesn't hang.

    __OSLockScheduler(thread);
    bool suspend_ok = thread->state == 1 || thread->state == 2;
    if (suspend_ok) {
        thread->pending_suspend_count++;
        __OSSuspendThreadNolock(thread);
    }
    __OSUnlockScheduler(thread);
    ensure(suspend_ok);

    int old = gdbstub_garbo_lock(gs);
    struct gdbstub_thread *gthread = gdbstub_find_or_create_thread_locked(gs, thread, true);
    if (gthread) {
        gthread->my_suspend_count++;
        gthread->pc_munged = true;
        uint32_t state = load_acquire_atomic_u32(&gthread->lowlevel_state);
        if (state != TS_RUNNING && state != TS_SUSPENDED && state != TS_SUSPENDED_NEW) {
            gdbstub_garbo_unlock(gs, old);
            // return panic_exc_handler(context);
            panic("gdbstub_exc_handler: gthread in unexpected state %d:\n%p,%d srr0=%p", state,
                  gthread, new_state, thread->context.srr0);
        }
        store_release_atomic_u32(&gthread->lowlevel_state, new_state);
    }
    gdbstub_garbo_unlock(gs, old);
    if (!gthread)
        panic("gdbstub_exc_handler: couldn't find/create?");

    // log("@ signaling due to pending exc\n");
    OSSignalEvent(&gs->wakeup_thread_event);
    // uint32_t *cns = (void *)0x10043D6C;
    // log("@%d %d %d %d\n", cns[0], cns[1], cns[2], OSGetCoreId());
    return true;
}

static SDKCALL bool
gdbstub_exc_handler_dsi(OSContext *context) {
    return gdbstub_exc_handler_main(context, DSI);
}
static SDKCALL bool
gdbstub_exc_handler_isi(OSContext *context) {
    return gdbstub_exc_handler_main(context, ISI);
}
static SDKCALL bool
gdbstub_exc_handler_program(OSContext *context) {
    return gdbstub_exc_handler_main(context, PROGRAM);
}

template<> SDKCALL void
FUNC_HOOK_TY(OSExitThread)::hook(int ret) {
    struct gdbstub *gs = get_gdbstub();
    if (gs) {
        OSThread *thread = OSGetCurrentThread();
        // log("hook_OSExitThread thread=%p ret=%d from %p,%p; waiting for mutex
        // owner=%p\n", thread, ret, __builtin_return_address(0),
        // __builtin_return_address(1), gs->everything_mutex.owner);
        if (gdbstub_may_suspend_thread(gs, thread)) {
            int old = gdbstub_garbo_lock(gs);
            struct gdbstub_thread *gthread = gdbstub_find_thread_locked(gs, thread);
            if (gthread) {
                store_release_atomic_u32(&gthread->lowlevel_state, TS_DEAD);
                gs->threads_trie.erase(gthread);
                gthread->thread = (OSThread *)0xdead0000;
            }
            gdbstub_garbo_unlock(gs, old);
            if (gthread) {
                // log("@ signaling due to exited thread [%s]\n",
                // gdbstub_thread_dbgname(gs, gthread));
                OSSignalEvent(&gs->wakeup_thread_event);
            } else {
                // log("@ we haven't seen the thread before anyway\n");
            }
            // log("hook_OSExitThread(%p) done with mutex\n", thread);
        }
    }
    orig(ret);
}

static void
note_new_thread(target_OSThread *thread) {
    struct gdbstub *gs = get_gdbstub();
    if (!gs || !load_acquire_atomic_u32(&gs->have_client))
        return;
    int old = gdbstub_garbo_lock(gs);
    struct gdbstub_thread *gthread = gdbstub_find_or_create_thread_locked(gs, thread, true);
    gdbstub_garbo_unlock(gs, old);
    #if !DUMMY
    ensure(thread->suspend_count);
    #endif
    if (!gthread)
        panic("note_new_thread: couldn't create?");
    if (gthread->may_suspend) {
        gdbstub_lowlevel_suspend(gs, gthread, TS_SUSPENDED_NEW);
        OSSignalEvent(&gs->wakeup_thread_event);
        log("@ signaling due to new thread [%s] thanks to %p,%p\n",
            gdbstub_thread_dbgname(gs, gthread), __builtin_return_address(1),
            __builtin_return_address(2));
    }
}

template<> SDKCALL int
FUNC_HOOK_TY(OSCreateThread)::hook(OSThread *thread, void *entry, int arg, void *arg2, void *stack,
                    int stack_size, int prio, int attr) {
    int ret
        = orig(thread, entry, arg, arg2, stack, stack_size, prio, attr);
    if (ret)
        note_new_thread(thread);
    return ret;
}
template<> SDKCALL int
FUNC_HOOK_TY(OSCreateThreadType)::hook(OSThread *thread, void *entry, int arg, void *arg2, void *stack,
                        int stack_size, int prio, int attr, int type) {
    int ret = orig(thread, entry, arg, arg2, stack, stack_size, prio,
                                      attr, type);
    if (ret)
        note_new_thread(thread);
    return ret;
}
template<> SDKCALL int
FUNC_HOOK_TY(__OSCreateThreadType)::hook(OSThread *thread, void *entry, int arg, void *arg2, void *stack,
                          int stack_size, int prio, int attr, int type) {
    int ret = orig(thread, entry, arg, arg2, stack, stack_size, prio,
                                        attr, type);
    if (ret)
        note_new_thread(thread);
    return ret;
}
#endif

static bool
gdbstub_send_all(struct gdbstub *gs, const char *data, size_t len) {
    return sock_wrapper_send_all(gs->client_wrapper, data, len);
}

static void gdbstub_send_response_type(struct gdbstub *gs, const char *respbuf,
                                       size_t resplen, char type) { // type = '$' or '%'
    if (GDBSTUB_VERBOSE >= 2)
        log("gdbstub_send_response_type: %c[%.*s]\n", type, (int)resplen, respbuf);
    char *escaped_buf = gs->escaped_response_buf;
    size_t escaped_off = 0;
    escaped_buf[escaped_off++] = type;
    for (size_t i = 0; i < resplen; i++) {
        char c = respbuf[i];
        switch (c) {
        case '#':
        case '$':
        case '}':
        case '*':
            escaped_buf[escaped_off++] = '}';
            escaped_buf[escaped_off++] = c ^ 0x20;
            break;
        default:
            escaped_buf[escaped_off++] = c;
            break;
        }
    }
    uint8_t checksum = 0;
    for (size_t i = 1; i < escaped_off; i++)
        checksum = (uint8_t)(checksum + escaped_buf[i]);
    escaped_buf[escaped_off++] = '#';
    snprintf(escaped_buf + escaped_off, sizeof(gs->escaped_response_buf) - escaped_off,
             "%02x", checksum);
    escaped_off += 2;
    gdbstub_send_all(gs, escaped_buf, escaped_off);
}

static void
gdbstub_send_response(struct gdbstub *gs, const char *respbuf, size_t resplen) {
    gdbstub_send_response_type(gs, respbuf, resplen, '$');
}

static void
gdbstub_send_response_str(struct gdbstub *gs, const char *respbuf) {
    gdbstub_send_response(gs, respbuf, strlen(respbuf));
}

#if !DUMMY
// The global bitmask of which (core, absprio) pairs have a nonempty queue is
// not exported.  If part1 below just removed 'thread' from the run queue,
// rather than replacing it with 'dummy', it could become empty, in which case
// the bitmask and the actual run queue would become mismatched and the
// scheduler could crash.
// We can use OSSetThreadAffinity to reach coreinit's own function for removing
// a thread from the run queue, but that requires not having the scheduler
// locked (or else we could just run it on the original thread rather than
// using 'dummy').

static bool
remove_from_run_queue_part1(struct gdbstub *gs, OSThread *thread, const char **errp) {
    if (thread->state != 1 || thread->suspend_count > 1 || (thread->attr & 0x10))
        return false;
    // the suspend for new threads shouldn't require this (and that would be
    // racey):
    if (OSGetCurrentThread() != gs->handler_thread) {
        *errp = "wrong thread?";
        return false;
    }
    OSThread *dummy = &gs->run_queue_dummy_thread;
    dummy->state = 1;
    dummy->priority = thread->priority;
    if ((dummy->attr & 7) || dummy->suspend_count) {
        *errp = "dummy was in weird state";
        return false;
    }
    gs->dummy_allowed_to_be_active = true;
    OSMemoryBarrier();
    bool need_part2 = false;
    for (int core = 0; core < 3; core++) {
        if (!(thread->attr & (1 << core)))
            continue;
        OSThread *prev = thread->run_link[core].prev;
        OSThread *next = thread->run_link[core].next;
        struct thread_link *queue = thread->run_queue[core];
        if (!queue) {
            *errp = "queue is null?";
            return false;
        }
        dummy->context.attr |= 1u << core;
        dummy->attr |= (uint8_t)(1 << core);
        if (prev)
            prev->run_link[core].next = dummy;
        else
            queue->next = dummy;
        if (next)
            next->run_link[core].prev = dummy;
        else
            queue->prev = dummy;
        dummy->run_queue[core] = queue;
        dummy->run_link[core].prev = prev;
        dummy->run_link[core].next = next;
        thread->run_queue[core] = nullptr;
        thread->run_link[core].prev = nullptr;
        thread->run_link[core].next = nullptr;
        need_part2 = true;
    }
    return need_part2;
}

static void
remove_from_run_queue_part2(struct gdbstub *gs) {
    OSThread *dummy = &gs->run_queue_dummy_thread;
    if (GDBSTUB_VERBOSE) {
        log("part2 [a=%x s=%x]:\n", dummy->attr, dummy->state);
        for (int core = 0; core < 3; core++)
            log("%d: %p, %p, %p[%p, %p]\n", core, dummy->run_link[core].prev,
                dummy->run_link[core].next, dummy->run_queue[core],
                dummy->run_queue[core] ? dummy->run_queue[core]->prev : 0,
                dummy->run_queue[core] ? dummy->run_queue[core]->next : 0);
    }
    while (!OSSetThreadAffinity(dummy, 0)) {
        // someone might have actually ran it; if the state is currently
        // running, OSSetThreadAffinity just returns false without changing
        // anything
        if (GDBSTUB_VERBOSE)
            log("state=%x\n", gs->run_queue_dummy_thread.state);
        OSYieldThread();
    }
    if (GDBSTUB_VERBOSE)
        log("set aff\n");
    ensure((gs->run_queue_dummy_thread.attr & 7) == 0);
    gs->dummy_allowed_to_be_active = false;
    ensure(gs->run_queue_dummy_thread.state == 1);
    if (gs->run_queue_dummy_thread.suspend_count)
        gs->run_queue_dummy_thread.suspend_count = 0; // no lock, lol
}

static int
run_queue_dummy_thread_func(int _, void *xgs) {
    struct gdbstub *gs = (struct gdbstub *)xgs;
    while (1) {
        ensure(gs->dummy_allowed_to_be_active);
        OSSuspendThread(OSGetCurrentThread());
    }
}

static bool
aggressively_suspend_thread(struct gdbstub *gs, struct gdbstub_thread *gthread) {
    ensure(gthread->thread != OSGetCurrentThread());
    if (GDBSTUB_VERBOSE)
        log("aggressively_suspend_thread(%s)\n", gdbstub_thread_dbgname(gs, gthread));
    uint32_t count = 0;
    bool did_bump = false;
    OSThreadQueue dummy_queue;
    OSInitThreadQueue(&dummy_queue);
    const char *err = nullptr;
    bool need_part2 = false;
    while (1) {
        bool done, ret;
        int old = gdbstub_garbo_lock(gs);
        __OSLockScheduler(OSGetCurrentThread());
        uint32_t llstate = load_acquire_atomic_u32(&gthread->lowlevel_state);
        if (llstate != TS_RUNNING) {
            done = true;
            ret = false;
            goto unlock;
        }
        OSThread *thread;
        uint8_t state;
        uint32_t psr, core, srr0;
        thread = gthread->thread;
        state = thread->state;
        psr = thread->pending_stop_reason;
        // just for debugging:
        core = thread->context.core;
        srr0 = thread->context.srr0;
        if (state == 8 || state == 0 || psr > 1) {
            done = true;
            ret = false;
        } else {
            if (!did_bump) {
                thread->pending_suspend_count++;
                did_bump = true;
            }
            psr = thread->pending_stop_reason = 1;
            if (state == 4 || state == 1) {
                thread->suspend_count += thread->pending_suspend_count;
                thread->pending_suspend_count = 0;
                thread->pending_stop_reason = 0;
                need_part2 = remove_from_run_queue_part1(gs, thread, &err);
                done = true;
                ret = true;
            } else {
                done = false;
            }
        }
    unlock:
        __OSUnlockScheduler(OSGetCurrentThread());
        gdbstub_garbo_unlock(gs, old);
        if (err)
            panic("aggressively_suspend_thread: %s", err);
        if (done) {
            if (need_part2)
                remove_from_run_queue_part2(gs);
            if (GDBSTUB_VERBOSE)
                log("aggressively_suspend_thread ret=%d [s=%x]\n", ret, state);
            return ret;
        }
        if (count++ == 1000) {
            count = 0;
            log("aggressively_suspend_thread: waiting on thread %p (%s), which was in "
                "state %d psr %d srr0 %x core %d (I'm on %d)...\n",
                thread, OSGetThreadName(thread), state, psr, srr0, core,
                OSGetCoreId());
        }
        OSWakeupThread(&dummy_queue); // to cause reschedule
    }
}
#endif // !DUMMY

static void
gdbstub_lowlevel_suspend(struct gdbstub *gs, struct gdbstub_thread *gthread,
                         enum gdbstub_thread_state target_state) {
    ensure(gthread->may_suspend);
    uint32_t llstate = load_acquire_atomic_u32(&gthread->lowlevel_state);
    /*
    (getting the name here can race with thread destruction)
    log("lowlevel_suspend: [%s] state=%s my_suspend_count=%d...\n",
        gdbstub_thread_dbgname(gs, gthread), gdbstub_thread_state_name(llstate),
        gthread->my_suspend_count);
    */
    // log("btw, PC=%x LR=%x\n", thread->context.srr0, thread->context.lr);
    #if DUMMY
        bool ret = true;
    #else
        bool ret = aggressively_suspend_thread(gs, gthread);
    #endif
    if (ret) {
        // thread->context.srr0 ^= 0x80000000;
        int old = gdbstub_garbo_lock(gs);
        gthread->my_suspend_count++;
        if (load_acquire_atomic_u32(&gthread->lowlevel_state) == TS_RUNNING)
            store_release_atomic_u32(&gthread->lowlevel_state, target_state);
        gdbstub_garbo_unlock(gs, old);
    } else if (llstate == TS_SUSPENDED_NEW) {
        store_release_atomic_u32(&gthread->lowlevel_state, target_state);
    }
}

#if DUMMY
static void
gdbstub_fake_continue_for_dummy(struct gdbstub *gs, struct gdbstub_thread *gthread) {
    gthread->thread->context.srr0 = 0x80001234;
    gthread->thread->context.srr1 = 1 << 17;
    gthread->my_suspend_count++;
    gthread->pc_munged = true;
    store_release_atomic_u32(&gthread->lowlevel_state, TS_PROGRAM);
    OSSignalEvent(&gs->wakeup_thread_event);
}
#endif

static void
gdbstub_lowlevel_continue(struct gdbstub *gs, struct gdbstub_thread *gthread) {
    uint32_t racestate = load_acquire_atomic_u32(&gthread->lowlevel_state);
    if (GDBSTUB_VERBOSE)
        log("lowlevel_continue: %s, my_suspend_count=%d state=%s pc=%llx\n",
            gdbstub_thread_dbgname(gs, gthread), gthread->my_suspend_count,
            gdbstub_thread_state_name(racestate),
            gdbstub_get_reg(gs, gthread, GDBSTUB_REG_PC));
    int old = gdbstub_garbo_lock(gs);
    uint32_t resume_count = 0;
    uint32_t state = load_acquire_atomic_u32(&gthread->lowlevel_state);
    if (state != TS_RUNNING && state != TS_DEAD) {
        if (gthread->pc_munged) {
            gthread->thread->context.srr0 ^= 0x80000000;
            gthread->pc_munged = false;
        }
        resume_count = gthread->my_suspend_count;
        gthread->my_suspend_count = 0;
        store_release_atomic_u32(&gthread->lowlevel_state, TS_RUNNING);
        #if !DUMMY
            for (uint32_t i = 0; i < resume_count; i++)
                OSResumeThread(gthread->thread);
        #else
            gdbstub_fake_continue_for_dummy(gs, gthread);
        #endif
    }
    gdbstub_garbo_unlock(gs, old);
}

// won't change from false to true under the lock
// just kidding
static bool
gdbstub_thread_is_running_or_dead(struct gdbstub *gs, struct gdbstub_thread *gthread) {
    uint32_t llstate = load_acquire_atomic_u32(&gthread->lowlevel_state);
    return llstate == TS_DEAD || llstate == TS_RUNNING;
}

static void
gdbstub_getset_reg(struct gdbstub *gs, struct gdbstub_thread *gthread,
                   enum gdbstub_reg which, uint64_t *val, bool is_set) {
    OSContext *context;
    ensure(gthread);
    if (!gthread->may_suspend) {
        *val = 0xeeeeeeeeeeeeeeee;
        return;
    }
    int old = gdbstub_garbo_lock(gs);
    if (gdbstub_thread_is_running_or_dead(gs, gthread)) {
        *val = 0xeeeeeeeeeeeeeeee;
        goto unlock;
    }
    context = &gthread->thread->context;
    if (which >= GDBSTUB_REG_FPR0 && which <= GDBSTUB_REG_FPR0 + 31) {
        // 64-bit
        // lolundef
        uint64_t *ptr = (uint64_t *)&context->fpr[which - GDBSTUB_REG_FPR0];
        if (is_set)
            *ptr = *val;
        else
            *val = *ptr;
    } else if (which == GDBSTUB_REG_PC && gthread->pc_munged) {
        // handle munging
        if (is_set)
            context->srr0 = (uint32_t)*val ^ 0x80000000;
        else
            *val = (uint64_t)context->srr0 ^ 0x80000000;
    } else {
        // otherwise, 32-bit
        uint32_t *ptr;
        switch (which) {
        case GDBSTUB_REG_GPR0 ... GDBSTUB_REG_GPR0 + 31:
            ptr = &context->gpr[which - GDBSTUB_REG_GPR0];
            break;
        case GDBSTUB_REG_MSR:
            ptr = &context->srr1;
            break;
        case GDBSTUB_REG_CR:
            ptr = &context->cr;
            break;
        case GDBSTUB_REG_LR:
            ptr = &context->lr;
            break;
        case GDBSTUB_REG_CTR:
            ptr = &context->ctr;
            break;
        case GDBSTUB_REG_XER:
            ptr = &context->xer;
            break;
        case GDBSTUB_REG_FPSCR:
            ptr = &context->fpscr;
            break;
        case GDBSTUB_REG_PC:
            ptr = &context->srr0;
            break;
        case GDBSTUB_REG_FPR0 ... GDBSTUB_REG_FPR0 + 31:
        case GDBSTUB_REG_COUNT:
        default:
            __builtin_unreachable();
        }
        if (is_set)
            *ptr = (uint32_t)*val;
        else
            *val = (uint64_t)*ptr;
    }
unlock:
    gdbstub_garbo_unlock(gs, old);
}

uint64_t
gdbstub_get_reg(struct gdbstub *gs, struct gdbstub_thread *gthread,
                enum gdbstub_reg which) {
    uint64_t val;
    gdbstub_getset_reg(gs, gthread, which, &val, false);
    return val;
}

static void
gdbstub_set_reg(struct gdbstub *gs, struct gdbstub_thread *gthread,
                enum gdbstub_reg which, uint64_t val) {
    gdbstub_getset_reg(gs, gthread, which, &val, true);
}

static void
cleanup_agent_exprs(struct gdbstub *gs, uarray<struct gdbstub_agent_expr *> *exprs) {
    for (struct gdbstub_agent_expr *ae : *exprs)
        heap_free(&gs->top_heap, ae);
    exprs->clear();
}

static bool
gdbstub_bp_stage(struct gdbstub *gs, struct gdbstub_bp *bp) {
    if (bp->staged)
        return true;
    switch (bp->type) {
    case GDBSTUB_BP_TYPE_SW: {
        size_t actual = priv_try_memcpy_in(&bp->orig_insn, (const void *)(uintptr_t)bp->addr, 4);
        uint32_t trap = 0x7fe00008;
        if (actual < 4) {
            log("failed to add SW breakpoint at 0x%x: can't read orig insn\n", bp->addr);
            return false;
        }
        actual = priv_try_memcpy_out((void *)(uintptr_t)bp->addr, &trap, 4);
        if (actual < 4) {
            log("failed to add SW breakpoint at 0x%x: can't overwrite insn\n", bp->addr);
            if (actual > 0)
                panic("partial overwrite!?");
            return false;
        }
        break;
    }
    case GDBSTUB_BP_TYPE_HW:
        log("setting IABR (%x)\n", (int)bp->addr);
        #if !DUMMY
            OSSetIABR(true, bp->addr);
        #endif
        break;
    case GDBSTUB_BP_TYPE_WATCH_W:
    case GDBSTUB_BP_TYPE_WATCH_R:
    case GDBSTUB_BP_TYPE_WATCH_RW: {
        uint32_t word = bp->addr & ~3u;
        log("setting DABR (%x)\n", (int)word);
        #if !DUMMY
            OSSetDABR(true, word, //
                      bp->type != GDBSTUB_BP_TYPE_WATCH_W,
                      bp->type != GDBSTUB_BP_TYPE_WATCH_R);
        #endif
        break;
    }
    default:
        ensure(0);
    }
    bp->staged = true;
    return true;
}

static void
gdbstub_bp_unstage(struct gdbstub *gs, struct gdbstub_bp *bp) {
    if (!bp->staged)
        return;
    switch (bp->type) {
    case GDBSTUB_BP_TYPE_SW: {
        size_t actual = priv_try_memcpy_out((void *)(uintptr_t)bp->addr, &bp->orig_insn, 4);
        if (actual < 4) {
            log("failed to remove SW breakpoint at 0x%x!\n", (int)bp->addr);
            return;
        }
        break;
    }
    case GDBSTUB_BP_TYPE_HW:
        log("clearing IABR\n");
        #if !DUMMY
            OSSetIABR(true, 0);
        #endif
        break;
    case GDBSTUB_BP_TYPE_WATCH_W:
    case GDBSTUB_BP_TYPE_WATCH_R:
    case GDBSTUB_BP_TYPE_WATCH_RW:
        log("clearing DABR\n");
        #if !DUMMY
            OSSetDABR(true, 0, false, false);
        #endif
        break;
    default:
        ensure(0);
    }
    bp->staged = false;
}

static void
gdbstub_bp_remove(struct gdbstub *gs, struct gdbstub_bp *bp) {
    gdbstub_bp_unstage(gs, bp);
    gs->num_bps_with_exprs -= (bp->cond_exprs.count() || bp->action_exprs.count());
    cleanup_agent_exprs(gs, &bp->cond_exprs);
    cleanup_agent_exprs(gs, &bp->action_exprs);
    switch (bp->type) {
    case GDBSTUB_BP_TYPE_SW:
        gs->sw_bps_trie.erase(bp);
        heap_free(&gs->sw_bps_heap, bp);
        break;
    case GDBSTUB_BP_TYPE_HW:
        gs->iabr_bp_inuse = false;
        break;
    case GDBSTUB_BP_TYPE_WATCH_W:
    case GDBSTUB_BP_TYPE_WATCH_R:
    case GDBSTUB_BP_TYPE_WATCH_RW:
        gs->dabr_bp_inuse = false;
        break;
    default:
        ensure(0);
    }
}

static bool
gdbstub_bp_add(struct gdbstub *gs, enum gdbstub_bp_type type, uint32_t addr,
               size_t size,
               uarray<struct gdbstub_agent_expr *> *cond_exprs,
               uarray<struct gdbstub_agent_expr *> *action_exprs) {
    struct gdbstub_bp *bp;
    switch (type) {
    case GDBSTUB_BP_TYPE_SW: {
        if ((addr & 3) || (size != 4))
            return false;
        bool is_new;
        bp = unwrap_or(gs->sw_bps_trie.find(addr, &is_new), {
            return false;
        });
        ensure(is_new);
        break;
    }
    case GDBSTUB_BP_TYPE_HW:
        if ((addr & 3) || (size != 4))
            return false;
        if (gs->iabr_bp_inuse) {
            log("failed to add hw breakpoint because IABR is in use\n");
            return false;
        }
        gs->iabr_bp_inuse = true;
        bp = &gs->iabr_bp;
        break;
    case GDBSTUB_BP_TYPE_WATCH_W:
    case GDBSTUB_BP_TYPE_WATCH_R:
    case GDBSTUB_BP_TYPE_WATCH_RW: {
        if (size > 4) {
            log("failed to add oversize watchpoint: addr=0x%x, size=0x%x\n", (int)addr, (int)size);
            return false;
        }
        uint32_t word = addr & ~3u;
        uint32_t lastword = (addr + size - 1) & ~3u;
        if (lastword != word) {
            log("failed to add watchpoint crossing word boundary: addr=0x%x, size=0x%x\n",
                (int)addr, (int)size);
            return false;
        }
        if (gs->dabr_bp_inuse) {
            log("failed to add watchpoint because DABR is in use\n");
            return false;
        }
        gs->dabr_bp_inuse = true;
        bp = &gs->dabr_bp;
        break;
    }
    default:
        return false;
    }
    bp->type = type;
    bp->addr = addr;
    bp->size = (uint8_t)size;
    bp->staged = false;
    bp->cond_exprs = move(*cond_exprs);
    bp->action_exprs = move(*action_exprs);
    gs->num_bps_with_exprs += (bp->cond_exprs.count() || bp->action_exprs.count());
    if (!gdbstub_bp_stage(gs, bp)) {
        gdbstub_bp_remove(gs, bp);
        return false;
    }
    return true;
}

static bool
gdbstub_bp_mod(struct gdbstub *gs, enum gdbstub_bp_type type,
               enum gdbstub_bp_action action, uint32_t addr, size_t size,
               uarray<struct gdbstub_agent_expr *> *cond_exprs,
               uarray<struct gdbstub_agent_expr *> *action_exprs) {
    bool removed = false;
    switch (type) {
    case GDBSTUB_BP_TYPE_HW:
        if (gs->iabr_bp_inuse && gs->iabr_bp.addr == addr && gs->iabr_bp.size == size) {
            gdbstub_bp_remove(gs, &gs->iabr_bp);
            removed = true;
        }
        break;
    case GDBSTUB_BP_TYPE_WATCH_W:
    case GDBSTUB_BP_TYPE_WATCH_R:
    case GDBSTUB_BP_TYPE_WATCH_RW:
        if (gs->dabr_bp_inuse && gs->dabr_bp.addr == addr && gs->dabr_bp.size == size) {
            gdbstub_bp_remove(gs, &gs->dabr_bp);
            removed = true;
        }
        break;
    case GDBSTUB_BP_TYPE_SW:
        if (maybe<struct gdbstub_bp *> mbp = gs->sw_bps_trie.find(addr, nullptr)) {
            struct gdbstub_bp *bp = mbp.unwrap();
            if (bp->size == size) {
                gdbstub_bp_remove(gs, bp);
                removed = true;
            }
        }
        break;
    }
    if (action == GDBSTUB_BP_SET)
        return gdbstub_bp_add(gs, type, addr, size, cond_exprs, action_exprs);
    else
        return removed;
}

// return value is a breakpoint which should be unstaged, before autocontinue
static struct gdbstub_bp *
gdbstub_check_bp_conds_actions(struct gdbstub *gs, struct gdbstub_thread *gthread,
                               uint32_t llstate, bool skip_actions) {
    //log("gdbstub_check_bp_conds_actions: checked=%u\n", gthread->checked_conds_actions_since_continue);
    if (gthread->checked_conds_actions_since_continue)
        return nullptr;
    if (!skip_actions)
        gthread->checked_conds_actions_since_continue = true;
    struct gdbstub_bp *bp;
    //log("gdbstub_check_bp_conds_actions: llstate=%u\n", llstate);
    switch (llstate) {
    case TS_IAB:
        if (gs->iabr_bp_inuse) {
            bp = &gs->iabr_bp;
            break;
        }
        return nullptr;
    case TS_DSI:
        if ((gthread->thread->context.ex0 & (1 << 22)) &&
            gs->dabr_bp_inuse) {
            bp = &gs->dabr_bp;
            break;
        }
        return nullptr;
    case TS_PROGRAM:
        if ((gthread->thread->context.srr1 & (1 << 17)) &&
            gs->num_bps_with_exprs != 0) {
            // breakpoint
            uint32_t pc = gthread->thread->context.srr0;
            if (gthread->pc_munged)
                pc ^= 0x80000000;
            //log("gdbstub_check_bp_conds_actions: pc=%x\n", pc);
            bp = unwrap_or(gs->sw_bps_trie.find(pc, nullptr), return nullptr);
            break;
        }
        return nullptr;
    case TS_RUNNING:
        panic("gdbstub_check_bp_conds_actions: shouldn't be running");
    default:
        return nullptr;
    }
    //log("gdbstub_check_bp_conds_actions: bp=%p\n", bp);

    for (struct gdbstub_agent_expr *expr : bp->cond_exprs) {
        uint64_t result;
        if (!gdbstub_eval_agent_expr(gs, expr, &gs->trace_global, nullptr, &result, 1, gthread)) {
            log("evaluating breakpoint condition failed\n");
            return nullptr;
        }
        if (GDBSTUB_VERBOSE)
            log("gdbstub_check_bp_conds_actions([%s]): result=%llu\n", gdbstub_thread_dbgname(gs, gthread), result);
        if (!result)
            return bp;
    }
    if (!skip_actions) {
        for (struct gdbstub_agent_expr *expr : bp->action_exprs) {
            if (GDBSTUB_VERBOSE)
                log("gdbstub_check_bp_conds_actions([%s]): evaluating action\n", gdbstub_thread_dbgname(gs, gthread));
            if (!gdbstub_eval_agent_expr(gs, expr, &gs->trace_global, nullptr, nullptr, 0, gthread)) {
                log("evaluating breakpoint action failed\n");
                return nullptr;
            }
        }
    }
    if (bp->action_exprs.count())
        return bp;
    return nullptr;
}

static bool gdbstub_thread_step(struct gdbstub *gs, struct gdbstub_thread *gthread, bool have_srr0,
                    uint32_t srr0, bool range, uint32_t start, uint32_t end);

static void
gdbstub_autocont(struct gdbstub *gs, struct gdbstub_thread *gthread, struct gdbstub_bp *bp_to_unstage) {
    if (GDBSTUB_VERBOSE)
        log("gdbstub_autocont([%s])\n", gdbstub_thread_dbgname(gs, gthread));
    for (struct gdbstub_thread *othread : gdbstub_iter_known_threads(gs)) {
        if (othread != gthread &&
            othread->may_suspend &&
            load_acquire_atomic_u32(&othread->lowlevel_state) == TS_RUNNING)
            gdbstub_lowlevel_suspend(gs, othread, TS_SUSPENDED_FOR_AUTOCONT);
    }
    uarray<struct gdbstub_bp *> unstaged_bps;
    while (1) {
        ensure(bp_to_unstage->staged);
        gdbstub_bp_unstage(gs, bp_to_unstage);
        if (!unstaged_bps.append(bp_to_unstage, &gs->top_heap)) {
            log("gdbstub_autocont: unstaged_bps list oom\n");
            break;
        }
        uint32_t llstate;
        if (!gdbstub_thread_step(gs, gthread, false, 0, false, 0, 0) ||
            (llstate = load_acquire_atomic_u32(&gthread->lowlevel_state)) == TS_TRACE) {
            break;
        }
        bp_to_unstage = gdbstub_check_bp_conds_actions(gs, gthread, llstate, /*skip_actions*/false);
        if (!bp_to_unstage) {
            // It's stopped, but it should have signaled wakeup_thread_event
            // while doing so, so we'll get another go-around.
            break;
        }
        // otherwise, keep stepping
    }
    for (struct gdbstub_bp *bp : unstaged_bps)
        gdbstub_bp_stage(gs, bp);
    for (struct gdbstub_thread *othread : gdbstub_iter_known_threads(gs)) {
        if (load_acquire_atomic_u32(&othread->lowlevel_state) == TS_SUSPENDED_FOR_AUTOCONT)
            gdbstub_lowlevel_continue(gs, othread);
    }
    gdbstub_lowlevel_continue(gs, gthread);
    if (GDBSTUB_VERBOSE)
        log("gdbstub_autocont done\n");
}

enum send_stopped_packet_mode {
    SSPM_ASYNC,
    SSPM_SYNC_SOLICITED,
    SSPM_SYNC_UNSOLICITED,
};

static bool
gdbstub_send_stopped_packet(struct gdbstub *gs, struct gdbstub_thread *gthread,
                            uint32_t llstate, enum send_stopped_packet_mode mode) {
    gthread->sent_stopped_packet_since_continue = true;
    size_t thread_id = gthread->id;

    const char *reason = nullptr;
    int signal;
    char buf[128];
    char *stop_reply = buf + 5;
    size_t stop_reply_cap = sizeof(buf) - 5;
    uint32_t reason_arg;

    if (GDBSTUB_VERBOSE) {
        if (llstate == TS_DEAD)
            log("gdbstub_send_stopped_packet([%s], state=TS_DEAD, mode=%d\n",
                gdbstub_thread_dbgname(gs, gthread), mode);
        else
            log("gdbstub_send_stopped_packet([%s], state=%s, mode=%d, srr0=%x srr1=%x ex0=%x ex1=%x attr=%x\n",
                gdbstub_thread_dbgname(gs, gthread), gdbstub_thread_state_name(llstate), mode,
                gthread->thread->context.srr0, gthread->thread->context.srr1,
                gthread->thread->context.ex0, gthread->thread->context.ex1, gthread->thread->attr);
    }

    if (struct gdbstub_bp *bp_to_unstage = gdbstub_check_bp_conds_actions(gs, gthread, llstate, /*skip_actions*/false)) {
        gdbstub_autocont(gs, gthread, bp_to_unstage);
        return false;
    }

    switch (llstate) {
    case TS_RUNNING:
    case TS_SUSPENDED_FOR_AUTOCONT:
        panic("?");
    case TS_SUSPENDED_NEW:
        store_release_atomic_u32(&gthread->lowlevel_state, TS_SUSPENDED);
        if (mode != SSPM_SYNC_SOLICITED) {
            if (gs->thread_events) {
                reason = "create";
                goto send_reason;
            } else if (mode == SSPM_ASYNC) {
                return false;
            } else {
                ensure(false); // caller should have continued this thread
            }
        }
        FALLTHROUGH;
    case TS_SUSPENDED:
        signal = gs->non_stop ? 0 : PPC_LINUX_SIGINT;
        goto send_signal;
    case TS_TRACE: // single step
        signal = PPC_LINUX_SIGTRAP;
        goto send_signal;
    case TS_IAB:
        reason = "hwbreak";
        goto send_reason;
    case TS_DSI:
        if (gthread->thread->context.ex0 & (1 << 22)) {
            // DABR (watchpoint) hit
            int type = GDBSTUB_BP_TYPE_WATCH_RW;
            if (gs->dabr_bp_inuse)
                type = (int)gs->dabr_bp.type;
            reason_arg = gthread->thread->context.ex1;
            switch (type) {
            case GDBSTUB_BP_TYPE_WATCH_W:
                reason = "watch";
                goto send_reason_with_arg;
            case GDBSTUB_BP_TYPE_WATCH_R:
                reason = "rwatch";
                goto send_reason_with_arg;
            case GDBSTUB_BP_TYPE_WATCH_RW:
                reason = "awatch";
                goto send_reason_with_arg;
            default:
                panic("?type");
            }
            signal = PPC_LINUX_SIGTRAP;
            goto send_signal;
        }
        signal = PPC_LINUX_SIGSEGV;
        goto send_signal;
    case TS_ISI:
        signal = PPC_LINUX_SIGSEGV;
        goto send_signal;
    case TS_PROGRAM:
        if (gthread->thread->context.srr1 & (1 << 17)) {
            // breakpoint
            reason = "swbreak";
            goto send_reason;
        }
        signal = PPC_LINUX_SIGILL;
        goto send_signal;
    case TS_DEAD:
        gdbstub_thread_remove(gs, gthread);
        if (!gs->thread_events)
            return false;
        snprintf(stop_reply, stop_reply_cap, "w00;%zx;", thread_id);
        goto send_it;
    default:
        panic("?reason %d", llstate);
    }

send_reason:
    snprintf(stop_reply, stop_reply_cap, "T05thread:%zx;%s:;", thread_id, reason);
    goto send_it;

send_reason_with_arg:
    snprintf(stop_reply, stop_reply_cap, "T05thread:%zx;%s:%08x;", thread_id, reason,
             reason_arg);
    goto send_it;

send_signal:
    snprintf(stop_reply, stop_reply_cap, "T%02xthread:%zx;", signal, thread_id);
    goto send_it;

send_it:
    if (mode == SSPM_ASYNC) {
        memcpy(buf, "Stop:", 5);
        // send a notification packet
        gdbstub_send_response_type(gs, buf, strlen(buf), '%');
    } else
        gdbstub_send_response_str(gs, stop_reply);
    return true;
}

static void
gdbstub_handle_vstopped(struct gdbstub *gs) {
    log("gdbstub_handle_vstopped\n");
    while (gs->thread_info_next_id < gs->threads_by_id.count()) {
        size_t id = gs->thread_info_next_id++;
        struct gdbstub_thread *gthread = gdbstub_find_thread_by_id(gs, id);
        if (!gthread)
            continue;
        uint32_t llstate = load_acquire_atomic_u32(&gthread->lowlevel_state);
        if (llstate != TS_RUNNING) {
            if (gdbstub_send_stopped_packet(gs, gthread, llstate, SSPM_SYNC_SOLICITED)) {
                // we may be dead, but gdbstub_thread_remove fixes things up in that case
                return;
            }
        }
    }
    gdbstub_send_response_str(gs, "OK");
}

size_t
gdbstub_read_mem(struct gdbstub *gs, char *buf, uint32_t addr, size_t len) {
    if (!addr)
        return 0; // just to avoid triggering assert in priv_try_memcpy
    // log("reading mem %x,%x\n", (int)addr, (int)len);
    size_t actual = priv_try_memcpy_in(buf, (const void *)(uintptr_t)addr, len);
    // log("actual=%x\n", (int)actual);

    // redact
    for (maybe<struct gdbstub_bp *> mbp = gs->sw_bps_trie.first_ge(addr & ~3ul);
         mbp; mbp = gs->sw_bps_trie.next(mbp.unwrap())) {
        struct gdbstub_bp *bp = mbp.unwrap();
        ensure(bp->type == GDBSTUB_BP_TYPE_SW);
        uint32_t start = max(bp->addr, addr);
        uint32_t end = min(bp->addr + bp->size - 1, addr + (uint32_t)actual - 1);
        if (start < end) {
            memcpy(buf + (start - addr), (char *)&bp->orig_insn + (start - bp->addr),
                   end - start);
        } else {
            break;
        }
    }

    return actual;
}

static size_t
gdbstub_write_mem(struct gdbstub *gs, uint32_t addr, size_t len, const char *buf) {
    if (!addr)
        return 0; // just to avoid triggering assert in priv_try_memcpy
    log("writing mem %x,%x\n", (int)addr, (int)len);
    // unredact
    for (maybe<struct gdbstub_bp *> mbp = gs->sw_bps_trie.first_ge(addr & ~3ul);
         mbp; mbp = gs->sw_bps_trie.next(mbp.unwrap())) {
        struct gdbstub_bp *bp = mbp.unwrap();
        ensure(bp->type == GDBSTUB_BP_TYPE_SW);
        uint32_t start = max(bp->addr, addr);
        uint32_t end = min(bp->addr + bp->size - 1, addr + (uint32_t)len - 1);
        if (start < end) {
            memcpy((char *)&bp->orig_insn + (start - bp->addr), buf + (start - addr),
                   end - start);
        } else {
            break;
        }
    }
    size_t ret = priv_try_memcpy_out((void *)(uintptr_t)addr, buf, len);
    return ret;
}

__attribute__((noinline)) static bool
search_memory_inner(const char *tmp, size_t tmp_len, const char *pat_buf, size_t pat_len,
                    uintptr_t *offset) {
    if (pat_len > tmp_len)
        return false;
#define SPECIALIZED(ty)                                                                  \
    do {                                                                                 \
        ty val, tmpval;                                                                  \
        __builtin_memcpy(&val, pat_buf, sizeof(val));                                    \
        for (size_t i = 0; i <= tmp_len - pat_len; i++) {                                \
            __builtin_memcpy(&tmpval, tmp + i, sizeof(val));                             \
            if (val == tmpval) {                                                         \
                *offset = i;                                                             \
                return true;                                                             \
            }                                                                            \
        }                                                                                \
        return false;                                                                    \
    } while (0)

    switch (pat_len) {
    case 1:
        SPECIALIZED(uint8_t);
    case 2:
        SPECIALIZED(uint16_t);
    case 4:
        SPECIALIZED(uint32_t);
    case 8:
        SPECIALIZED(uint64_t);
    default:
        for (size_t i = 0; i <= tmp_len - pat_len; i++) {
            if (!memcmp(tmp + i, pat_buf, pat_len)) {
                *offset = i;
                return true;
            }
        }
        return false;
    }
}

static bool
gdbstub_search_memory(struct gdbstub *gs, uint32_t addr, size_t len, const char *pat_buf,
                      size_t pat_len, uintptr_t *out) {
    size_t tmp_len = 0x1000;
    char *tmp = &gs->memrw_raw_buf[GDBSTUB_MAX_MEMRW_LEN - tmp_len];
    while (len > 0) {
        size_t xlen = min(len, (size_t)0x1000);
        size_t actual = gdbstub_read_mem(gs, tmp, addr, xlen);
        size_t offset;
        bool was_short = actual < xlen;
        if (was_short)
            log("gdbstub_search_memory: note: only read %x @ %x\n", (int)actual, (int)addr);
        if (search_memory_inner(tmp, actual, pat_buf, pat_len, &offset)) {
            *out = addr + offset;
            return true;
        }
        if (was_short)
            break;
        size_t to_advance = xlen - (pat_len - 1);
        ensure(to_advance <= xlen); // caller checked that 0 < pat_len <= 0xf00
        addr += xlen;
        len -= xlen;
    }
    return false;
}

static void
gdbstub_suspend_all_threads(struct gdbstub *gs) {
    if (GDBSTUB_VERBOSE)
        log("gdbstub_suspend_all_threads: {\n");
    for (struct gdbstub_thread *gthread : gdbstub_iter_known_threads(gs)) {
        if (!gthread->may_suspend)
            continue;
        uint32_t llstate = load_acquire_atomic_u32(&gthread->lowlevel_state);
        if (llstate == TS_RUNNING) {
            gdbstub_lowlevel_suspend(gs, gthread, TS_SUSPENDED);
        } else if (struct gdbstub_bp *bp = gdbstub_check_bp_conds_actions(gs, gthread, llstate, /*skip_actions*/true)) {
            // Pretend it was suspended before getting to the breakpoint.
            store_release_atomic_u32(&gthread->lowlevel_state, TS_SUSPENDED);
        }
    }
    if (GDBSTUB_VERBOSE)
        log("} gdbstub_suspend_all_threads\n");
}

static void
gdbstub_mark_all_threads_sent(struct gdbstub *gs) {
    // "GDB uses the ‘?’ packet as necessary to probe the target state after a mode
    // change."
    for (struct gdbstub_thread *gthread : gdbstub_iter_known_threads(gs)) {
        if (gthread->may_suspend
            && load_acquire_atomic_u32(&gthread->lowlevel_state) != TS_RUNNING)
            gthread->sent_stopped_packet_since_continue = true;
    }
}

static void
gdbstub_ctrlc(struct gdbstub *gs) {
    gdbstub_suspend_all_threads(gs);
}

static int
gdbstub_recv_more(struct gdbstub *gs) {
    ssize_t res;
    gs->recv_buf_off = 0;
    while (1) {
        gdbstub_handle_all_stopped_threads(gs);
        res = sock_wrapper_recv_awkward(gs->client_wrapper, gs->recv_buf,
                                        sizeof(gs->recv_buf), &gs->wakeup_thread_event);
        // log("@res=%d\n", res);
        if (res == WOULDBLOCK_RET)
            continue;
        else if (res < 0) {
            log("sock_wrapper_recv_awkward() returned %zd\n", res);
            return -1;
        }
        gs->recv_buf_size = (size_t)res;
        return 0;
    }
}

static inline int
gdbstub_getchar(struct gdbstub *gs, char *cp) {
    if (gs->recv_buf_off == gs->recv_buf_size) {
        int err;
        if ((err = gdbstub_recv_more(gs)))
            return err;
    }
    *cp = gs->recv_buf[gs->recv_buf_off++];
    return 0;
}

static void
gdbstub_xfer_generate_targetxml(struct gdbstub *gs, char *buf, size_t *sizep, bool init,
                                bool *donep) {
    ensure(init);
    ensure(*sizep >= sizeof(target_xml) - 1);
    memcpy(buf, target_xml, sizeof(target_xml) - 1);
    *sizep = sizeof(target_xml) - 1;
    *donep = true;
}

static bool
quote_escape(char **pp, char *end, const char *str) {
    char c;
    while ((c = *str++)) {
        if (c == '"') {
            if (!usprintf(pp, end, "&quot;"))
                return false;
        } else {
            if (*pp == end)
                return false;
            *(*pp)++ = c;
        }
    }
    return true;
}

static void
gdbstub_xfer_generate_threads(struct gdbstub *gs, char *buf, size_t *sizep, bool init,
                              bool *donep) {
    char *p = buf, *end = buf + *sizep;
    *sizep = 0;

    if (init) {
        gs->xferthreads_next_id = 0;
        if (!usprintf(&p, end, "<?xml version=\"1.0\"?><threads>"))
            return; // bad
    }

    size_t id;
    for (id = gs->xferthreads_next_id;
         id < gs->threads_by_id.count();
         (gs->xferthreads_next_id = ++id)) {
        struct gdbstub_thread *gthread = gdbstub_find_thread_by_id(gs, id);
        if (!gthread)
            continue;
        int gold = gdbstub_garbo_lock(gs);
        if (TS_DEAD == load_acquire_atomic_u32(&gthread->lowlevel_state)) {
            // won't be able to get thread name
            gdbstub_garbo_unlock(gs, gold);
            continue;
        }
        char *old = p;
        bool ok = usprintf(&p, end, "<thread id=\"%zx\" name=\"", gthread->id)
            && quote_escape(&p, end, get_thread_name(gthread->thread))
            && usprintf(&p, end, "\" />");
        gdbstub_garbo_unlock(gs, gold);
        if (!ok) {
            p = old;
            break;
        }
    }

    if (id == gs->threads_by_id.count() && usprintf(&p, end, "</threads>")) {
        *donep = true;
    }
    *sizep = (size_t)(p - buf);
}

static void
gdbstub_xfer_generate_libraries(struct gdbstub *gs, char *buf, size_t *sizep, bool init,
                                bool *donep) {
    char *p = buf, *end = buf + *sizep;
    *sizep = 0;

    if (init) {
        if (!usprintf(&p, end, "<?xml version=\"1.0\"?><library-list>"))
            return; // bad
        heap_free(&gs->top_heap, gs->qxfer_libraries_rpl_info);
        gs->qxfer_libraries_rpl_info = nullptr;
        gs->qxfer_libraries_rpl_info_idx = 0;
        gs->qxfer_libraries_rpl_info_count = 0;
        int count = OSDynLoad_GetNumberOfRPLs();
        if (count < 0) {
            log("gdbstub_xfer_generate_libraries: OSDynLoad_GetNumberOfRPLs() returned %d\n", count);
            goto post_init;
        }
        maybe<void *> mrpli = heap_alloc(&gs->top_heap, sat_mul((size_t)count, sizeof(struct rpl_info)));
        if (!mrpli) {
            log("gdbstub_xfer_generate_libraries: oom\n");
            goto post_init;
        }
        struct rpl_info *rpli = (struct rpl_info *)mrpli.unwrap();
        if (!OSDynLoad_GetRPLInfo(0, count, rpli)) {
            log("gdbstub_xfer_generate_libraries: OSDynLoad_GetRPLInfo failed\n");
            heap_free(&gs->top_heap, rpli);
            goto post_init;
        }

        gs->qxfer_libraries_rpl_info = rpli;
        gs->qxfer_libraries_rpl_info_idx = 0;
        gs->qxfer_libraries_rpl_info_count = (size_t)count;
    }
post_init:
    for (; gs->qxfer_libraries_rpl_info_idx < gs->qxfer_libraries_rpl_info_count;
         ++gs->qxfer_libraries_rpl_info_idx) {
        struct rpl_info *lib = &gs->qxfer_libraries_rpl_info[gs->qxfer_libraries_rpl_info_idx];
        if (!usprintf(&p, end,
            "<library name=\"%s\"><segment address=\"0x%x\"/><segment address=\"0x%x\"/><segment address=\"0x%x\"/></library>",
            lib->name,
            lib->text_addr,
            lib->data_addr,
            lib->rodata_addr))
            break;
    }

    if (gs->qxfer_libraries_rpl_info_idx == gs->qxfer_libraries_rpl_info_count &&
        usprintf(&p, end, "</library-list>")) {
        heap_free(&gs->top_heap, gs->qxfer_libraries_rpl_info);
        gs->qxfer_libraries_rpl_info = nullptr;
        *donep = true;
    }
    *sizep = (size_t)(p - buf);
}

static void
gdbstub_handle_xfer_read(struct gdbstub *gs, const char *area, const char *annex,
                         size_t xferoff, size_t xferlen) {
    if (GDBSTUB_VERBOSE)
        log("gdbstub_handle_xfer_read: area=%s annex=%s off=%u len=%u\n", area, annex,
            (int)xferoff, (int)xferlen);
    const char *expected_annex;
    xfer_generator_t generator;

    if (!strcmp(area, "features")) {
        expected_annex = "target.xml";
        generator = gdbstub_xfer_generate_targetxml;
    } else if (!strcmp(area, "threads")) {
        expected_annex = "";
        generator = gdbstub_xfer_generate_threads;
    } else if (!strcmp(area, "libraries")) {
        expected_annex = "";
        generator = gdbstub_xfer_generate_libraries;
    } else {
        gdbstub_send_response_str(gs, "");
        return;
    }

    if (strcmp(annex, expected_annex)) {
        gdbstub_send_response_str(gs, "E00");
        return;
    }

    bool init;
    if (xferoff == 0) {
        init = true;
    } else if (generator != gs->cur_xfer_generator || xferoff != gs->cur_xfer_off) {
        gdbstub_send_response_str(gs, "E44");
        return;
    } else {
        init = false;
    }

    bool done = false;
    char *buf = gs->response_buf;
    size_t actual = min(xferlen, sizeof(gs->response_buf) - 1);
    generator(gs, buf + 1, &actual, init, &done);

    if (xferlen > 0 && actual == 0 && !done) {
        log("gdbstub: client had a weirdly small buffer size?\n");
        gdbstub_send_response_str(gs, "E45");
        gs->cur_xfer_generator = nullptr;
        return;
    }

    gs->cur_xfer_generator = generator;
    gs->cur_xfer_off = xferoff + actual;

    buf[0] = done ? 'l' : 'm';
    gdbstub_send_response(gs, buf, actual + 1);
}

static void
gdbstub_handle_qthreadinfo(struct gdbstub *gs) {
    char *buf = gs->response_buf;
    size_t cap = sizeof(gs->response_buf);
    buf[0] = 'm';
    size_t off = 1;

    for (size_t id = gs->thread_info_next_id;
         id < gs->threads_by_id.count();
         (gs->thread_info_next_id = ++id)) {
        if (!gdbstub_find_thread_by_id(gs, id))
            continue;
        if (cap - off < sizeof(",deadbeef"))
            break;
        if (off > 1)
            buf[off++] = ',';
        snprintf(buf + off, cap - off, "%zx", id);
        off += strlen(buf + off);
    }
    if (off == 1)
        gdbstub_send_response_str(gs, "l");
    else
        gdbstub_send_response(gs, buf, off);
}

#define CMD_IS(cmd_buf, cmd_len, strlit)                                                 \
    ((cmd_len) == sizeof(strlit) - 1 && !memcmp(cmd_buf, (strlit), cmd_len))

static void
gdbstub_handle_q(struct gdbstub *gs, const char *cmd_buf, size_t cmd_len) {
    if (cmd_len == 1) {
        log("gdbstub: malformed query command\n");
        return;
    }
    switch (cmd_buf[1]) {
    case 'A':
        if (CMD_IS(cmd_buf, cmd_len, "qAttached")) {
            gdbstub_send_response_str(gs, "1");
            return;
        }
        if (CMD_IS(cmd_buf, cmd_len, "QAgent:1") || CMD_IS(cmd_buf, cmd_len, "QAgent:0"))
            goto ok;
        if (!strncmp(cmd_buf, "QAllow:", sizeof("QAllow:") - 1)) {
            // whatever
            goto ok;
        }
        break;

    case 'C':
        // current thread ID? which one? TODO
        snprintf(gs->response_buf, sizeof(gs->response_buf), "QC%zx",
                 gs->cur_thread_g->id);
        gdbstub_send_response_str(gs, gs->response_buf);
        return;

    case 'f':
        if (CMD_IS(cmd_buf, cmd_len, "qfThreadInfo")) {
            gs->thread_info_next_id = 0;
            return gdbstub_handle_qthreadinfo(gs);
        }
        break;
    case 's':
        if (CMD_IS(cmd_buf, cmd_len, "qsThreadInfo")) {
            return gdbstub_handle_qthreadinfo(gs);
        }
        break;
    case 'N':
        if (CMD_IS(cmd_buf, cmd_len, "QNonStop:1")) {
            gs->non_stop = true;
            gs->allstop_is_stopped = false;
            gdbstub_mark_all_threads_sent(gs);
            goto ok;
        } else if (CMD_IS(cmd_buf, cmd_len, "QNonStop:0")) {
            gs->non_stop = false;
            gs->allstop_is_stopped = true;
            gdbstub_suspend_all_threads(gs);
            gdbstub_mark_all_threads_sent(gs);
            goto ok;
        }
        break;
    case 'T':
        if (CMD_IS(cmd_buf, cmd_len, "QThreadEvents:1")) {
            gs->thread_events = true;
            goto ok;
        } else if (CMD_IS(cmd_buf, cmd_len, "QThreadEvents:0")) {
            gs->thread_events = false;
            goto ok;
        }
        break;
    case 'S':
        if (CMD_IS(cmd_buf, cmd_len, "QStartNoAckMode")) {
            gs->no_ack = true;
            goto ok;
        }
        if (!strncmp(cmd_buf, "qSupported", sizeof("qSupported") - 1)) {
            snprintf(gs->response_buf, sizeof(gs->response_buf),
// clang-format off
                     "PacketSize=%x;"
                     "qXfer:features:read+;"
                     "qXfer:threads:read+;"
                     "qXfer:libraries:read+;"
                     "QNonStop+;"
                     "QStartNoAckMode+;"
                     "swbreak+;"
                     "hwbreak+;"
                     "QThreadEvents+;"
                     //"TracepointSource+;"
                     "QAgent+;"
                     "QAllow+;"
                     //"InstallInTrace+;" // TODO
                     //"EnableDisableTracepoints+;" // TODO
                     //"QTBuffer:size+;" // TODO
                     "tracenz+;" // TODO
                     "BreakpointCommands+;"
                     "ConditionalBreakpoints+;"
                     //"ConditionalTracepoints+;"
// clang-format on
                     , (int)GDBSTUB_MAX_REQUEST_LEN_ENCODED);
            gdbstub_send_response_str(gs, gs->response_buf);
            return;
        }
        if (!strncmp(cmd_buf, "qSearch", sizeof("qSearch") - 1)) {
            unsigned int addr, len;
            int pat_off;
            size_t pat_len;
            // The docs claim that qSearch patterns are hex encoded, but GDB just sends
            // binary!

            // size_t pat_len_hex;
            // char *pat_buf = gs->memrw_raw_buf;
            if (2 != ssscanf(cmd_buf, "qSearch:memory:%x;%x;%n", &addr, &len, &pat_off)
                || (pat_len = cmd_len - (size_t)pat_off) > 0xf00 || pat_len == 0) {
                /* ||
                (pat_len_hex = (cmd_len - (size_t)pat_off)) & 1 ||
                pat_len_hex > 0x1000 ||
                !s_parse_hex(cmd_buf + pat_off, pat_buf, (pat_len = pat_len_hex / 2)))
                */
                log("invalid qSearch command: %s\n", cmd_buf);
                gdbstub_send_response_str(gs, "E01");
                return;
            }
            uintptr_t found_addr;
            const char *pat_buf = cmd_buf + pat_off;
            if (gdbstub_search_memory(gs, addr, len, pat_buf, pat_len, &found_addr)) {
                snprintf(gs->response_buf, sizeof(gs->response_buf), "1,%x", (int)found_addr);
                gdbstub_send_response_str(gs, gs->response_buf);
            } else {
                gdbstub_send_response_str(gs, "0");
            }

            return;
        }
        break;
    case 'X': {
        char area[32], annex[32];
        unsigned xferoff, xferlen;
        int size;

        annex[0] = '\0';
        // ew, two are needed because ssscanf class fields can't be empty
        if ((4
                 == ssscanf(cmd_buf, "qXfer:%31[^:]:read:%31[^:]:%x,%x%n", area, annex,
                            &xferoff, &xferlen, &size)
             || 3
                 == ssscanf(cmd_buf, "qXfer:%31[^:]:read::%x,%x%n", area, &xferoff,
                            &xferlen, &size))
            && size == cmd_len) {
            gdbstub_handle_xfer_read(gs, area, annex, xferoff, xferlen);
            return;
        }
        break;
    }
    }
    log("gdbstub: unknown query command: %.*s\n", (int)cmd_len, cmd_buf);
    gdbstub_send_response_str(gs, "");
    return;
ok:
    gdbstub_send_response_str(gs, "OK");
}

static void
gdbstub_handle_stopped_thread(struct gdbstub *gs, struct gdbstub_thread *gthread) {
    uint32_t llstate = load_acquire_atomic_u32(&gthread->lowlevel_state);
    ensure(llstate != TS_RUNNING);
    if (llstate == TS_SUSPENDED_NEW && !gs->thread_events) {
        // it's fine, just let the thread start
        gdbstub_lowlevel_continue(gs, gthread);
        return;
    }
    if (gs->non_stop) {
        gdbstub_send_stopped_packet(gs, gthread, llstate, SSPM_ASYNC);
    } else {
        ensure(!gs->allstop_is_stopped);
        if (gdbstub_send_stopped_packet(gs, gthread, llstate, SSPM_SYNC_UNSOLICITED)) {
            gs->allstop_is_stopped = true;
            gs->cur_thread_c = gs->cur_thread_g = gthread;
            gdbstub_suspend_all_threads(gs);
        }
    }
}

static void
gdbstub_check_nascent_threads(struct gdbstub *gs) {
    while (gs->first_nascent_thread) {
        int old = gdbstub_garbo_lock(gs);
        struct gdbstub_thread *gthread = gs->first_nascent_thread;
        gs->first_nascent_thread = gthread->next_nascent_thread;
        gthread->next_nascent_thread = nullptr;
        gdbstub_garbo_unlock(gs, old);

        size_t id = gs->next_thread_id;
        gthread->id = id;
        if (id >= gs->threads_by_id.count()) {
            if (!gs->threads_by_id.append((uintptr_t)gthread, &gs->top_heap))
                panic("gdbstub_check_nascent_threads: OOM");
            gs->next_thread_id = id + 1;
        } else {
            gs->next_thread_id = gs->threads_by_id[id] >> 1;
            gs->threads_by_id[id] = (uintptr_t)gthread;
        }
    }
}

static void
gdbstub_top_up_heap(struct gdbstub *gs) {
    int old = gdbstub_garbo_lock(gs);
    bool ok = club_heap_ensure_min_free_count(&gs->threads_heap);
    gdbstub_garbo_unlock(gs, old);
    if (!ok)
        panic("gdbstub_top_up_heap: OOM?");
}

static void
gdbstub_handle_all_stopped_threads(struct gdbstub *gs) {
    if (GDBSTUB_VERBOSE)
        log("gdbstub_handle_all_stopped_threads\n");
    gdbstub_check_nascent_threads(gs);
    gdbstub_top_up_heap(gs);
    //log("...topup done\n");
    if (gs->allstop_is_stopped)
        return;
    // Report on threads in order of interestingness, represented by higher
    // lowlevel_state values
    while (!gs->allstop_is_stopped) {
        uint32_t max_llstate = TS_RUNNING;
        for (struct gdbstub_thread *gthread : gdbstub_iter_known_threads(gs)) {
            if (!gthread->sent_stopped_packet_since_continue)
                max_llstate
                    = max(max_llstate, load_acquire_atomic_u32(&gthread->lowlevel_state));
        }
        if (max_llstate == TS_RUNNING)
            break;
        // Prioritize cur_thread_c so we don't keep getting switched back
        // to thread 1.  (But this doesn't take precedence over the
        // interestingness sort, so if a different thread got a more
        // important exception, it'll be handled first.
        struct gdbstub_thread *gthread = gs->cur_thread_c;
        uint32_t llstate = load_acquire_atomic_u32(&gthread->lowlevel_state);
        if (!gthread->sent_stopped_packet_since_continue && llstate >= max_llstate)
            gdbstub_handle_stopped_thread(gs, gthread);
        // Then try all the other threads:
        for (struct gdbstub_thread *gthread : gdbstub_iter_known_threads(gs)) {
            if (gs->allstop_is_stopped)
                return;
            uint32_t llstate = load_acquire_atomic_u32(&gthread->lowlevel_state);
            if (!gthread->sent_stopped_packet_since_continue && llstate >= max_llstate)
                gdbstub_handle_stopped_thread(gs, gthread);
        }
    }
}

static void
gdbstub_clear_allstop(struct gdbstub *gs) {
    if (gs->allstop_is_stopped) {
        // Clear exceptions, even if we're trying to continue a different thread (!?)...
        // This should really only affect one thread.
        for (struct gdbstub_thread *gthread : gdbstub_iter_known_threads(gs)) {
            if (gthread->sent_stopped_packet_since_continue) {
                if (load_acquire_atomic_u32(&gthread->lowlevel_state) > TS_SUSPENDED)
                    store_release_atomic_u32(&gthread->lowlevel_state, TS_SUSPENDED);
                gthread->sent_stopped_packet_since_continue = false;
            }
        }
        gs->allstop_is_stopped = false;
    }
}

static void
gdbstub_thread_continue(struct gdbstub *gs, struct gdbstub_thread *gthread) {
    gdbstub_clear_allstop(gs);
    gthread->sent_stopped_packet_since_continue = false;
    gthread->checked_conds_actions_since_continue = false;
    gdbstub_lowlevel_continue(gs, gthread);
    if (!gs->non_stop) {
        bool have_more_exceptions = false;
        for (struct gdbstub_thread *othread : gdbstub_iter_known_threads(gs)) {
            uint32_t state = load_acquire_atomic_u32(&othread->lowlevel_state);
            if (state > TS_SUSPENDED) {
                if (GDBSTUB_VERBOSE)
                    log("gdbstub_thread_continue: [%s] had an exception (%s)\n",
                        gdbstub_thread_dbgname(gs, othread),
                        gdbstub_thread_state_name(state));
                have_more_exceptions = true;
                break;
            }
        }
        if (GDBSTUB_VERBOSE)
            log("gdbstub_thread_continue: have_more_exceptions=%d\n", have_more_exceptions);
        if (!have_more_exceptions) {
            // Continue all suspended threads
            for (struct gdbstub_thread *othread : gdbstub_iter_known_threads(gs)) {
                //log("gdbstub_thread_continue: othread=%s\n", gdbstub_thread_dbgname(gs, othread));
                uint32_t llstate = load_acquire_atomic_u32(&othread->lowlevel_state);
                if (llstate == TS_SUSPENDED || llstate == TS_SUSPENDED_NEW)
                    gdbstub_lowlevel_continue(gs, othread);
            }
        }
        if (GDBSTUB_VERBOSE)
            log("gdbstub_thread_continue: end\n");
    }
}

static bool
gdbstub_thread_step(struct gdbstub *gs, struct gdbstub_thread *gthread, bool have_srr0,
                    uint32_t srr0, bool range, uint32_t start, uint32_t end) {
    if (load_acquire_atomic_u32(&gthread->lowlevel_state) == TS_RUNNING)
        return true;
    if (!(gthread->thread->context.srr1 & (1 << 15))) {
        log("gdbstub_thread_step: refusing to step thread that was suspended in interrupts-disabled mode\n");
        return false;
    }
    gdbstub_clear_allstop(gs);
    gthread->sent_stopped_packet_since_continue = false;
    gthread->checked_conds_actions_since_continue = false;
    if (have_srr0)
        gdbstub_set_reg(gs, gthread, GDBSTUB_REG_PC, srr0);
    uint32_t pc;
    uint32_t llstate = -1u;
    do {
        gthread->thread->context.srr1 |= 1 << 10;
        gdbstub_lowlevel_continue(gs, gthread);
        if (GDBSTUB_VERBOSE)
            log("stepping...\n");
        do {
            OSUnlockMutex(&gs->everything_mutex);
            OSWaitEvent(&gs->wakeup_thread_event);
            OSLockMutex(&gs->everything_mutex);
            llstate = load_acquire_atomic_u32(&gthread->lowlevel_state);
            pc = (uint32_t)gdbstub_get_reg(gs, gthread, GDBSTUB_REG_PC);
            if (GDBSTUB_VERBOSE)
                log("step done(?) state=%s pc=%x\n", gdbstub_thread_state_name(llstate), (int)pc);
        } while (llstate == TS_RUNNING);
    } while (range && llstate == TS_TRACE && pc >= start && pc < end);
    return true;
}

static void
gdbstub_detach_actions(struct gdbstub *gs) {
    log("gdbstub_detach_actions\n");
    for (struct gdbstub_bp *bp : gs->sw_bps_trie)
        gdbstub_bp_remove(gs, bp);
    if (gs->dabr_bp_inuse)
        gdbstub_bp_remove(gs, &gs->dabr_bp);
    if (gs->iabr_bp_inuse)
        gdbstub_bp_remove(gs, &gs->iabr_bp);
    // At this point, no new gthreads should be created because have_client is false.
    gdbstub_check_nascent_threads(gs);
    for (struct gdbstub_thread *gthread : gdbstub_iter_known_threads(gs)) {
        gdbstub_lowlevel_continue(gs, gthread);
        gdbstub_thread_remove(gs, gthread);
    }
}

enum gdbstub_x_type {
    GDBSTUB_X_COND,
    GDBSTUB_X_ACTION,
};

static const char *
gdbstub_parse_x_list(struct gdbstub *gs, const char *p, const char *end, uarray<struct gdbstub_agent_expr *> *arrayp, enum gdbstub_x_type type) {
    if (p == end || *p == ';')
        return p;
    uarray<struct gdbstub_agent_expr *> array;
    struct gdbstub_agent_expr *ae = nullptr;
    while (p != end && *p != ';') {
        int parsed_size;
        unsigned int expr_size;
        if (1 != ssscanf(p, "X%x,%n", &expr_size, &parsed_size))
            goto bad;
        p += parsed_size;
        if (expr_size > (end - p) / 2)
            goto bad;
        ae = (struct gdbstub_agent_expr *)unwrap_or(heap_alloc(&gs->top_heap, sizeof(struct gdbstub_agent_expr) + expr_size), goto oom);
        ae->len = expr_size;
        if (!s_parse_hex(p, ae->expr, expr_size)) {
            log("gdbstub_parse_x_list: bad hex\n");
            goto bad;
        }
        if (!gdbstub_validate_agent_expr(gs, ae, &gs->trace_global, false, type == GDBSTUB_X_ACTION ? 0 : 1)) {
            log("gdbstub_parse_x_list(type=%d): validation failed\n", type);
            goto bad;
        }
        if (!array.append(ae, &gs->top_heap))
            goto oom;
        ae = nullptr;
        p += expr_size * 2;
    }

    *arrayp = move(array);
    return p;
oom:
    log("gdbstub_parse_x_list: oom\n");
bad:
    heap_free(&gs->top_heap, ae);
    return nullptr;
}

static bool
gdbstub_handle_cont_action(struct gdbstub *gs, struct gdbstub_thread *gthread,
                           const char *action) {
    if (gthread == nullptr)
        return true;
    switch (action[0]) {
    case 'c': // continue
    case 'i': // stepi
    case 's': // step
    case 'C': // continue with sig
    case 'I': // stepi with sig
    case 'S': { // step with sig
        const char *p = action + 1;
        bool have_addr;
        uint32_t addr = 0;

        if (action[0] <= 'Z') {
            // capital letter: we have a signal
            unsigned int sig;
            int size;
            if (1 != ssscanf(p, "%x%n", &sig, &size))
                goto invalid_syntax;
            p += size;
            have_addr = *p != '\0';
            if (have_addr) {
                if (*p++ != ';')
                    goto invalid_syntax;
            }
        } else
            have_addr = *p != '\0';
        if (have_addr) {
            int size2;
            if (1 != ssscanf(p, "%x%n", &addr, &size2) || p[size2] != 0)
                goto invalid_syntax;
        }
        if (action[0] == 'C' || action[0] == 'c') {
            if (have_addr)
                gdbstub_set_reg(gs, gthread, GDBSTUB_REG_PC, addr);
            gdbstub_thread_continue(gs, gthread);
        } else {
            if (!gdbstub_thread_step(gs, gthread, have_addr, addr, false, 0, 0))
                goto e43;
        }
        return true;
    }
    case 'r': {
        unsigned int start, end;
        int size;
        if (2 != ssscanf(action, "r%x,%x%n", &start, &end, &size) || action[size] != 0)
            goto invalid_syntax;
        if (start == end) {
            // docs say this is supposed to always stop, even if
            // pc == start
            start = 1;
            end = 0;
        }
        if (!gdbstub_thread_step(gs, gthread, false, 0, true, start, end))
            goto e43;
        return true;
    }
    case 't': // stop
        if (!gs->non_stop) {
            log("gdbstub: can't stop thread in all-stop mode\n");
            gdbstub_send_response_str(gs, "E01");
            return false;
        }
        if (gthread->may_suspend && !gthread->sent_stopped_packet_since_continue)
            gdbstub_lowlevel_suspend(gs, gthread, TS_SUSPENDED);
        return true;
    default:
        goto invalid_syntax;
    }
invalid_syntax:
    log("gdbstub: malformed thread-continue action: [%s]\n", action);
    gdbstub_send_response_str(gs, "E01");
    return false;
e43:
    gdbstub_send_response_str(gs, "E43");
    return false;
}

static void
gdbstub_handle_command(struct gdbstub *gs, char *cmd_buf, size_t cmd_len) {
    char first;
    int size;
    if (GDBSTUB_VERBOSE)
        log("gdbstub: received command: [%.*s]\n", (int)cmd_len, cmd_buf);
    if (cmd_len == 0)
        goto bad;
    first = cmd_buf[0];
    switch (first) {
    case '!':
        // enable extended mode, which just enables the R (restart) command
        goto ok;
    case '?':
        if (cmd_len != 1)
            goto bad;
        if (gs->non_stop) {
            gs->vstopped_next_id = 0;
            gdbstub_handle_vstopped(gs);
        } else {
            struct gdbstub_thread *gthread = gs->cur_thread_g;
            uint32_t llstate = load_acquire_atomic_u32(&gthread->lowlevel_state);
            if (llstate == TS_DEAD || llstate == TS_RUNNING)
                goto e42;
            ensure(
                gdbstub_send_stopped_packet(gs, gthread, llstate, SSPM_SYNC_SOLICITED));
        }
        return;
    case 'B': {
        // old breakpoint packet
        unsigned int addr;
        char mode;
        if (2 != ssscanf(cmd_buf, "B%x,%c%n", &addr, &mode, &size) || size != cmd_len
            || (mode != 'S' && mode != 'C'))
            goto bad;
        if (!gdbstub_bp_mod(gs, GDBSTUB_BP_TYPE_SW,
                            mode == 'S' ? GDBSTUB_BP_SET : GDBSTUB_BP_CLEAR, addr, 4, nullptr, nullptr))
            goto e42;
        goto ok;
    }
    case 'z':
    case 'Z': {
        // new breakpoint packet
        unsigned int type, addr, mode;
        if (cmd_len == 1 || cmd_buf[1] < '0' || cmd_buf[1] > '9')
            goto bad;
        type = (unsigned int)(cmd_buf[1] - '0');
        if (type > GDBSTUB_BP_TYPE_MAX) {
            // just unsupported, not bad
            gdbstub_send_response_str(gs, "");
            return;
        }
        if (2 != ssscanf(cmd_buf + 2, ",%x,%x%n", &addr, &mode, &size))
            goto bad;
        const char *p = cmd_buf + 2 + size;
        uarray<struct gdbstub_agent_expr *> cond_exprs, action_exprs;
        auto _d = defer([&]() {
            cleanup_agent_exprs(gs, &cond_exprs);
            cleanup_agent_exprs(gs, &action_exprs);
        });
        const char *end = cmd_buf + cmd_len;
        while (p != end) {
            if (first != 'Z' || *p++ != ';')
                goto bad;
            int persist;
            if (1 == ssscanf(p, "cmds:%x,%n", &persist, &size)) {
                // ignore 'persist'
                p += size;
                if (!(p = gdbstub_parse_x_list(gs, p, end, &action_exprs, GDBSTUB_X_ACTION)))
                    goto bad;
            } else {
                if (!(p = gdbstub_parse_x_list(gs, p, end, &cond_exprs, GDBSTUB_X_COND)))
                    goto bad;
            }
        }
        if (!gdbstub_bp_mod(gs, (enum gdbstub_bp_type)type,
                            first == 'Z' ? GDBSTUB_BP_SET : GDBSTUB_BP_CLEAR,
                            addr, mode,
                            &cond_exprs, &action_exprs))
            goto e42;
        goto ok;
    }

    case 'c':
    case 'i':
    case 's':
    case 'C':
    case 'I':
    case 'S':
        if (gs->non_stop)
            goto bad;
        gdbstub_handle_cont_action(gs, gs->cur_thread_c, cmd_buf);
        return;
    case 'D':
        // detach ...
        goto ok;
    case 'g': {
        // read all registers
        struct gdbstub_thread *gthread = gs->cur_thread_g;
        static_assert(sizeof(gs->memrw_raw_buf) >= GDBSTUB_REG_BUF_SIZE, "reg buf size");
        char *buf = gs->memrw_raw_buf, *out = buf;
        if (gthread->may_suspend && gdbstub_thread_is_running_or_dead(gs, gthread))
            goto e42;
        for (int reg = 0; reg < GDBSTUB_REG_COUNT; reg++) {
            uint64_t val = gdbstub_get_reg(gs, gthread, (enum gdbstub_reg)reg);
            if (reg >= GDBSTUB_REG_FPR0 && reg <= GDBSTUB_REG_FPR31) {
                uint64_t swapped = bswap(val);
                memcpy(out, &swapped, 8);
                out += 8;
            } else {
                uint32_t swapped = bswap((uint32_t)val);
                memcpy(out, &swapped, 4);
                out += 4;
            }
        }
        ensure(out - buf == GDBSTUB_REG_BUF_SIZE);
        char *respbuf = gs->response_buf;
        s_hex_encode(respbuf, buf, GDBSTUB_REG_BUF_SIZE);
        gdbstub_send_response(gs, respbuf, GDBSTUB_REG_BUF_SIZE * 2);
        return;
    }
    case 'G': {
        // write all registers
        struct gdbstub_thread *gthread = gs->cur_thread_g;
        if (gthread->may_suspend && gdbstub_thread_is_running_or_dead(gs, gthread))
            goto e42;
        if (cmd_len != 1 + GDBSTUB_REG_BUF_SIZE * 2)
            goto bad;
        const char *in = cmd_buf + 1;
        for (int reg = 0; reg < GDBSTUB_REG_COUNT; reg++) {
            uint64_t val;
            if (reg >= GDBSTUB_REG_FPR0 && reg <= GDBSTUB_REG_FPR31) {
                uint64_t tmp;
                memcpy(&tmp, in, 8);
                in += 8;
                val = bswap(tmp);
            } else {
                uint32_t tmp;
                memcpy(&tmp, in, 4);
                in += 4;
                val = (uint64_t)bswap(tmp);
            }
            gdbstub_set_reg(gs, gthread, (enum gdbstub_reg)reg, val);
        }
        ensure(in - (cmd_buf + 1) == GDBSTUB_REG_BUF_SIZE);
        goto ok;
    }
    case 'H': {
        struct gdbstub_thread **threadpp;
        char kind;
        unsigned int tid;
        if (2 != ssscanf(cmd_buf, "H%c%x%n", &kind, &tid, &size) || size != cmd_len)
            goto bad;
        if (kind == 'c')
            threadpp = &gs->cur_thread_c;
        else if (kind == 'g')
            threadpp = &gs->cur_thread_g;
        else
            goto bad;
        struct gdbstub_thread *gthread
            = gdbstub_find_next_thread_by_vcont_id(gs, tid, nullptr);
        if (!gthread)
            goto e42;
        *threadpp = gthread;
        goto ok;
    }
    case 'k':
        // kill
        goto e42;
    case 'R':
    case 'r':
        // restart
        goto e42;
    case 'm': // read memory - hex
    case 'x': { // read memory - binary (LLDB extension)
        unsigned int addr, len;
        if (2 != ssscanf(cmd_buf + 1, "%x,%x%n", &addr, &len, &size)
            || size != cmd_len - 1)
            goto bad;
        len = min(len, (unsigned)GDBSTUB_MAX_MEMRW_LEN);
        char *raw_buf = gs->memrw_raw_buf;
        size_t actual = gdbstub_read_mem(gs, raw_buf, addr, len);
        if (first == 'x') {
            gdbstub_send_response(gs, raw_buf, actual);
        } else {
            size_t resplen = actual * 2;
            ensure(resplen <= sizeof(gs->response_buf));
            char *respbuf = gs->response_buf;
            s_hex_encode(respbuf, raw_buf, actual);
            gdbstub_send_response(gs, respbuf, resplen);
        }
        return;
    }
    case 'M': // read memory - hex
    case 'X': { // read memory - binary
        // write memory
        unsigned long long laddr;
        unsigned int addr, len;
        unsigned int bytes_off;
        char *raw_buf;
        const char *in_ptr;
        size_t in_len;
        if (2 != ssscanf(cmd_buf, "%*c%llx,%x:%n", &laddr, &len, &bytes_off))
            goto bad;
        addr = (unsigned int)laddr; // ew, gdb
        in_ptr = cmd_buf + bytes_off;
        in_len = cmd_len - bytes_off;
        if (first == 'M') {
            // avoid overflow
            if ((in_len & 1) || in_len / 2 != len)
                goto bad;
            if (len > GDBSTUB_MAX_MEMRW_LEN)
                panic("should be impossible");
            raw_buf = gs->memrw_raw_buf;
            if (!s_parse_hex(in_ptr, raw_buf, len))
                goto bad;
        } else {
            if (in_len != len)
                goto bad;
            raw_buf = (char *)in_ptr;
        }
        size_t actual = gdbstub_write_mem(gs, addr, len, raw_buf);
        if (actual < len)
            goto bad;
        goto ok;
    }

    case 'p': { // read reg
        unsigned int reg;
        char respbuf[16];
        if (1 != ssscanf(cmd_buf, "p%x%n", &reg, &size) || size != cmd_len)
            goto bad;
        if (reg >= GDBSTUB_REG_COUNT) {
            log("gdbstub: read request for invalid register 0x%x\n", reg);
            goto e42;
        }
        struct gdbstub_thread *gthread = gs->cur_thread_g;
        if (gthread->may_suspend && gdbstub_thread_is_running_or_dead(gs, gthread))
            goto e42;
        uint64_t temp = bswap(gdbstub_get_reg(gs, gthread, (enum gdbstub_reg)reg));
        s_hex_encode(respbuf, (char *)&temp, 8);
        gdbstub_send_response(gs, respbuf, sizeof(respbuf));
        return;
    }
    case 'P': { // write reg
        unsigned int reg;
        unsigned long long val;
        if (2 != ssscanf(cmd_buf, "P%x=%llx%n", &reg, &val, &size) || size != cmd_len)
            goto bad;
        if (reg >= GDBSTUB_REG_COUNT) {
            log("gdbstub: write request for invalid register 0x%x\n", reg);
            goto e42;
        }
        struct gdbstub_thread *gthread = gs->cur_thread_g;
        if (gthread->may_suspend && gdbstub_thread_is_running_or_dead(gs, gthread))
            goto e42;
        gdbstub_set_reg(gs, gthread, (enum gdbstub_reg)reg, val);
        goto ok;
    }

    case 'T': {
        unsigned int tid;
        if (1 != ssscanf(cmd_buf, "T%x%n", &tid, &size) || size != cmd_len)
            goto bad;
        if (gdbstub_find_thread_by_id(gs, tid))
            goto ok;
        else
            goto e42;
    }
    case 'v':
        if (CMD_IS(cmd_buf, cmd_len, "vStopped")) {
            if (!gs->non_stop)
                goto bad;
            gdbstub_handle_vstopped(gs);
            return;
        }
        if (CMD_IS(cmd_buf, cmd_len, "vCtrlC")) {
            gdbstub_ctrlc(gs);
            goto ok;
        }
        if (CMD_IS(cmd_buf, cmd_len, "vCont?")) {
            gdbstub_send_response_str(gs, "vCont;cCsSr");
            return;
        }
        if (!strncmp(cmd_buf, "vCont", 5)) {
            if (cmd_buf[5] != ';')
                goto bad;
            char *p = cmd_buf + 6;
            while (*p != '\0') {
                char *action = p;
                while (*p != ';' && *p != ':' && *p != '\0')
                    p++;
                const char *thread_id_str = "-1";
                if (*p != '\0') {
                    *p++ = '\0';
                    thread_id_str = p;
                    while (*p != ';' && *p != '\0')
                        p++;
                    if (*p != '\0')
                        *p++ = '\0';
                }
                unsigned int thread_id;
                int size;
                if (1 != ssscanf(thread_id_str, "%x%n", &thread_id, &size)
                    || thread_id_str[size] != '\0')
                    goto bad;
                struct gdbstub_thread *gthread = nullptr;
                while ((gthread
                        = gdbstub_find_next_thread_by_vcont_id(gs, thread_id, gthread))) {
                    if (!gdbstub_handle_cont_action(gs, gthread, action))
                        return;
                }
            }
            if (gs->non_stop)
                goto ok; // GDB expects OK, which is undocumented.
            else
                return; // Response (if any) will be a stop reply.
        }
        goto unk;
    case 'Q':
    case 'q':
        gdbstub_handle_q(gs, cmd_buf, cmd_len);
        return;
    default:
        goto unk;
    }
    ENSURE_UNREACHABLE;
unk:
    log("gdbstub: unknown command: [%.*s]\n", (int)cmd_len, cmd_buf);
    gdbstub_send_response_str(gs, "");
    return;
bad:
    log("gdbstub: malformed command: [%.*s]\n", (int)cmd_len, cmd_buf);
    gdbstub_send_response_str(gs, "E01");
    return;

e42:
    gdbstub_send_response_str(gs, "E42");
    return;
ok:
    gdbstub_send_response_str(gs, "OK");
    return;
}

static void
gdbstub_client_loop(struct gdbstub *gs) {
    while (1) {
        if (GDBSTUB_VERBOSE)
            log("gdbstub_client_loop: loop\n");
        char *cmd_buf = gs->cmd_buf;
        size_t cmd_buf_cap = sizeof(gs->cmd_buf) - 1; // 1 for nul terminator
        size_t cmd_buf_off = 0;
        char c;
        if (gdbstub_getchar(gs, &c))
            return;
        switch (c) {
        case '+':
        case '-':
            // ignore acknowledgement
            break;
        case '\x03':
            gdbstub_ctrlc(gs);
            break;
        case '\xff':
            // may be the first half of a telnet break sequence
            if (gdbstub_getchar(gs, &c))
                return;
            if (c != '\xf3') {
                log("gdbstub: got byte 0x%02x after 0xff, expected 0xf3\n", (int)c);
                return;
            }
            gdbstub_ctrlc(gs);
            break;
        case '$':
            // a normal command
            while (1) {
                if (gdbstub_getchar(gs, &c))
                    return;
                if (c == '#') {
                    // end of command; ignore checksum
                    int i;
                    for (i = 0; i < 2; i++)
                        if (gdbstub_getchar(gs, &c))
                            return;
                    break;
                } else if (c == '}') {
                    // escape
                    if (gdbstub_getchar(gs, &c))
                        return;
                    c ^= 0x20;
                }
                if (cmd_buf_off + 1 >= cmd_buf_cap) {
                    log("overlong packet\n");
                    cmd_buf[0] = '?';
                    cmd_buf_off = 1;
                }
                cmd_buf[cmd_buf_off++] = c;
            }
            cmd_buf[cmd_buf_off++] = '\0'; // for ssscanf
            if (!gs->no_ack) {
                char ack = '+';
                gdbstub_send_all(gs, &ack, 1);
            }
            gdbstub_handle_command(gs, cmd_buf, cmd_buf_off - 1);
            break;
        default:
            log("gdbstub: invalid byte at start of command: 0x%02x\n", (int)c);
            return;
        }
    }
}

static void gdbstub_gather_existing_threads(struct gdbstub *gs);

static int
gdbstub_client_thread_func(int _, struct async_listener_thread *alt) {
    struct gdbstub *gs = (struct gdbstub *)alt->al->user;
    log("gdbstub_client_thread_func gs=%p\n", gs);
    //log_flush();
    OSLockMutex(&gs->everything_mutex);
    log("locked\n");
    //log_flush();
    if (load_acquire_atomic_u32(&gs->have_client)) {
        struct sock_wrapper *old = gs->client_wrapper;
        gs->client_wrapper = &alt->wrapper;

        gdbstub_send_response_str(gs, "E999");

        gs->client_wrapper = old;
        OSUnlockMutex(&gs->everything_mutex);
        return 0;
    }

    gs->client_wrapper = &alt->wrapper;
    gs->handler_thread = OSGetCurrentThread();

    log("gathering existing threads\n");
    gdbstub_gather_existing_threads(gs);
    log("gather ok\n");

    // per-connection initialization

    gs->non_stop = false;
    gs->no_ack = false;
    gs->thread_events = false;
    gs->allstop_is_stopped = false;
    gs->cur_xfer_generator = nullptr;

    gs->recv_buf_off = gs->recv_buf_size = 0;

    // initialize threads
    gs->cur_thread_g = gs->cur_thread_c = gdbstub_get_any_suspendable_thread(gs);

    store_release_atomic_u32(&gs->have_client, true);

    gdbstub_ctrlc(gs);

    gdbstub_client_loop(gs);

    store_release_atomic_u32(&gs->have_client, false);
    gdbstub_detach_actions(gs);

    gs->handler_thread = nullptr;
    OSUnlockMutex(&gs->everything_mutex);
    return 0;
}

#define DUMP_USE_COMPRESSION 1

#if DUMP_USE_COMPRESSION
#include "lz4.h"
#endif

#define DUMP_CHUNKSIZE ((size_t)(128 * 1024))
#if DUMP_USE_COMPRESSION
struct dump_compressed {
    uint32_t clen;
    uint32_t dlen;
    char data[LZ4_COMPRESSBOUND(DUMP_CHUNKSIZE)];
} __attribute__((aligned(8)));
#endif
struct dump_tmp {
    struct sock_wrapper *wrapper;
#if DUMP_USE_COMPRESSION
    OSThread compressor_thread;
    OSEvent ready_to_compress_event;
    OSEvent done_compressing_event;
    const void *cur_in;
    size_t cur_inlen;
    struct dump_compressed *cur_out;
    char compressor_stack[0x8000] __attribute__((aligned(16)));
    char lz4_state[LZ4_STREAMSIZE] __attribute__((aligned(8)));
    struct dump_compressed compressed[2];
#else
    char tmp[DUMP_CHUNKSIZE] __attribute__((aligned(8)));
#endif
};

// over wifi this dumps at the amazing rate of 1MB/s

#if DUMP_USE_COMPRESSION
static int
compressor_thread_func(int i, void *xtmp) {
    struct dump_tmp *tmp = (struct dump_tmp *)xtmp;
    while (1) {
        OSWaitEvent_mb(&tmp->ready_to_compress_event);
        if (!tmp->cur_in)
            return 123;

        size_t outlen = sizeof(tmp->compressed[0].data);
        // log("compressing %p,%u to %p,%u\n", tmp->cur_in, tmp->cur_inlen,
        // tmp->cur_out->data, outlen);
        int ret
            = LZ4_compress_fast_extState(tmp->lz4_state, (char *)tmp->cur_in, tmp->cur_out->data,
                                         (int)tmp->cur_inlen, (int)outlen, 1);
        ensure(ret > 0);
        tmp->cur_out->clen = (uint32_t)ret;
        tmp->cur_out->dlen = (uint32_t)tmp->cur_inlen;
        OSSignalEvent_mb(&tmp->done_compressing_event);
    }
}

static void
dump_init_compression(struct dump_tmp *tmp) {
    ensure(LZ4_sizeofState() == sizeof(tmp->lz4_state));
    OSInitEvent(&tmp->ready_to_compress_event, 0, true);
    OSInitEvent(&tmp->done_compressing_event, 0, true);
    ensure(OSCreateThread_orig(&tmp->compressor_thread, (void *)compressor_thread_func, 0, tmp,
                          tmp->compressor_stack + sizeof(tmp->compressor_stack),
                          sizeof(tmp->compressor_stack),
                          16, // prio
                          0)); // not detached
    OSSetThreadName(&tmp->compressor_thread, "gdbstub dump compressor");
    ensure(OSResumeThread(&tmp->compressor_thread));
}

static void
dump_exit_compression(struct dump_tmp *tmp) {
    tmp->cur_in = nullptr;
    OSSignalEvent_mb(&tmp->ready_to_compress_event);
    ensure(OSJoinThread(&tmp->compressor_thread, nullptr));
}
#endif

static bool
dump_send_compressed(const void *data, size_t len, struct dump_tmp *tmp) {
    log("dump_send_compressed: %p,%u\n", data, (int)len);
    size_t chunks = (len + DUMP_CHUNKSIZE - 1) / DUMP_CHUNKSIZE;
    for (size_t i = 0; i <= chunks; i++) {
        size_t off = 0, xlen = 0;
        const char *in = nullptr;
        if (i < chunks) {
            off = i * DUMP_CHUNKSIZE;
            xlen = min(len - off, DUMP_CHUNKSIZE);
            in = (char *)data + off;
        }
#if DUMP_USE_COMPRESSION
        struct dump_compressed *prev_out;
        if (i > 0) { // wait for the previous chunk to be done
            OSWaitEvent_mb(&tmp->done_compressing_event);
            prev_out = &tmp->compressed[(i - 1) & 1];
        }
        if (i < chunks) {
            // start compressing this chunk
            tmp->cur_in = in;
            tmp->cur_inlen = xlen;
            tmp->cur_out = &tmp->compressed[i & 1];
            OSSignalEvent_mb(&tmp->ready_to_compress_event);
        }
        if (i > 0) {
            // send the previous chunk
            if (!sock_wrapper_send_all(tmp->wrapper, prev_out,
                                       offsetof(struct dump_compressed, data)
                                           + prev_out->clen)) {
                if (i < chunks)
                    OSWaitEvent_mb(&tmp->done_compressing_event);
                return false;
            }
        }
#else
        if (i < chunks) {
            memcpy(tmp->tmp, in, xlen);
            if (!sock_wrapper_send_all(tmp->wrapper, tmp->tmp, xlen))
                return false;
        }
#endif
    }
    return true;
}

static bool
dump_send_compressed_check_overlap(const void *data, size_t len, struct dump_tmp *tmp) {
    while (len) {
        size_t xlen;
#if DUMP_USE_COMPRESSION
        void *compressed = tmp->compressed;
        size_t compressed_len = sizeof(tmp->compressed);
#else
        void *compressed = tmp->tmp;
        size_t compressed_len = sizeof(tmp->tmp);
#endif
        size_t coff = (uintptr_t)data - (uintptr_t)compressed;
        if (coff < compressed_len) {
            char fakebuf[512];
            xlen = min(sizeof(fakebuf), min(len, compressed_len - coff));
            memset(fakebuf, 0xef, xlen);
            if (!dump_send_compressed(fakebuf, xlen, tmp))
                return false;
        } else {
            xlen = min(len, -coff);
            if (!dump_send_compressed(data, xlen, tmp))
                return false;
        }
        data = (char *)data + xlen;
        len -= xlen;
    }
    return true;
}

static UNUSED bool
dump_region(const void *data, size_t len, struct dump_tmp *tmp) {
    uint32_t info[2] = {(uint32_t)(uintptr_t)data, (uint32_t)len};
    return dump_send_compressed(info, sizeof(info), tmp)
        && dump_send_compressed_check_overlap(data, len, tmp);
}

static int
dumper_thread_func(int _, struct async_listener_thread *alt) {
    struct dump_tmp *tmp = (struct dump_tmp *)alignup(alt->ctx, __alignof(*tmp));
    tmp->wrapper = &alt->wrapper;

#if DUMP_USE_COMPRESSION
    dump_init_compression(tmp);
#endif

    bool ok = true;

#if !DUMMY
    void *addr = nullptr;
    size_t size = 0;
    for (int i = 1; i <= 2; i++) {
        ensure(!OSGetMemBound(i, &addr, &size));
        if (i == 2) {
            // ???
            ensure(size > 131072);
            size -= 131072;
        }
        if (ok)
            ok = dump_region(addr, size, tmp);
    }
    ensure(OSGetForegroundBucket(&addr, &size));
    if (ok)
        ok = dump_region(addr, size, tmp);
#endif // !DUMMY

    if (ok)
        ok = dump_send_compressed("OKOK", 4, tmp);
#if DUMP_USE_COMPRESSION
    dump_exit_compression(tmp);
#endif
    log("dumper gone (ok=%d)\n", ok);
    return 0;
}

enum {
    MEMRW_ACTION_READ = 0,
    MEMRW_ACTION_WRITE = 1,
    MEMRW_ACTION_GET_MODULE_LIST = 2,
};

struct memrw_request {
    uint32_t magic; // MEMQ
    uint32_t action;
    uint32_t addr;
    uint32_t len;
};

struct memrw_response {
    uint32_t magic; // MEMA
    uint32_t actual;
};

static int
memrw_thread_func(int _, struct async_listener_thread *alt) {
    struct sock_wrapper *wrapper = &alt->wrapper;
    struct memrw_request req;
    char stackbuf[0x1000];
    void *buf = stackbuf;
    struct memrw_response resp;
    UNUSED int rpl_count;
    while (sock_wrapper_recv_all(wrapper, &req, sizeof(req))) {
        if (req.magic != 'MEMQ') {
            log("memrw_thread_func: bad req magic %x\n", req.magic);
            goto end;
        }
        size_t required;
        switch (req.action) {
        case MEMRW_ACTION_READ:
        case MEMRW_ACTION_WRITE:
            required = req.len;
            break;
        case MEMRW_ACTION_GET_MODULE_LIST:
            #if !DUMMY
                rpl_count = OSDynLoad_GetNumberOfRPLs();
                if (rpl_count < 0)
                    goto oom;
                required = sizeof(struct rpl_info) * (size_t)rpl_count;
            #else
                required = 0;
            #endif
            break;
        default:
            log("memrw_thread_func: bad action %u\n", req.action);
            goto end;
        }
        buf = req.len > sizeof(stackbuf) ? MEMAllocFromDefaultHeap(req.len)
                                         : stackbuf;
        if (!buf)
            goto oom;
        size_t actual, to_send;
        switch (req.action) {
        case MEMRW_ACTION_READ:
            actual = to_send = priv_try_memcpy_in(buf, (void *)(uintptr_t)req.addr, req.len);
            break;
        case MEMRW_ACTION_WRITE:
            if (!sock_wrapper_recv_all(wrapper, buf, req.len))
                goto end;
            actual = priv_try_memcpy_out((void *)(uintptr_t)req.addr, buf, req.len);
            to_send = 0;
            break;
        case MEMRW_ACTION_GET_MODULE_LIST:
            #if !DUMMY
                actual = to_send = OSDynLoad_GetRPLInfo(0, rpl_count, (struct rpl_info *)buf) ? required : 0;
            #else
                actual = to_send = 0;
            #endif
            break;
        }
        resp.magic = 'MEMA';
        resp.actual = (uint32_t)actual;
        if (!sock_wrapper_send_all(wrapper, &resp, sizeof(resp)) ||
            !sock_wrapper_send_all(wrapper, buf, to_send))
            goto end;
        if (buf != stackbuf)
            MEMFreeToDefaultHeap(buf);
        buf = stackbuf;
    }
oom:
    resp.magic = 'OOM!';
    resp.actual = 0;
    sock_wrapper_send_all(wrapper, &resp, sizeof(resp));
end:
    if (buf != stackbuf)
        MEMFreeToDefaultHeap(buf);
    return 0;
}

static void
gdbstub_thread_remove(struct gdbstub *gs, struct gdbstub_thread *gthread) {
    bool is_dead = load_acquire_atomic_u32(&gthread->lowlevel_state) == TS_DEAD;
    ensure(is_dead || !load_acquire_atomic_u32(&gs->have_client));

    if (gs->cur_thread_g == gthread)
        gs->cur_thread_g = gdbstub_get_any_suspendable_thread(gs);
    if (gs->cur_thread_c == gthread)
        gs->cur_thread_c = gdbstub_get_any_suspendable_thread(gs);

    gthread->thread = (target_OSThread *)0xbadddead;

    ensure(gthread->id != (size_t)-1);
    gs->threads_by_id[gthread->id] = (gs->next_thread_id << 1) | 1;
    gs->next_thread_id = gthread->id;

    int old = gdbstub_garbo_lock(gs);
    if (!is_dead)
        gs->threads_trie.erase(gthread);
    heap_free(&gs->threads_heap, gthread);
    gdbstub_garbo_unlock(gs, old);
}

static struct gdbstub_thread *
gdbstub_find_thread_by_id(struct gdbstub *gs, size_t id) {
    if (id >= gs->threads_by_id.count())
        return nullptr;
    uintptr_t maybe_gthread = gs->threads_by_id[id];
    if (maybe_gthread & 1)
        return nullptr;
    struct gdbstub_thread *gthread = (struct gdbstub_thread *)maybe_gthread;
    if (load_acquire_atomic_u32(&gthread->lowlevel_state) == TS_DEAD)
        return nullptr;
    return gthread;
}

static const char *
gdbstub_thread_dbgname_(struct gdbstub *gs, const struct gdbstub_thread *gthread,
                        struct gdbstub_thread_dbgname_buf &&buf) {
    int old = gdbstub_garbo_lock(gs);
    bool dead = TS_DEAD == load_acquire_atomic_u32(&gthread->lowlevel_state);
    // XXX TODO: I've gotten a crash here, name = 0xba0c (probably meaningless)
    snprintf(buf.buf, 128u, "id:%#zx g:%p t:%p name:%s", gthread->id,
             gthread, gthread->thread, dead ? "(dead)" : get_thread_name(gthread->thread));
    gdbstub_garbo_unlock(gs, old);
    return buf.buf;
}

// no panics please, this can be called inside scheduler lock
static struct gdbstub_thread *
gdbstub_find_thread_locked(struct gdbstub *gs, target_OSThread *thread) {
    return gdbstub_find_or_create_thread_locked(gs, thread, false);
}

static struct gdbstub_thread *
gdbstub_find_or_create_thread_locked(struct gdbstub *gs, target_OSThread *thread, bool or_create) {
    bool is_new = false;
    maybe<struct gdbstub_thread *> mgthread = gs->threads_trie.find((uintptr_t)thread, or_create ? &is_new : nullptr);
    struct gdbstub_thread *gthread = unwrap_or(mgthread, {
        // OOM
        return nullptr;
    });
    if (!is_new)
        return gthread;
    gthread->thread = thread;
    gthread->next_nascent_thread = gs->first_nascent_thread;
    gs->first_nascent_thread = gthread;
    gthread->id = (size_t)-1;
    #if DUMMY
        gthread->may_suspend = true;
    #else
        gthread->may_suspend = gdbstub_may_suspend_thread(gs, gthread->thread);
    #endif
    store_release_atomic_u32(&gthread->lowlevel_state, TS_RUNNING);
    gthread->sent_stopped_packet_since_continue = false;
    gthread->checked_conds_actions_since_continue = false;
    gthread->my_suspend_count = 0;
    gthread->pc_munged = false;
    return gthread;
}

#if DUMMY
static void
gdbstub_create_fake_thread_for_dummy(struct gdbstub *gs) {
    target_OSThread *thread = new target_OSThread;
    memset(&thread->context, 0x33, sizeof(thread->context));
    thread->name = "fakey";
    struct gdbstub_thread *gthread = gdbstub_find_or_create_thread_locked(gs, thread, true);
    ensure(gthread);

}
#endif // DUMMY

static void
gdbstub_gather_existing_threads(struct gdbstub *gs) {
#if !DUMMY
    OSThread *thread = OSGetCurrentThread();
    //log("active_thread=%p next=%p\n", thread->active_link.prev, thread->active_link.next);
    ensure(thread->active_link.prev || thread->active_link.next);
    bool bad;
    for (size_t run = 0; run < 5; run++) {
        gdbstub_top_up_heap(gs);
        int old = gdbstub_garbo_lock(gs);
        __OSLockScheduler(OSGetCurrentThread());
        thread = OSGetCurrentThread();
        while (thread->active_link.prev)
            thread = thread->active_link.prev;
        bad = false;
        while (thread) {
            if (!gdbstub_find_or_create_thread_locked(gs, thread, true)) {
                bad = true;
                break;
            }
            thread = thread->active_link.next;
        }
        __OSUnlockScheduler(OSGetCurrentThread());
        gdbstub_garbo_unlock(gs, old);
        if (!bad)
            break;
    }
    if (bad)
        panic("gdbstub_gather_existing_threads: out of space to make gthread for thread "
              "%p :(",
              thread);
#else // !DUMMY
    gdbstub_create_fake_thread_for_dummy(gs);
#endif
    gdbstub_check_nascent_threads(gs);
}

static void
install_exception_stuff(void) {
#if !DUMMY
    patch_kernel_devmode();
    patch_tracestub();
    if (1) {
        FUNC_HOOK_TY(OSSetExceptionCallbackEx)::orig(ALL_CORES, DSI, gdbstub_exc_handler_dsi);
        FUNC_HOOK_TY(OSSetExceptionCallbackEx)::orig(ALL_CORES, ISI, gdbstub_exc_handler_isi);
        FUNC_HOOK_TY(OSSetExceptionCallbackEx)::orig(ALL_CORES, PROGRAM, gdbstub_exc_handler_program);
    }
#endif
};

static void
gdbstub_init(struct gdbstub *gs) {
    log("gdbstub_init: 1\n");
    install_exception_stuff();
#if !DUMMY
    log("gdbstub_init: 2, btw clock = %u\n", OSGetSystemInfo()->clock);
#endif

    OSInitMutex(&gs->everything_mutex);
    OSLockMutex(&gs->everything_mutex);

    OSInitEvent(&gs->wakeup_thread_event, 0, /*autoreset*/ true);

    splitter_heap_init(&gs->top_heap);
    ensure(club_heap_init(&gs->threads_heap, &gs->top_heap, sizeof(struct gdbstub_thread), 32, 32));
    ensure(club_heap_init(&gs->sw_bps_heap, &gs->top_heap, sizeof(struct gdbstub_bp), 64, 0));
    gs->threads_trie.init(&gs->threads_heap);
    gs->sw_bps_trie.init(&gs->sw_bps_heap);

    ensure(gs->threads_by_id.append((uintptr_t)-1, &gs->top_heap));
    gs->next_thread_id = 1;

    /*
    log("orig_OSCreateThread=%p\n", hook_OSCreateThread::orig);
    uint32_t *p = (uint32_t *)hook_OSCreateThread::orig;
    log("%x, %x, %x, %x\n", p[0], p[1], p[2], p[3]);
    log_flush();
    */

#if !DUMMY
    ensure(OSCreateThread_orig(
        &gs->run_queue_dummy_thread, (void *)run_queue_dummy_thread_func, 0, gs,
        gs->run_queue_dummy_thread_stack
            + sizeof(gs->run_queue_dummy_thread_stack), // stack top
        (int)sizeof(gs->run_queue_dummy_thread_stack), // stack size
        16, // prio
        8)); // detached
    log("all good\n");
    log_flush();
    gs->dummy_allowed_to_be_active = true; // ??
    ensure(OSSetThreadAffinity(&gs->run_queue_dummy_thread, 0));
    OSSetThreadName(&gs->run_queue_dummy_thread, "gdbstub dummy");
    log("starting\n");
    OSResumeThread(&gs->run_queue_dummy_thread);
#endif

    log("what's up\n");
    log_flush();

    uint16_t listen_port = 8000, dumper_port = 8001, memrw_port = 8002;
#if !GDBSTUB_USE_USBDUCKS
    dump_my_ips();
#endif
    log("listening for GDB on port %d; dumper on %d; memrw on %d\n", listen_port,
        dumper_port, memrw_port);

    async_listener_start(&gs->handler_async_listener, listen_port, "gdbstub handler",
                         gdbstub_client_thread_func, 0, 1, gs);
    async_listener_start(&gs->dumper_async_listener, dumper_port, "gdbstub dumper",
                         dumper_thread_func, sizeof(struct dump_tmp),
                         __alignof(struct dump_tmp), nullptr);
    async_listener_start(&gs->memrw_async_listener, memrw_port, "gdbstub memrw",
                         memrw_thread_func, 0, 1, nullptr);

    store_release_atomic_u32(&gs->initialized, 2);

    OSUnlockMutex(&gs->everything_mutex);

    log("gdbstub_init out\n");
}

static struct gdbstub *
get_gdbstub(void) {
    if (!in_right_process())
        return nullptr;
    struct gdbstub *gs = &*the_gdbstub;
    if (load_acquire_atomic_u32(&gs->initialized) == 2)
        return gs;
    else
        return nullptr;
}

static SDKCALL UNUSED void
shutdown_gdbstub(void) {
    uninstall_hooks(gdbstub_hooks_list);
}

void startup_gdbstub(void);
void
startup_gdbstub(void) {
#if !DUMMY
    log("init_gdbstub, self_elf_start=%p, data_start=%p\n", self_elf_start, data_start);
    log_flush();
    install_hooks(gdbstub_hooks_list);
    static struct at_exit my_at_exit = {shutdown_gdbstub};
    __ghs_at_exit(&my_at_exit);
#endif
    did_install_gdbstub_hooks = true;
    gdbstub_init(&*the_gdbstub);
}

#if !DUMMY
struct func_hook_info gdbstub_hooks_list[] = {
    FUNC_HOOK(OSExitThread),
    FUNC_HOOK(OSCreateThread),
    FUNC_HOOK(OSCreateThreadType),
    FUNC_HOOK(__OSCreateThreadType),
    {0}
};
#endif

#if GDBSTUBTEST
int main() {
    startup_gdbstub();
    while (1)
        sleep(1);
}
#endif

END_LOCAL_DECLS


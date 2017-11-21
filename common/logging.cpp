#include "logging.h"
#include "decls.h"
#include "loader.h"
#include "misc.h"
#include "ssprintf.h"

#define ENABLE_LOG_USBDUCKS (1 && !DUMMY && ENABLE_USBDUCKS)
#define ENABLE_LOG_NETWORK (1 && !DUMMY)
#define ENABLE_LOG_COS (1 && !DUMMY)

#if ENABLE_LOG_USBDUCKS
#include "usbducks.h"
#endif

static UNUSED void
OSSignalEvent_mb_if_safe(OSEvent *event) {
    OSMemoryBarrier();
#if !DUMMY
    if (!OSIsInterruptEnabled()) {
        uint32_t locked_magic = ((uintptr_t)OSGetCurrentThread() & ~3u) | (uintptr_t)OSGetCoreId();
        if (__OSSchedulerLock == locked_magic)
            return;
    }
#endif
    OSSignalEvent(event);
}

#if LOG_ENABLED
#define LOG_BUF_SIZE 0x10000u
#define LOG_BUF_MASK (LOG_BUF_SIZE - 1)

static void log_backend(const char *s, size_t len, int flags);

#define LOG_USE_ASYNC (!DUMMY || LOGTEST)
#if LOG_USE_ASYNC
static struct log_buf {
    char buf[LOG_BUF_SIZE];
    char backend_buf[LOG_BUF_SIZE];
    atomic<uint32_t> read_pos;
    atomic<uint32_t> write_pos;
    atomic<uint32_t> overrun;
    OSEvent new_data_event;
#if !DUMMY
    OSThread thread;
    char thread_stack[0x4000] __attribute__((aligned(16)));
#endif
    atomic<uint32_t> total_queued_logs;
    OSMutex total_finished_logs_mtx;
    OSCond total_finished_logs_cond;
    uint32_t total_finished_logs;
} *g_log_buf;

struct log_packet {
    atomic<char> ready;
    uint16_t packet_len;
    uint16_t len;
    int flags;
    char data[];
};

static struct log_packet *
log_buf_append_start(struct log_buf *log_buf, size_t len) {
    if (0) {
    overrun:
        atomic_store_explicit(&log_buf->overrun, true, memory_order_relaxed);
        OSSignalEvent_mb_if_safe(&log_buf->new_data_event);
        return nullptr;
    }
    if (len >= LOG_BUF_SIZE - sizeof(struct log_packet))
        goto overrun;
    size_t packet_len = sizeof(struct log_packet) + len;
    packet_len = (packet_len + __alignof(struct log_packet) - 1) & ~(__alignof(struct log_packet) - 1);
    while (1) {
        uint32_t read_pos = atomic_load_explicit(&log_buf->read_pos, memory_order_seq_cst);
        uint32_t write_pos = atomic_load_explicit(&log_buf->write_pos, memory_order_relaxed);
        if (write_pos == read_pos + LOG_BUF_SIZE)
            goto overrun;
        int32_t avail = (int32_t)(read_pos & LOG_BUF_MASK) - (int32_t)(write_pos & LOG_BUF_MASK);
        if (avail <= 0) {
            avail = LOG_BUF_SIZE - (write_pos & LOG_BUF_MASK);
            if (avail < (int32_t)packet_len && (read_pos & LOG_BUF_MASK) >= packet_len) {
                uint32_t new_write_pos = (write_pos + LOG_BUF_MASK) & ~LOG_BUF_MASK;
                if (OSCompareAndSwapAtomic(&log_buf->write_pos, write_pos, new_write_pos)) {
                    memset(log_buf->buf + (write_pos & LOG_BUF_MASK), 2, (size_t)avail);
                    OSSignalEvent_mb_if_safe(&log_buf->new_data_event);
                }
                continue;
            }
        }
        if (avail < (int32_t)packet_len)
            goto overrun;
        uint32_t new_write_pos = (uint32_t)(write_pos + packet_len);
        if (!OSCompareAndSwapAtomic(&log_buf->write_pos, write_pos, new_write_pos))
            continue;
        struct log_packet *lp = (struct log_packet *)(log_buf->buf + (write_pos & LOG_BUF_MASK));
        lp->packet_len = (uint16_t)packet_len;
        OSAddAtomic(&log_buf->total_queued_logs, 1);
        return lp;
    }
}

static struct log_packet *
log_buf_shift_start(struct log_buf *log_buf) {
    while (1) {
        size_t read_pos = atomic_load_explicit(&log_buf->read_pos, memory_order_relaxed);
        size_t write_pos = atomic_load_explicit(&log_buf->write_pos, memory_order_acquire);
        if (read_pos == write_pos)
            return nullptr;
        char *p = log_buf->buf + (read_pos & LOG_BUF_MASK);
        char c = *p;
        switch (c) {
        case 0:
            return nullptr; // not yet
        case 1:
            return (struct log_packet *)p;
        case 2: {
            size_t new_read_pos = (read_pos + LOG_BUF_MASK) & ~LOG_BUF_MASK;
            memset(p, 0, new_read_pos - read_pos);
            atomic_store_explicit(&log_buf->read_pos, new_read_pos, memory_order_release);
            continue;
        }
        default:
            _panic(LOG_NONE, "?");
        }
    }
}

static void
log_buf_shift_finish(struct log_buf *log_buf) {
    size_t read_pos = atomic_load_explicit(&log_buf->read_pos, memory_order_relaxed);
    struct log_packet *lp = (struct log_packet *)(log_buf->buf + (read_pos & LOG_BUF_MASK));
    size_t packet_len = lp->packet_len;
    read_pos += packet_len;
    // this is needed:
    memset(lp, 0, packet_len);
    atomic_store_explicit(&log_buf->read_pos, read_pos, memory_order_release);
}

#if DUMMY
static void *
log_buf_thread_func(void *_log_buf)
#else
static int
log_buf_thread_func(int _, void *_log_buf)
#endif
{
    struct log_buf *log_buf = (struct log_buf *)_log_buf;
    while (1) {
        char *bb = log_buf->backend_buf;
        size_t bb_len = 0, bb_cap = sizeof(log_buf->backend_buf);
        size_t nfinished = 0;
        struct log_packet *lp;
        int flags = -1;
        if (OSCompareAndSwapAtomic(&log_buf->overrun, 1, 0)) {
            bb_len = (size_t)snprintf(bb, bb_cap, "[OVERRUN]\n");
        }
        while ((lp = log_buf_shift_start(log_buf))) {
            if (flags == -1) {
                flags = lp->flags;
            } else {
                if (flags != lp->flags)
                    break;
            }
            if (bb_cap - bb_len < lp->len)
                break;
            memcpy(bb + bb_len, lp->data, lp->len);
            bb_len += lp->len;
            nfinished++;
            log_buf_shift_finish(log_buf);
        }
        if (flags == -1) {
            OSWaitEvent_mb(&log_buf->new_data_event);
            continue;
        }
        log_backend(bb, bb_len, flags);

        OSLockMutex(&log_buf->total_finished_logs_mtx);
        log_buf->total_finished_logs += nfinished;
        OSSignalCond(&log_buf->total_finished_logs_cond);
        OSUnlockMutex(&log_buf->total_finished_logs_mtx);
    }
}

static struct log_buf *
log_buf_init(void) {
    struct log_buf *log_buf = (struct log_buf *)MEMAllocFromDefaultHeapEx(sizeof(*log_buf), __alignof(*log_buf));
    ensure(log_buf);
    memset(log_buf, 0, sizeof(*log_buf));
    OSInitEvent(&log_buf->new_data_event, false, true);
    OSInitMutex(&log_buf->total_finished_logs_mtx);
    OSInitCond(&log_buf->total_finished_logs_cond);
#if DUMMY
    pthread_t thread;
    ensure(!pthread_create(&thread, nullptr, log_buf_thread_func, log_buf));
#else
    ensure(OSCreateThread(&log_buf->thread, (void *)log_buf_thread_func, 0, log_buf,
                          log_buf->thread_stack + sizeof(log_buf->thread_stack),
                          sizeof(log_buf->thread_stack),
                          16, // prio
                          8)); // detach
    OSSetThreadName(&log_buf->thread, "gdbstub log");
    ensure(OSResumeThread(&log_buf->thread));
#endif
    return log_buf;
}

void
log_buf(const char *s, size_t len, int flags) {
    struct log_buf *log_buf = g_log_buf;
    if (!len || !log_buf)
        return;
    struct log_packet *lp = log_buf_append_start(log_buf, len);
    if (!lp)
        return;
    lp->flags = flags;
    lp->len = (uint16_t)len;
    memcpy(lp->data, s, len);
    atomic_store_explicit(&lp->ready, (char)1, memory_order_release);
    OSSignalEvent_mb_if_safe(&log_buf->new_data_event);
}

void
_vlog_notimestamp(int flags, const char *fmt, my_va_list *map) {
    struct log_buf *log_buf = g_log_buf;
    if (!log_buf)
        return;

    my_va_list other = map->copy();

    int ret = min(svsnprintf(nullptr, 0, fmt, &other), 65535);
    if (ret <= 0)
        return;

    size_t len = (size_t)ret + 1;
    struct log_packet *lp = log_buf_append_start(log_buf, len);
    if (!lp)
        return;
    lp->flags = flags;
    int real_len = svsnprintf(lp->data, len, fmt, map);
    _ensure(LOG_NONE, real_len >= 0);
    lp->len = (uint16_t)min(real_len, ret);
    //printf("store(%p)\n", &lp->ready);
    atomic_store_explicit(&lp->ready, (char)1, memory_order_release);
    OSSignalEvent_mb_if_safe(&log_buf->new_data_event);
}
#else // LOG_USE_ASYNC
static_assert(DUMMY);
void
log_buf(const char *s, size_t len, int flags) {
    log_backend(s, len, flags);
}
void
_vlog_notimestamp(int flags, const char *fmt, my_va_list *map) {
    my_va_list map2 = map->copy();
    int ret = min(svsnprintf(nullptr, 0, fmt, &map2), 65535);
    if (ret <= 0)
        return;
    size_t len = (size_t)ret + 1;
    char *tmp = (char *)malloc(len);
    ensure(tmp);
    int real_len = svsnprintf(tmp, len, fmt, map);
    _ensure(LOG_NONE, real_len >= 0);
    _ensure(LOG_NONE, real_len <= ret);
    log_backend(tmp, (size_t)min(real_len, ret), flags);
    free(tmp);
}
#endif

void
_vlog(int flags, const char *fmt, my_va_list *map) {
    bool prepend_timestamp = true; // xxx
    if (prepend_timestamp) {
        uint64_t args[3] = {cur_time_us(), (uint64_t)(uintptr_t)fmt, (uint64_t)(uintptr_t)map};
        my_va_list map2 = my_va_list::custom(args, 3);
        _vlog_notimestamp(flags, "[%llu] `i%s%p", &map2);
    } else
        _vlog_notimestamp(flags, fmt, map);
}

void
_log(int flags, const char *fmt, ...) {
    my_va_list map;
    MVA_START(&map, fmt);
    _vlog(flags, fmt, &map);
}

static int log_port;
static int log_initted;
#if ENABLE_LOG_USBDUCKS
static struct usbducks_connection *log_conn;
#endif
#if ENABLE_LOG_NETWORK
static int log_sock = -1;
#endif

void
log_reset(void) {
#if ENABLE_LOG_NETWORK
    log_sock = -1;
#endif
#if ENABLE_LOG_USBDUCKS
    log_conn = nullptr;
#endif
    log_port = 0;
    log_initted = 0;
}

static void
log_backend(const char *s, size_t len, int flags) {
    flags &= log_initted;
#if ENABLE_LOG_NETWORK
    if (flags & LOG_NETWORK) {
        size_t off = 0;
        while (log_sock != -1 && off < len) {
            ssize_t ret = send(log_sock, s + off, len - off, 0);
            if (ret <= 0)
                break;
            off += (size_t)ret;
        }
    }
#endif
#if ENABLE_LOG_USBDUCKS
    if ((flags & LOG_USBDUCKS) && log_conn) {
        usbducks_backend_ensure_lock_not_owned(&g_usbducks);
        usbducks_backend_lock(&g_usbducks);
        size_t off = 0;
        while (off < len) {
            size_t ret = usbducks_send_sync(log_conn, s + off, len - off);
            usbducks_backend_lock(&g_usbducks);
            if (ret == 0)
                break;
            off += ret;
        }
        usbducks_backend_unlock(&g_usbducks);
    }
#endif
#if ENABLE_LOG_COS
    if (flags & LOG_COS) {
        COSError(2, "%.*s", (int)len, s);
    }
#endif
#if DUMMY
    fwrite(s, 1, len, stdout);
#endif
}

#if ENABLE_LOG_NETWORK
static int
connect_helper(const char *host, int port) {
    int ret, sock;
    socket_lib_init_wrapper();
    if (0 > (sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)))
        panic("socket(): %d [%d]", sock, socketlasterr());
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (1 != (ret = inet_aton(host, &sa.sin_addr)))
        panic("inet_aton(): %d", ret);
    if ((ret = connect(sock, &sa, sizeof(sa))))
        panic("connect(%s:%d): %d [%d]", host, port, ret, socketlasterr());
    send(sock, "connected\n", sizeof("connected\n") - 1, 0);
    return sock;
}
#endif

#if ENABLE_LOG_USBDUCKS
static void log_on_disconnect(struct usbducks_connection *conn) LOCKED;
static void
log_connect(struct usbducks *ud) LOCKED {
    struct usbducks_connection *conn = usbducks_connect(ud, (uint16_t)log_port);
    ensure(conn);
    conn->on_disconnect = log_on_disconnect;
    log_conn = conn;
}
void
log_on_disconnect(struct usbducks_connection *conn) {
    ensure(log_conn == conn);
    log_conn = nullptr;
    log_connect(conn->ud);
}
#endif

void
log_init(int port, int flags, struct usbducks *ud) {
    log_port = port;
#if LOG_USE_ASYNC
    g_log_buf = log_buf_init();
#endif
#if ENABLE_LOG_NETWORK
    if (flags & LOG_NETWORK) {
        log_sock = connect_helper(LOG_HOST, port);
        log_initted |= LOG_NETWORK;
    }
#endif
#if ENABLE_LOG_USBDUCKS
    if (flags & LOG_USBDUCKS) {
        ensure(ud);
        usbducks_backend_lock(ud);
        log_connect(&g_usbducks);
        usbducks_backend_unlock(&g_usbducks);
        log_initted |= LOG_USBDUCKS;
    }
#endif
#if ENABLE_LOG_COS
    if (flags & LOG_COS)
        log_initted |= LOG_COS;
#endif
}

void
_log_flush(int flags) {
    flags &= log_initted;
#if LOG_USE_ASYNC
    struct log_buf *log_buf = g_log_buf;
    if (log_buf
#if !DUMMY
        && OSGetCurrentThread() != &log_buf->thread
#endif
    ) {
        uint32_t total_queued_logs = atomic_load_explicit(&log_buf->total_queued_logs, memory_order_relaxed);
        OSLockMutex(&log_buf->total_finished_logs_mtx);
        //COSError(2, "lf(%x): total_finished_logs=%u total_finished_logs=%u\n", flags, log_buf->total_finished_logs, total_queued_logs);
        while (log_buf->total_finished_logs < total_queued_logs)
            OSWaitCond(&log_buf->total_finished_logs_cond, &log_buf->total_finished_logs_mtx);
        OSUnlockMutex(&log_buf->total_finished_logs_mtx);
    }
#endif
#if ENABLE_LOG_NETWORK
    if ((flags & LOG_NETWORK) && log_sock != -1) {
        usleep(100000);
    }
#endif
#if DUMMY
    fflush(stdout);
#endif
}

void
log_str(const char *s, int flags) {
    log_buf(s, strlen(s), flags);
}
#endif // LOG_ENABLED

#if ENABLE_USBDUCKS
struct usbducks g_usbducks = {};

struct sendsync {
    OSEvent done;
    size_t actual;
};

static void
on_send_complete_sendsync(struct usbducks_connection *conn, size_t actual, uint64_t time) LOCKED {
    if (USBDUCKS_VERBOSE >= 2)
        _log(LOG_NOUSBDUCKS, "on_send_complete_sendsync(conn=%p, actual=%zu) after %lluus\n", conn, actual, time);
    struct sendsync *ss = (struct sendsync *)conn->user2;
    conn->on_send_complete = nullptr;
    conn->user2 = nullptr;
    ss->actual = actual;
    OSSignalEvent_mb(&ss->done);
}

size_t
usbducks_send_sync(struct usbducks_connection *conn, const void *buf, size_t len) {
    struct usbducks *ud = conn->ud;
#if !DUMMY
    if (OSGetCurrentThread() == &ud->be.main_loop_thread)
        _panic(LOG_NONE, "usbducks_send_sync on usbducks thread");
#endif
    if (USBDUCKS_VERBOSE)
        _log(LOG_NOUSBDUCKS, "usbducks_send_sync(conn=%p, %u bytes) start...\n", conn,
             (int)len);
    uint64_t start_time = cur_time_us();
    struct sendsync ss;
    OSInitEvent(&ss.done, false, true);
    conn->user2 = &ss;
    conn->on_send_complete = on_send_complete_sendsync;
    usbducks_send_async(conn, buf, len);
    usbducks_start_transfers_if_necessary(&g_usbducks);
    usbducks_backend_unlock(ud);
    if (USBDUCKS_VERBOSE)
        _log(LOG_NOUSBDUCKS, "usbducks_send_sync(conn=%p, %u bytes) waiting...\n", conn,
             (int)len);
    while (1) {
        if (OSWaitEventWithTimeout(&ss.done, 1000 * (uint64_t)1000000))
            break;
        _log(LOG_NOUSBDUCKS,
             "usbducks_send_sync(conn=%p) still waiting after 1000ms...\n", conn);
    }
    OSMemoryBarrier();
    if (USBDUCKS_VERBOSE)
        _log(LOG_NOUSBDUCKS, "usbducks_send_sync(conn=%p) done after %lluus, actual=%zu\n", conn,
             cur_time_us() - start_time, ss.actual);
    return ss.actual;
}
#endif

#if !DUMMY
static const char *
build_id_str(void) {
#ifdef FIXED_BUILD_ID
    return "[build-id: " FIXED_BUILD_ID "]";
#else
    return "[build-id: unknown]";
#endif
}
#endif

#define LOG_STACKBUF_SIZE (DUMMY ? 8192 : 512)
void
_panic(int flags, const char *fmt, ...) {
    my_va_list map;
    MVA_START(&map, fmt);
    #if DUMMY
        char buf[LOG_STACKBUF_SIZE];
        ssnprintf(buf, sizeof(buf), "panic: `i%s%p\n", fmt, &map);
        fprintf(stderr, "%s", buf);
        #ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
            abort();
        #else
            exit(1);
        #endif
    #else
        char buf[LOG_STACKBUF_SIZE];
        int len = ssnprintf(buf, sizeof(buf), "panic: `i%s%p\n%s\n", fmt, &map, build_id_str());
        if (OSIsInterruptEnabled()) {
            log_buf(buf, min((size_t)len, sizeof(buf) - 1), flags);
            _log_flush(LOG_ALL);
        }
        while (1)
            OSFatal_ptr(buf);
    #endif
}

void
__ensure(bool ok, int flags, const char *str) {
    if (!ok)
        _panic(flags, "ensure failed %s", str);
}

#if !DUMMY
bool
panic_exc_handler(OSContext *ctx) {
    char buf[64 * 11];
    char *p = buf, *end = buf + sizeof(buf);
    usprintf(&p, end, "exc %d LR: %08x PC: %08x SRR1: %08x CTR: %08x\n",
             ctx->exception_type, ctx->lr, ctx->srr0, ctx->srr1, ctx->ctr);
    for (int i = 0; i < 32; i += 4) {
        usprintf(&p, end, "r%2d: %08x r%2d: %08x r%2d: %08x r%2d: %08x\n", i + 0,
                 ctx->gpr[i + 0], i + 1, ctx->gpr[i + 1], i + 2, ctx->gpr[i + 2], i + 3,
                 ctx->gpr[i + 3]);
    }
    usprintf(&p, end, "DAR:%08x DSISR:%08x\n%s\ntext_start:%08x data_start:%08x\n",
             ctx->ex1, ctx->ex0, build_id_str(), (uint32_t)self_elf_start,
             (uint32_t)data_start);
    //usprintf(&p, end, "thread:%s", OSGetThreadName(OSGetCurrentThread()) ?: "(null)");

    while (1)
        OSFatal(buf);
}

void
install_exc_handler(void (*x_OSSetExceptionCallbackEx)(int, int, bool (*)(OSContext *))) {
    x_OSSetExceptionCallbackEx(ALL_CORES, DSI, panic_exc_handler);
    x_OSSetExceptionCallbackEx(ALL_CORES, ISI, panic_exc_handler);
    x_OSSetExceptionCallbackEx(ALL_CORES, PROGRAM, panic_exc_handler);
}
#endif

#if LOGTEST

static void *logtest_thread(void *_i) {
    while (1) {
        log("AAA %d\n", (int)_i);
        sleep(1);
    }
}
int main(int argc, char **argv) {
    log_init(0, 0);
    if (argc > 1)
        panic("panic test");
    for (size_t i = 0; i < 16; i++) {
        pthread_t t;
        pthread_create(&t, nullptr, logtest_thread, (void *)i);
    }
    while (1) {
        log("z\n");
        sleep(1);
    }
}
#endif

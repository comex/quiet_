#pragma once
#include "decls.h"
#include "types.h"
#include "ssprintf.h"

#if DUMMY
#define LOG_ENABLED 1
#else
#define LOG_ENABLED (1 && !RELEASE_MODE)
#endif

#define LOG_HOST "192.168.1.131"

enum {
    LOG_NONE = 0,
    LOG_NETWORK = 1,
    LOG_USBDUCKS = 2,
    LOG_COS = 4,
    LOG_ALL = LOG_NETWORK | LOG_USBDUCKS | LOG_COS,

    LOG_DEFAULT = LOG_ALL & ~LOG_COS,
    LOG_NOUSBDUCKS = LOG_DEFAULT & ~LOG_USBDUCKS,
};
#define LOG_FLAGS_TO_USE (THIS_C_FILE_IS_USBDUCKS_RELATED ? LOG_NOUSBDUCKS : LOG_DEFAULT)
#define THIS_C_FILE_IS_USBDUCKS_RELATED 0

#if LOG_ENABLED
__attribute__((format(printf, 2, 3)))
void _log(int flags, const char *fmt, ...);
__attribute__((format(printf, 2, 0)))
void _vlog(int flags, const char *fmt, my_va_list *map);
__attribute__((format(printf, 2, 0)))
void _vlog_notimestamp(int flags, const char *fmt, my_va_list *map);

void log_reset(void);
void log_str(const char *s, int flags);
void log_buf(const char *s, size_t len, int flags);
void _log_flush(int flags);
void log_init(int port, int flags, struct usbducks *ud);
#else
__attribute__((format(printf, 2, 3)))
static inline void
log(int flags, const char *fmt, ...) {}
__attribute__((format(printf, 2, 0)))
static inline void
_vlog(int flags, const char *fmt, my_va_list *map) {}
__attribute__((format(printf, 2, 0)))
static inline void
_vlog_notimestamp(int flags, const char *fmt, my_va_list *map) {}

static inline void
log_reset(void) {}
static inline void
log_str(const char *s, int flags) {}
static inline void
log_buf(const char *s, size_t len, int flags) {}
static inline void
_log_flush(int flags) {}
static inline void
log_init(int port) {}
#endif

#define log_flush() _log_flush(LOG_FLAGS_TO_USE)
#define log(...) _log(LOG_FLAGS_TO_USE, __VA_ARGS__)

#if DUMMY
#define debug_ensure ensure
#else
#define debug_ensure(x) ((void)0)
#endif

#define ensure(x...) _ensure(LOG_FLAGS_TO_USE, x)
#define _ensure(flags, x...) _ensure_(flags, __FILE__, __LINE__, x)
#define _ensure_(flags, file, line, x...) _ensure__(flags, file, line, x)
#define _ensure__(flags, file, line, x...)                                                  \
    __ensure((x), (flags), "(" file ":" #line "): " #x)

#define ensure_eq(x, y) ensure_op(x, ==, y)

#define ensure_op(x, op, y)                                                              \
    ({                                                                                   \
        auto _eox = (x);                                                          \
        auto _eoy = (y);                                                          \
        if (!(_eox op _eoy))                                                             \
            panic("ensure_op failed (%s:%d): %s (%#llx) " #op " %s (%#llx)", __FILE__,     \
                  __LINE__, #x, (long long)_eox, #y, (long long)_eoy);                   \
        (void)0;                                                                         \
    })

#if DUMMY
#define ensure_errno(x)                                                                  \
    ({                                                                                   \
        if (!(x)) {                                                                      \
            panic("ensure failed (%s:%d): %s\n%s", __FILE__, __LINE__, #x,               \
                  strerror(errno));                                                      \
        }                                                                                \
        (void)0;                                                                         \
    })

#endif

[[noreturn]] void _panic(int flags, const char *fmt, ...);
#define panic(...) _panic(LOG_FLAGS_TO_USE, __VA_ARGS__)

void __ensure(bool ok, int flags, const char *str);

#if !DUMMY
bool panic_exc_handler(OSContext *ctx);
void install_exc_handler(void (*x_OSSetExceptionCallbackEx)(int, int,
                                                            bool (*)(OSContext *)));
#endif

#if ENABLE_USBDUCKS
#include "usbducks.h"

// misc usbducks stuff, not actually logging-specific
extern struct usbducks g_usbducks;

size_t usbducks_send_sync(struct usbducks_connection *conn, const void *buf, size_t len)
    RELEASE(_ud_mutex);
#endif

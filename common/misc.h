#pragma once

#include "decls.h"
#include "types.h"
#include "ssprintf.h"

BEGIN_LOCAL_DECLS

#define snprintf ssnprintf

#define INLINE __attribute__((always_inline)) inline

#if !defined(__has_attribute) || __has_attribute(no_icf)
    #define ATTR_NO_ICF __attribute__((no_icf))
#else
    #define ATTR_NO_ICF
#endif

#if !defined(__has_attribute) || __has_attribute(noclone)
    #define ATTR_NOCLONE __attribute__((noclone))
#else
    #define ATTR_NOCLONE
#endif

#if !defined(__has_attribute) || __has_attribute(noipa)
    #define ATTR_NOIPA __attribute__((noipa))
#else
    #define ATTR_NOIPA __attribute__((noinline)) ATTR_NO_ICF ATTR_NOCLONE
#endif

template <typename T, typename U>
struct __assert_types_equal;
template <typename T>
struct __assert_types_equal<T, T> {};
#define _assert_types_equal(a, b) __assert_types_equal<a, b>()

template <typename T, typename U>
struct __types_are_equal { static constexpr bool val = false; };
template <typename T>
struct __types_are_equal<T, T> { static constexpr bool val = true; };

template <typename T>
inline T
max(T a, T b) {
    return a > b ? a : b;
}
template <typename T>
inline T
min(T a, T b) {
    return a < b ? a : b;
}

#define usprintf(pp, end, args...)                                                      \
    ({                                                                                   \
        char **_pp = (pp), *_p = *_pp;                                                   \
        char *_end = (end);                                                              \
        size_t _avail = (size_t)(_end - _p);                                             \
        int _len = ssnprintf(_p, _avail, args); \
        bool _fits = _len >= 0 && _len < _avail; /* < due to \0 */                       \
        *_pp = _fits ? (_p + _len) : (_end - 1);                                         \
        _fits;                                                                           \
    })

template <typename T>
T actually_swap(T val) {
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
    if constexpr(sizeof(T) == 1)
        return val;
    if constexpr(sizeof(T) == 2)
        return (T)__builtin_bswap16((uint16_t)val);
    if constexpr(sizeof(T) == 4)
        return (T)__builtin_bswap32((uint32_t)val);
    if constexpr(sizeof(T) == 8)
        return (T)__builtin_bswap64((uint64_t)val);
}
#if DUMMY
// todo improve naming, lol
#define bswap(x) actually_swap(x)
#define bswap_le(x) (x)
#else
#define bswap(x) (x)
#define bswap_le(x) actually_swap(x)
#endif

uint32_t get_branch(uint32_t src, uint32_t dst);
uint32_t get_ba(uint32_t dst);
uint32_t adjust_branch(uint32_t orig, uint32_t src, uint32_t dst);

void install_hook(const char *foo_name, void *foo, void *hook_foo, void *orig_foo);
void uninstall_hook(const char *foo_name, void *foo, void *hook_foo, void *orig_foo);

void install_hooks(const struct func_hook_info *list);
void uninstall_hooks(const struct func_hook_info *list);

template <char... chars>
struct typestring {};
template <typename Char, Char... chars>
constexpr typestring<chars...> operator "" _typestring() {
    return typestring<chars...>();
}

struct func_hook_info {
    const char *name;
    void *fptr_or_ptrptr;
    void *hook;
    void *orig;
    int flags;
};

static constexpr int HOOK_VALID = 1;
static constexpr int HOOK_FPTR_IS_PTRPTR = 2;
template <typename NameTS, typename FTy>
struct func_hook;

#if __has_attribute(naked)
    #define IF_SUPPORTS_NAKED(x...) x
    #define IF_NOT_SUPPORTS_NAKED(x...)
#else
    #define IF_SUPPORTS_NAKED(x...)
    #define IF_NOT_SUPPORTS_NAKED(x...) x
#endif

#if !DUMMY

// This is stupid: C++ parameter packs can't include the ... at the end of a
// variadic function declaration, so there have to be two copies of this
// definition, one with and one without.  Hence the macro.
#define _DEF_FUNC_HOOK(dot_dot_dot...) \
    template<typename NameTS, typename Ret, typename... Args> \
    struct func_hook<NameTS, SDKCALL Ret(Args... dot_dot_dot)> { \
        static SDKCALL Ret hook(Args... dot_dot_dot); \
\
        __attribute__((used)) \
        __attribute__((section(".orig"))) \
        ATTR_NOIPA \
        IF_SUPPORTS_NAKED(__attribute__((naked))) \
        static SDKCALL Ret orig(Args... dot_dot_dot) { \
            asm volatile( \
                "trap; trap; trap; trap; trap; trap; trap\n" \
            ); \
            IF_NOT_SUPPORTS_NAKED(return ((Ret (*)())1)();) \
        } \
    };

_DEF_FUNC_HOOK()
_DEF_FUNC_HOOK(, ...)

#define make_fptrptr_hook(namets, name, fptr) \
    func_hook_info{ \
        (name), \
        (void *)(fptr), \
        (void *)&decltype(make_fptrptr_hook_helper(namets, fptr))::hook, \
        (void *)&decltype(make_fptrptr_hook_helper(namets, fptr))::orig, \
        HOOK_VALID | HOOK_FPTR_IS_PTRPTR \
    }
template <typename NameTS, typename FTy>
constexpr func_hook<NameTS, FTy>
make_fptrptr_hook_helper(NameTS namets, FTy **fptr) { while (1); }

#define make_func_hook(namets, name, fptr) \
    func_hook_info{ \
        (name), \
        (void *)(fptr), \
        (void *)&decltype(make_func_hook_helper(namets, fptr))::hook, \
        (void *)&decltype(make_func_hook_helper(namets, fptr))::orig, \
        HOOK_VALID \
    }
template <typename NameTS, typename FTy>
constexpr func_hook<NameTS, FTy>
make_func_hook_helper(NameTS namets, FTy *fptr) { while (1); }

#define FUNC_HOOK(name) \
    make_func_hook(#name##_typestring, #name, &name)
#define FUNC_HOOK_TY(name) \
    func_hook<decltype(#name##_typestring), decltype(name)>

#endif // DUMMY

#include "sinvals.h"
static inline int
fixed_sin(int value) {
    int idx = value & 0x3f;
    int ret = sinvals[(value & 0x40) ? (0x3f - idx) : idx];
    return (value & 0x80) ? -ret : ret;
}

#if !DUMMY
END_LOCAL_DECLS
#include "kern_garbage.h"
#include "logging.h"
BEGIN_LOCAL_DECLS
#endif

void
dump_my_ips(void);

#ifdef __clang__
#define FALLTHROUGH [[fallthrough]]
#else
#define FALLTHROUGH __attribute__((fallthrough))
#endif

extern void __not_unreachable_i_guess(void);
#define ENSURE_UNREACHABLE __not_unreachable_i_guess()

static inline void
OSSignalEvent_mb(OSEvent *event) {
    OSMemoryBarrier();
    OSSignalEvent(event);
}
static inline void
OSWaitEvent_mb(OSEvent *event) {
    OSWaitEvent(event);
    OSMemoryBarrier();
}

#define sat_add(a, b)                                                                    \
    ({                                                                                   \
        size_t _sa_out;                                                                  \
        __builtin_add_overflow((a), (b), &_sa_out) ? (size_t)-1 : _sa_out;               \
    })
#define sat_mul(a, b)                                                                    \
    ({                                                                                   \
        size_t _sa_out;                                                                  \
        __builtin_mul_overflow((a), (b), &_sa_out) ? (size_t)-1 : _sa_out;               \
    })

#define UNUSED __attribute__((unused))

void inval_dc(volatile void *dst, size_t size);
void flush_dc(const volatile void *dst, size_t size);
void inval_ic(volatile void *dst, size_t size);

void xmemcpy(volatile void *a, const volatile void *b, size_t size);
int xmemcmp(const volatile void *a, const volatile void *b, size_t size);
void xbzero(volatile void *a, size_t size);

// relative to an arbitrary base; might wrap
uint64_t cur_time_us(void);

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#ifdef __cplusplus
    #define xtypeof(x) typename remove_reference<decltype(x)>::type
#else
    #define xtypeof(x) typeof(x)
#endif

#include "logging.h"

template <typename T, size_t n>
static constexpr size_t _countof2(T (*array)[n]) {
    return n;
}
template <typename A>
static constexpr size_t _countof(A *array) {
    // lol, GCC treats 0-size arrays very weirdly
    if constexpr(sizeof(A) == 0)
        return 0;
    else
        return _countof2(array);
}
#define countof(array) _countof(&(array))


template <typename T, typename U>
static inline T *
downcast(U *val) {
#if DUMMY
    T *t = dynamic_cast<T *>(val);
    if (!t)
        panic("bad cast");
    return t;
#else
    return static_cast<T *>(val);
#endif
}

template <typename T>
static inline T black_box(T val) {
    asm volatile("" :: "r"(&val) : "memory");
    return val;
}

void socket_lib_init_wrapper(void);

#define offsetof_stfu(ty, field) \
    ((size_t)(((uintptr_t)&((typeof(ty) *)0x10000)->field) - 0x10000))

END_LOCAL_DECLS

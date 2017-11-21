#pragma once
#if DUMMY
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#else
typedef __INT8_TYPE__ int8_t;
typedef __UINT8_TYPE__ uint8_t;
typedef __INT16_TYPE__ int16_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __INT32_TYPE__ int32_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __INT64_TYPE__ int64_t;
typedef __UINT64_TYPE__ uint64_t;
#ifndef __cplusplus
    typedef _Bool bool;
#endif
typedef __SIZE_TYPE__ size_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __INTPTR_TYPE__ intptr_t, ssize_t;
#define UINT_MAX (-1u)
#define INT_MAX (0x7fffffff)
#define INT64_MIN ((int64_t)0x8000000000000000)

// clang-format off
#define sizeof(foo) ((size_t)sizeof(foo))
// clang-format on

typedef __builtin_va_list va_list;
#endif

#ifndef __has_feature
    #define __has_feature(x) 0
#endif

#define USE_THREAD_SAFETY_ANALYSIS __clang__

#if USE_THREAD_SAFETY_ANALYSIS
#define THREAD_ANNOTATION_ATTRIBUTE__(x) __attribute__((x))
#else
#define THREAD_ANNOTATION_ATTRIBUTE__(x)
#endif

#define REQUIRES(...) THREAD_ANNOTATION_ATTRIBUTE__(requires_capability(__VA_ARGS__))
#define PT_GUARDED_BY(x) THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_by(x))
#define GUARDED_BY(x) THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))
#define ACQUIRE(...) THREAD_ANNOTATION_ATTRIBUTE__(acquire_capability(__VA_ARGS__))
#define RELEASE(...) THREAD_ANNOTATION_ATTRIBUTE__(release_capability(__VA_ARGS__))
#define NO_THREAD_SAFETY_ANALYSIS THREAD_ANNOTATION_ATTRIBUTE__(no_thread_safety_analysis)

#define restrict __restrict
#ifndef NULL
#define NULL nullptr
#endif

#if defined(__cplusplus)
template <typename T> struct remove_reference { typedef T type;};
template <typename T> struct remove_reference<T &> { typedef T type; };
template <typename T> struct remove_reference<T &&> { typedef T type; };
template <typename T>
static inline typename remove_reference<T>::type &&
move(T &&t) {
    return (typename remove_reference<T>::type &&)t;
}
template<typename T>
static inline typename remove_reference<T>::type &&
forward(T &t) {
    return (typename remove_reference<T>::type &&)t;
}

template <bool, typename T = void> struct enable_if;
template <typename T> struct enable_if<true, T> { typedef T type; };

#if !defined(__GNUC__)
    template <typename T, typename U> struct _is_same { constexpr bool value = false; }
    template <typename T> struct _is_same<T, T> { constexpr bool value = true; }
    #define __is_same(T, U) _is_same<T, U>::value
#elif !defined(__clang__)
    #define __is_same __is_same_as
#endif

#if !defined(__clang__)
    template <typename T> struct _is_pointer { static constexpr bool value = false; };
    template <typename T> struct _is_pointer<T *> { static constexpr bool value = true; };
    #define __is_pointer(T) _is_pointer<T>::value
#endif

extern void *_declval_should_not_be_evaluated(void);
template<class T>
static inline T&& declval() {
    return (T&&)*(T *)_declval_should_not_be_evaluated();
}

#if !__has_feature(is_convertible_to)
    template <typename From, typename To, typename = void> struct is_convertible {
        static constexpr bool value = false;
    };
    template <typename From, typename To> struct is_convertible<From, To, decltype(declval<void (*)(To)>()(declval<From>()))> {
        static constexpr bool value = true;
    };
    #define __is_convertible_to(From, To) (is_convertible<From, To>::value)
    static_assert(!__is_convertible_to(char *, int));
    static_assert(__is_convertible_to(char *, void *));
    struct _ictest;
    static_assert(__is_convertible_to(struct _ictest *, const struct _ictest *));
#endif

#ifndef __clang__
    template <typename T>
    struct xx_is_integral { constexpr static bool value = false; };
    template <> struct xx_is_integral<bool> { constexpr static bool value = true; };
    template <> struct xx_is_integral<char> { constexpr static bool value = true; };
    template <> struct xx_is_integral<signed char> { constexpr static bool value = true; };
    template <> struct xx_is_integral<unsigned char> { constexpr static bool value = true; };
    template <> struct xx_is_integral<short> { constexpr static bool value = true; };
    template <> struct xx_is_integral<unsigned short> { constexpr static bool value = true; };
    template <> struct xx_is_integral<int> { constexpr static bool value = true; };
    template <> struct xx_is_integral<unsigned int> { constexpr static bool value = true; };
    template <> struct xx_is_integral<long> { constexpr static bool value = true; };
    template <> struct xx_is_integral<unsigned long> { constexpr static bool value = true; };
    template <> struct xx_is_integral<long long> { constexpr static bool value = true; };
    template <> struct xx_is_integral<unsigned long long> { constexpr static bool value = true; };
    #define __is_integral(T) xx_is_integral<T>::value
    #define __is_signed(T) ((T)-1 < (T)0)
#endif

#if DUMMY
    #include <new>
#else
    inline void *
    operator new(size_t, void *p) noexcept {
        return p;
    }
#endif
#endif

#ifdef __clang__
    #define TRIVIAL_ABI [[clang::trivial_abi]]
#elif NOTYET
    #define TRIVIAL_ABI [[move_relocates]]
#else
    #define TRIVIAL_ABI
#endif

#if ENABLE_SWIFTCALL && !DUMMY
    #define BEGIN_LOCAL_DECLS _Pragma("clang attribute push(__attribute__((swiftcall)), apply_to = function_type)")
    #define END_LOCAL_DECLS _Pragma("clang attribute pop")
    #define SDKCALL __attribute__((defaultcall))
#else
    #define BEGIN_LOCAL_DECLS
    #define END_LOCAL_DECLS
    #define SDKCALL
#endif

template <typename T>
struct manually_construct {
    alignas(T) char storage[sizeof(T)];
    constexpr T &operator*() { return *(T *)storage; }
    constexpr T *operator->() { return (T *)storage; }
};

template <size_t a, size_t b>
struct __assert_sizes_equal;
template <size_t a>
struct __assert_sizes_equal<a, a> {};
#define CHECK_SIZE(ty, siz)                                                              \
    static_assert(!(__assert_sizes_equal<sizeof(ty), siz> *)0)

template <typename F>
struct defer {
    constexpr inline defer(F f) : f(move(f)) {}
    inline ~defer() { f(); }
    F f;
};

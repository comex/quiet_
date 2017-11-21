#pragma once
#include "types.h"
#include "decls.h"

#if DUMMY
    #include <stdarg.h>
#else
    #define va_start __builtin_va_start
    #define va_copy __builtin_va_copy
    #define va_arg __builtin_va_arg
    #define va_end __builtin_va_end
#endif

#define MVA_START(mapp, arg) do { \
    va_list _ap; \
    va_start(_ap, arg); \
    *(mapp) = my_va_list::with_ap(_ap); \
    va_end(_ap); \
} while (0)

struct my_va_list {
    bool is_custom;
    union {
        va_list ap;
        struct {
            const uint64_t *custom_args;
            size_t custom_args_count;
        };
    };

    inline ~my_va_list() {
        if (!is_custom)
            va_end(ap);
    }
    inline my_va_list copy() {
        my_va_list ret;
#if DUMMY
        ret.is_custom = is_custom;
        if (is_custom) {
            ret.custom_args = custom_args;
            ret.custom_args_count = custom_args_count;
        } else
            va_copy(ret.ap, ap);
#else
        memcpy(&ret, this, sizeof(my_va_list));
#endif
        return ret;
    }
    static inline my_va_list with_ap(va_list ap) {
        my_va_list ret;
        ret.is_custom = false;
        va_copy(ret.ap, ap);
        return ret;
    }
    static inline my_va_list custom(const uint64_t *args, size_t args_count) {
        my_va_list ret;
        ret.is_custom = true;
        ret.custom_args = args;
        ret.custom_args_count = args_count;
        return ret;
    }
    template <typename T>
    inline T arg() {
        if (!is_custom) {
            return va_arg(ap, T);
        } else {
            if (!custom_args_count)
                return 0;
            custom_args_count--;
            T ret;
            convert_custom_arg(*custom_args++, &ret);
            return ret;
        }
    }
    template <typename T>
    inline void convert_custom_arg(uint64_t val, T *out) {
        *out = (T)val;
    }
    inline void convert_custom_arg(uint64_t val, double *out) {
        memcpy(out, &val, 8);
    }
};


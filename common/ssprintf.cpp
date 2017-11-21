#include "ssprintf.h"
#include "decls.h"
#include "misc.h"
#include "logging.h"
#include "my_stdarg.h"

// avoid recursion if snprintf is defined as ssnprintf:
#undef snprintf

static bool
s_parse_dec_str(const char **sp, unsigned int *outp) {
    const char *s = *sp;
    unsigned int out = 0;
    bool ok = false;
    while (*s >= '0' && *s <= '9') {
        out = (out * 10) + (unsigned int)(*s++ - '0');
        ok = true;
    }
    *sp = s;
    *outp = out;
    return ok;
}

static inline char *
s_format_int(char *buf_end, char *buf_start, uint64_t val, unsigned int base, bool is_signed, const char *digits, unsigned int prec, bool alt_form) {
    bool minus = is_signed && (val & ((uint64_t)1 << 63));
    char *orig_buf_end = buf_end;
    if (minus)
        val = -val;
    do {
        if (buf_end == buf_start)
            goto bad;
        *--buf_end = digits[val % base];
        val /= base;
    } while (val);
    while ((size_t)(orig_buf_end - buf_end) < prec) {
        if (buf_end == buf_start)
            goto bad;
        *--buf_end = '0';
    }
    if (alt_form && base == 16) {
        if (buf_end - buf_start < 2)
            goto bad;
        *--buf_end = 'x';
        *--buf_end = '0';
    }
    if (minus) {
        if (buf_end == buf_start)
            goto bad;
        *--buf_end = '-';
    }
    return buf_end;
bad:
    panic("s_format_int: overflow");
}

int
svsnprintf(char *buf, size_t len, const char *format, my_va_list *map) {
    size_t out_off = 0;
#define PUTC(c) do { \
    char _c = (c); \
    if (out_off < len) { \
        buf[out_off] = _c; \
    } \
    out_off++; \
} while (0)

    while (*format) {
        char percent = *format++;
        if (percent == '`') {
            // indirect mode.  syntax:
            // printf("`i%s%p", sub_fmt, sub_map);
            // using this weird syntax to placate the compiler's printf format checker
            ensure(*format++ == 'i');
            ensure(*format++ == '%');
            ensure(*format++ == 's');
            ensure(*format++ == '%');
            ensure(*format++ == 'p');
            const char *sub_fmt = (const char *)map->arg<uintptr_t>();
            my_va_list *sub_map = (my_va_list *)map->arg<uintptr_t>();
            my_va_list sub_map_copy = sub_map->copy();
            int sub_ret;
            if (out_off < len)
                sub_ret = svsnprintf(buf + out_off, len - out_off, sub_fmt, &sub_map_copy);
            else
                sub_ret = svsnprintf(nullptr, 0, sub_fmt, &sub_map_copy);
            out_off += (size_t)sub_ret;
            continue;
        }
        if (percent != '%') {
            PUTC(percent);
            continue;
        }
        if (*format == '%') {
            PUTC(*format++);
            continue;
        }
        char pad = ' ';
        bool alt_form = false;
        while (1) {
            if (*format == '0') {
                pad = '0';
                format++;
            } else if (*format == '#') {
                alt_form = true;
                format++;
            } else {
                break;
            }
        }
        unsigned int min_width = 0, prec = 0;
        bool have_prec = false;
        s_parse_dec_str(&format, &min_width);
        if (*format == '.') {
            format++;
            if (*format == '*') {
                format++;
                prec = map->arg<uint32_t>();
                have_prec = true;
            } else
                have_prec = s_parse_dec_str(&format, &prec);
        }
        size_t size = sizeof(int);
        if (*format == 'z') {
            format++;
            size = sizeof(size_t);
        } else if (*format == 'l') {
            format++;
            size = sizeof(long);
            if (*format == 'l') {
                format++;
                size = sizeof(long long);
            }
        }
        bool is_signed = false;
        unsigned int base = 10;
        const char *digits = "0123456789abcdef";
        char tmp_buf[32];
        const char *value_str;
        size_t value_str_len;
        uint64_t val;
        char conversion = *format++;
        switch (conversion) {
        case 'f': {
            double d = map->arg<double>();
            // cheat
            value_str = tmp_buf;
            if (have_prec)
                value_str_len = (size_t)__os_snprintf(tmp_buf, sizeof(tmp_buf), "%.*f", (int)prec, d);
            else
                value_str_len = (size_t)__os_snprintf(tmp_buf, sizeof(tmp_buf), "%f", d);
            goto print_it;
        }
        case 'c': {
            tmp_buf[0] = (char)map->arg<uint32_t>();
            value_str = tmp_buf;
            value_str_len = 1;
            goto print_it;
        }
        case 's': {
            if constexpr(sizeof(const char *) == 8)
                value_str = (const char *)map->arg<uint64_t>();
            else
                value_str = (const char *)(uintptr_t)map->arg<uint32_t>();
            if (!value_str)
                value_str = "(null)";
            value_str_len = have_prec ? strnlen(value_str, prec) : strlen(value_str);
            goto print_it;
        }
        case 'd':
        case 'i':
            is_signed = true;
            goto integer;
        case 'u':
            goto integer;
        case 'p':
            alt_form = true;
            base = 16;
            size = sizeof(void *);
            goto integer;
        case 'x':
            base = 16;
            goto integer;
        case 'X':
            base = 16;
            digits = "0123456789ABCDEF";
            goto integer;
        default:
            panic("bad conversion '%c'", conversion); // recursive
        }
    integer:;
        val = size == 8 ? map->arg<uint64_t>() : map->arg<uint32_t>();
        if (is_signed && size != 8 && (val & 0x80000000))
            val |= 0xffffffff00000000;
        value_str = s_format_int(tmp_buf + sizeof(tmp_buf), tmp_buf, val, base, is_signed, digits, prec, alt_form);
        value_str_len = (size_t)(tmp_buf + sizeof(tmp_buf) - value_str);
    print_it:
        while (min_width > value_str_len) {
            PUTC(pad);
            min_width--;
        }
        while (value_str_len--)
            PUTC(*value_str++);
    }
    PUTC('\0');
    return (int)out_off - 1;
#undef PUTC
}

int
ssnprintf(char *buf, size_t len, const char *format, ...) {
    my_va_list map;
    MVA_START(&map, format);
    int ret = svsnprintf(buf, len, format, &map);
    return ret;
}

#if SSPRINTFTEST
static int fail;
__attribute__((format(printf, 1, 2)))
static void
test(const char *format, ...) {
    va_list ap, ap2;
    va_start(ap, format);
    va_copy(ap2, ap);
    my_va_list map = my_va_list::with_ap(ap);
    char my_buf[1024];
    char reference_buf[sizeof(my_buf) - 16];
    memset(my_buf, 'X', sizeof(my_buf) - 1);
    my_buf[sizeof(my_buf) - 1] = '\0';
    vsnprintf(reference_buf, sizeof(reference_buf), format, ap2);
    int ret = svsnprintf(my_buf, sizeof(my_buf) - 16, format, &map);
    ensure_eq(ret, strlen(my_buf));
    if (strcmp(reference_buf, my_buf)) {
        log("test fail: expected [%s] got [%s] with format [%s]\n", reference_buf, my_buf, format);
        fail = 1;
    }
    va_end(ap2);
    va_end(ap);
}

__attribute__((format(printf, 2, 3)))
static void
test_onesided(const char *expected, const char *format, ...) {
    my_va_list map;
    MVA_START(&map, format);
    char my_buf[1024];
    memset(my_buf, 'X', sizeof(my_buf) - 1);
    my_buf[sizeof(my_buf) - 1] = '\0';
    int ret = svsnprintf(my_buf, sizeof(my_buf) - 16, format, &map);
    ensure_eq(ret, strlen(my_buf));
    if (strcmp(expected, my_buf)) {
        log("test fail: expected [%s] got [%s] with format [%s]\n", expected, my_buf, format);
        fail = 1;
    }
}


int main() {
    test("%%");
    test("%c", 'a');
    test("%d", 123);
    test("%s", "hello");
    test("%.*s", 4, "hello");
    test("%.*s", 42, "hello");
    int numbers[] = {123, -123, 12345, -12345, 1234567, -1234567};
    for (size_t i = 0; i < countof(numbers); i++) {
        test("%5.3d", numbers[i]);
        test("%3.5d", numbers[i]);
        test("%.3d", numbers[i]);
        test("%.5d", numbers[i]);
    }
    test("%x", 0x1234);
    test("%X", 0x1234);
    test("%X", -0x1234);
    test("%#x", 0x1234);
    test("%#2.3x", 1);
    test("%#3.2x", 1);
    test("%f", 0.123);
    test("%10f", 0.123);
    test("%10.5f", 0.123);
    test("%10.5f", 0.1234578475);
    test("%p", nullptr);
    test("%p", (void *)0x123);
    test("%p", main);
    test("%s", (char *)nullptr); // nonstandard
    test("id:%d g:%p t:%p name:%s", 42, "a", "b", "asdf");

    {
        uint64_t number = 12345;
        my_va_list map = my_va_list::custom(&number, 1);
        test_onesided("12345", "`i%s%p", "%d", &map);
    }

    log("hi %d\n", 124);

    return fail;
}
#endif

#include "ssscanf.h"
#include "logging.h"
#include "decls.h"

static inline bool
s_parse_hex_char(char c, uint8_t *value) {
    int val;
    if ('0' <= c && c <= '9')
        val = c - '0';
    else if ('a' <= c && c <= 'f')
        val = 10 + (c - 'a');
    else if ('A' <= c && c <= 'F')
        val = 10 + (c - 'A');
    else
        return false;
    *value = (uint8_t)val;
    return true;
}

static bool
s_parse_hex_str(const char **sp, unsigned long long *outp) {
    const char *s = *sp;
    unsigned long long out = 0;
    uint8_t char_value;
    bool neg = false;
    bool ok = false;
    if (*s == '-') {
        neg = true;
        s++;
    }
    while (s_parse_hex_char(*s, &char_value)) {
        ok = true;
        out = (out << 4) | char_value;
        s++;
    }
    if (neg)
        out = -out;
    *sp = s;
    *outp = out;
    return ok;
}

__attribute__((format(scanf, 2, 3))) int
ssscanf(const char *s, const char *format, ...) {
#define _SET_ARG(type, val)                                                              \
    do {                                                                                 \
        typeof(type) _val = (val), *ptr;                                                 \
        if (!noconv) {                                                                   \
            ptr = va_arg(ap, typeof(type) *);                                            \
            *ptr = _val;                                                                 \
        }                                                                                \
    } while (0)

    va_list ap;
    va_start(ap, format);
    int ret = 0;
    char c;
    const char *s_orig = s;
    while ((c = *format++)) {
        if (c != '%') {
            if (*s++ != c)
                break;
            continue;
        }
        ensure((c = *format++));
        if (c == '%') {
            if (*s++ != '%')
                break;
            continue;
        }
        bool noconv = false;
        if (c == '*') {
            ensure((c = *format++));
            noconv = true;
        }
        if (c == 'n') {
            _SET_ARG(int, (int)(s - s_orig));
            // no ret++
            continue;
        }
        if (c == 'c') {
            _SET_ARG(char, *s++);
            ret += !noconv;
            continue;
        }
        int longness = 0;
        int width = 0;
        while ('0' <= c && c <= '9') {
            width = 10 * width + (c - '0');
            ensure((c = *format++));
        }
        if (c == 'l') {
            ensure((c = *format++));
            longness++;
        }
        if (c == 'l') {
            ensure((c = *format++));
            longness++;
        }
        if (c == '[') {
            ensure(longness == 0);
            ensure(width > 0);
            ensure(*format++ == '^');
            char excluded = *format++;
            ensure(*format++ == ']');
            char *outp = noconv ? nullptr : va_arg(ap, char *);
            const char *start = s;
            while (*s != excluded && *s != '\0' && width-- > 0) {
                if (outp)
                    *outp++ = *s;
                s++;
            }
            if (outp)
                *outp++ = '\0';
            if (s == start)
                break;
            ret += !noconv;
            continue;
        }
        if (c == 'x') {
            ensure(width == 0);
            unsigned long long val;
            if (!s_parse_hex_str(&s, &val))
                break;
            if (longness == 2) {
                _SET_ARG(unsigned long long, val);
            } else {
                ensure(longness == 0);
                _SET_ARG(unsigned int, (unsigned int)val);
            }
            ret += !noconv;
            continue;
        }
        panic("fmt? %c", c);
    }
    va_end(ap);
    return ret;
#undef _SET_ARG
}

static char
s_get_hex_digit(uint8_t n) {
    return n < 10 ? (char)('0' + n) : (char)('a' + n - 10);
}

void
s_hex_encode(char *out, const char *in, size_t in_len) {
    uint8_t c;
    while (in_len--) {
        c = (uint8_t)*in++;
        *out++ = s_get_hex_digit((uint8_t)(c >> 4));
        *out++ = s_get_hex_digit(c & 0xf);
    }
}

bool
s_parse_hex(const char *in, void *out, size_t out_len) {
    uint8_t *outp = (uint8_t *)out;
    while (out_len) {
        uint8_t v1, v2;
        if (!s_parse_hex_char(*in++, &v1) || !s_parse_hex_char(*in++, &v2))
            return false;
        *outp++ = (uint8_t)((v1 << 4) | v2);
        out_len --;
    }
    return true;
}

#if SSSCANFTEST
int
main() {
    const char *input = "qXfer:features:read:target.xml:0,fff";
    char area[32], annex[32];
    unsigned xferoff = 0xdead, xferlen = 0xdead;
    int size;
    ensure(4
           == ssscanf(input, "qXfer:%31[^:]:read:%31[^:]:%x,%x%n", area, annex, &xferoff,
                      &xferlen, &size));
    ensure(size == strlen(input));
    ensure(!strcmp(area, "features"));
    ensure(!strcmp(annex, "target.xml"));
    ensure(xferoff == 0);
    ensure(xferlen == 0xfff);
    unsigned long long laddr;
    unsigned int addr, len;
    unsigned int bytes_off;
    ensure(2 == ssscanf("M1234,56:", "%*c%llx,%x:%n", &laddr, &len, &bytes_off));
    ensure(laddr == 0x1234);
    ensure(len == 0x56);

    unsigned int thread_id;
    ensure(1 == ssscanf("-1", "%x%n", &thread_id, &size));
    ensure(size == 2);
}
#endif

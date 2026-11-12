#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/util/snprint.h>

#define FLAG_LEFT  0x01
#define FLAG_ZERO  0x02

enum length_mod { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z };

static inline void append_char(char *str, size_t size, size_t *written, char c) {
    if (*written + 1 < size) {
        str[*written] = c;
    }
    (*written)++;
}

static void append_pad(char *str, size_t size, size_t *written, char c, int count) {
    while (count-- > 0) {
        append_char(str, size, written, c);
    }
}

static void format_number(char *str, size_t size, size_t *written,
                          uint64_t num, int negative, const char *prefix,
                          unsigned base, int uppercase,
                          int flags, int width, int precision) {
    char buf[24];  // 2^64 needs 20 decimal / 16 hex digits
    int len = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    // Precision 0 with value 0 prints no digits (C standard behaviour)
    if (!(num == 0 && precision == 0)) {
        do {
            buf[len++] = digits[num % base];
            num /= base;
        } while (num > 0);
    }

    int zeros = (precision > len) ? precision - len : 0;
    int prefix_len = negative ? 1 : 0;
    if (prefix) {
        for (const char *p = prefix; *p; p++) prefix_len++;
    }

    int total = prefix_len + zeros + len;
    int pad = (width > total) ? width - total : 0;

    // '0' flag is ignored when left-justifying or when a precision is given
    int zero_pad = (flags & FLAG_ZERO) && !(flags & FLAG_LEFT) && precision < 0;

    if (!(flags & FLAG_LEFT) && !zero_pad) append_pad(str, size, written, ' ', pad);
    if (negative) append_char(str, size, written, '-');
    if (prefix) for (const char *p = prefix; *p; p++) append_char(str, size, written, *p);
    if (zero_pad) append_pad(str, size, written, '0', pad);
    append_pad(str, size, written, '0', zeros);
    while (len > 0) append_char(str, size, written, buf[--len]);
    if (flags & FLAG_LEFT) append_pad(str, size, written, ' ', pad);
}

static int64_t fetch_signed(va_list *args, enum length_mod len) {
    switch (len) {
        case LEN_HH: return (signed char)va_arg(*args, int);
        case LEN_H:  return (short)va_arg(*args, int);
        case LEN_L:  return va_arg(*args, long);
        case LEN_LL: return va_arg(*args, long long);
        case LEN_Z:  return (int64_t)va_arg(*args, size_t);
        default:     return va_arg(*args, int);
    }
}

static uint64_t fetch_unsigned(va_list *args, enum length_mod len) {
    switch (len) {
        case LEN_HH: return (unsigned char)va_arg(*args, unsigned int);
        case LEN_H:  return (unsigned short)va_arg(*args, unsigned int);
        case LEN_L:  return va_arg(*args, unsigned long);
        case LEN_LL: return va_arg(*args, unsigned long long);
        case LEN_Z:  return va_arg(*args, size_t);
        default:     return va_arg(*args, unsigned int);
    }
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    size_t written = 0;
    va_list args;
    va_copy(args, ap);  // so we can safely take its address on every ABI

    for (const char *p = format; *p != '\0'; p++) {
        if (*p != '%') {
            append_char(str, size, &written, *p);
            continue;
        }

        const char *spec_start = p;
        p++; // Skip '%'

        // Flags
        int flags = 0;
        for (;; p++) {
            if (*p == '-') flags |= FLAG_LEFT;
            else if (*p == '0') flags |= FLAG_ZERO;
            else break;
        }

        // Width
        int width = 0;
        if (*p == '*') {
            width = va_arg(args, int);
            if (width < 0) {
                flags |= FLAG_LEFT;
                width = -width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p++ - '0');
            }
        }

        // Precision
        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            if (*p == '*') {
                precision = va_arg(args, int);
                if (precision < 0) precision = -1;
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    precision = precision * 10 + (*p++ - '0');
                }
            }
        }

        // Length modifier
        enum length_mod len = LEN_NONE;
        if (*p == 'h') {
            p++;
            if (*p == 'h') { len = LEN_HH; p++; } else len = LEN_H;
        } else if (*p == 'l') {
            p++;
            if (*p == 'l') { len = LEN_LL; p++; } else len = LEN_L;
        } else if (*p == 'z') {
            len = LEN_Z;
            p++;
        }

        switch (*p) {
            case 'c': {
                char c = (char)va_arg(args, int);
                int pad = width > 1 ? width - 1 : 0;
                if (!(flags & FLAG_LEFT)) append_pad(str, size, &written, ' ', pad);
                append_char(str, size, &written, c);
                if (flags & FLAG_LEFT) append_pad(str, size, &written, ' ', pad);
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                int slen = 0;
                while (s[slen] && (precision < 0 || slen < precision)) slen++;
                int pad = width > slen ? width - slen : 0;
                if (!(flags & FLAG_LEFT)) append_pad(str, size, &written, ' ', pad);
                for (int i = 0; i < slen; i++) append_char(str, size, &written, s[i]);
                if (flags & FLAG_LEFT) append_pad(str, size, &written, ' ', pad);
                break;
            }
            case 'd':
            case 'i': {
                int64_t val = fetch_signed(&args, len);
                // Negate in unsigned arithmetic so INT64_MIN is handled
                uint64_t mag = (val < 0) ? 0u - (uint64_t)val : (uint64_t)val;
                format_number(str, size, &written, mag, val < 0, NULL,
                              10, 0, flags, width, precision);
                break;
            }
            case 'u':
            case 'x':
            case 'X': {
                uint64_t val = fetch_unsigned(&args, len);
                unsigned base = (*p == 'u') ? 10 : 16;
                format_number(str, size, &written, val, 0, NULL,
                              base, *p == 'X', flags, width, precision);
                break;
            }
            case 'p': {
                uintptr_t ptr = (uintptr_t)va_arg(args, void *);
                format_number(str, size, &written, ptr, 0, "0x",
                              16, 0, flags, width, precision);
                break;
            }
            case '%': {
                append_char(str, size, &written, '%');
                break;
            }
            default: {
                // Unknown/incomplete specifier: output it literally
                for (const char *q = spec_start; q < p; q++) {
                    append_char(str, size, &written, *q);
                }
                if (*p != '\0') {
                    append_char(str, size, &written, *p);
                } else {
                    p--; // Trailing '%' at end of string: let the loop terminate
                }
                break;
            }
        }
    }

    va_end(args);

    // Always null-terminate if size > 0
    if (size > 0) {
        str[(written < size) ? written : size - 1] = '\0';
    }

    return (int)written;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(str, size, format, args);
    va_end(args);
    return result;
}

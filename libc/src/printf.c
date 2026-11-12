/*
 * printf engine and the string-targeted printf functions.
 *
 * __vformat() walks the format and hands each piece of output to an emit
 * callback, so the same code serves snprintf (memory), fprintf (a FILE) and
 * dprintf (a descriptor).
 *
 * Floating point is converted exactly (every digit of the binary value,
 * ties rounded to even), so output matches glibc digit for digit.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"

#define FL_LEFT     0x01    // '-'
#define FL_PLUS     0x02    // '+'
#define FL_SPACE    0x04    // ' '
#define FL_ALT      0x08    // '#'
#define FL_ZERO     0x10    // '0'
#define FL_UPPER    0x20

enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_J, LEN_Z, LEN_T, LEN_BIGL };


typedef struct {
    __emit_fn emit;
    void*     ctx;
    size_t    total;
    bool      failed;   // Out of memory for a conversion
} out_t;

static void out(out_t* o, const char* s, size_t n) {
    if (n == 0) return;
    o->emit(o->ctx, s, n);
    o->total += n;
}

static void pad(out_t* o, char c, int n) {
    static const char spaces[16] = "                ";
    static const char zeros[16]  = "0000000000000000";
    const char* src = (c == '0') ? zeros : spaces;
    while (n > 0) {
        int chunk = n < 16 ? n : 16;
        out(o, src, (size_t)chunk);
        n -= chunk;
    }
}

// Emit [prefix][zeros][body] padded to `width` as the flags say
static void emit_field(out_t* o, int flags, int width,
                       const char* prefix, size_t prefix_len,
                       int zeros, const char* body, size_t body_len) {
    int len = (int)(prefix_len + body_len) + zeros;
    int fill = (width > len) ? width - len : 0;

    if (!(flags & FL_LEFT) && !(flags & FL_ZERO)) pad(o, ' ', fill);
    out(o, prefix, prefix_len);
    if (!(flags & FL_LEFT) && (flags & FL_ZERO))  pad(o, '0', fill);
    pad(o, '0', zeros);
    out(o, body, body_len);
    if (flags & FL_LEFT) pad(o, ' ', fill);
}

//
// Integers
//

static void format_integer(out_t* o, uintmax_t value, bool negative, unsigned base,
                           int flags, int width, int precision) {
    char buf[24];       // 2^64 in octal is 22 digits
    char* end = buf + sizeof(buf);
    char* p = end;
    const char* digits = (flags & FL_UPPER) ? "0123456789ABCDEF" : "0123456789abcdef";

    if (value <= UINT32_MAX) {
        // 32-bit division is far cheaper on i386
        uint32_t v = (uint32_t)value;
        while (v) { *--p = digits[v % base]; v /= base; }
    } else {
        while (value) { *--p = digits[value % base]; value /= base; }
        value = 1;  // Remember it was nonzero
    }
    bool zero = (p == end);

    // Precision 0 with value 0 prints no digits; otherwise "0"
    if (zero && precision != 0)
        *--p = '0';

    char prefix[3];
    size_t prefix_len = 0;
    if (negative)                 prefix[prefix_len++] = '-';
    else if (flags & FL_PLUS)     prefix[prefix_len++] = '+';
    else if (flags & FL_SPACE)    prefix[prefix_len++] = ' ';

    if ((flags & FL_ALT) && base == 16 && !zero) {
        prefix[prefix_len++] = '0';
        prefix[prefix_len++] = (flags & FL_UPPER) ? 'X' : 'x';
    }

    int len = (int)(end - p);
    int zeros = (precision > len) ? precision - len : 0;

    // '#' with octal: the first digit must be a 0
    if ((flags & FL_ALT) && base == 8 && zeros == 0 && (len == 0 || *p != '0'))
        zeros = 1;

    if (precision >= 0)
        flags &= ~FL_ZERO;      // An explicit precision overrides '0'

    emit_field(o, flags, width, prefix, prefix_len, zeros, p, (size_t)len);
}

//
// Floating point
//
// Exact conversion: the value is M * 2^K (M the 64-bit x87 significand).
// For K >= 0 that integer is built in base 1e9; for K < 0 the value is
// M * 5^-K / 10^-K, so M * 5^-K is built instead and the decimal point
// placed afterwards. Either way every decimal digit of the binary value is
// available, and rounding (half to even, like glibc) is exact. A double
// needs at most ~90 limbs (on the stack); only extreme long doubles reach
// for malloc.
//

#define DEC_STACK_LIMBS     128
#define FLOAT_MAX_PREC      4096    // Larger precisions are clamped
#define FLOAT_STACK_BUF     1024

static const uint32_t pow10_u32[10] = {
    1U, 10U, 100U, 1000U, 10000U, 100000U, 1000000U, 10000000U, 100000000U, 1000000000U
};

typedef struct {
    uint32_t* limb;     // Base 1e9, least significant first
    int       n;
    int       top_digits;
    int       digits;   // Decimal digits in the big integer
    int       e;        // Decimal exponent of the first digit of the value
    bool      heap;
} dec_t;

// t / 1e9 and t % 1e9 in one divl. Requires t < 1e9 * 2^32.
static inline uint32_t divmod_1e9(uint64_t t, uint32_t* rem) {
    uint32_t q, r;
    __asm__("divl %4" : "=a"(q), "=d"(r) : "a"((uint32_t)t), "d"((uint32_t)(t >> 32)), "r"(1000000000U));
    *rem = r;
    return q;
}

// Multiply by f < 2^31
static void dec_mul(dec_t* d, uint32_t f) {
    uint32_t carry = 0;
    for (int i = 0; i < d->n; i++) {
        uint32_t rem;
        carry = divmod_1e9((uint64_t)d->limb[i] * f + carry, &rem);
        d->limb[i] = rem;
    }
    while (carry) {
        d->limb[d->n++] = carry % 1000000000U;
        carry /= 1000000000U;
    }
}

static bool dec_init(dec_t* d, long double v, uint32_t* stack) {
    union { long double ld; struct { uint64_t m; uint16_t se; } p; } u;
    u.ld = v;
    uint64_t m = u.p.m;
    int exp = u.p.se & 0x7FFF;
    int k = (exp == 0 ? 1 : exp) - 16383 - 63;     // Denormals: exponent of 1

    while (!(m & 1)) { m >>= 1; k++; }              // Smaller numbers, same value

    // Limbs needed: 20 digits for m, plus log10(2) or log10(5) per power
    int need = (k >= 0) ? (k * 302 / 1000 + 21) / 9 + 3 : (-k * 699 / 1000 + 21) / 9 + 3;
    d->heap = need > DEC_STACK_LIMBS;
    d->limb = d->heap ? (uint32_t*)malloc((size_t)need * sizeof(uint32_t)) : stack;
    if (d->limb == NULL)
        return false;

    d->n = 0;
    do { d->limb[d->n++] = (uint32_t)(m % 1000000000U); m /= 1000000000U; } while (m);

    int shift = 0;
    if (k >= 0) {
        for (; k >= 29; k -= 29) dec_mul(d, 1U << 29);
        if (k > 0) dec_mul(d, 1U << k);
    } else {
        shift = k;
        int j = -k;
        for (; j >= 13; j -= 13) dec_mul(d, 1220703125U);     // 5^13
        if (j > 0) {
            uint32_t f = 1;
            while (j--) f *= 5;
            dec_mul(d, f);
        }
    }

    uint32_t top = d->limb[d->n - 1];
    d->top_digits = 1;
    while (d->top_digits < 10 && top >= pow10_u32[d->top_digits]) d->top_digits++;
    d->digits = d->top_digits + 9 * (d->n - 1);
    d->e = d->digits - 1 + shift;
    return true;
}

static void dec_free(dec_t* d) {
    if (d->heap) free(d->limb);
}

// i-th decimal digit of the big integer, most significant first
static int dec_digit(const dec_t* d, int i) {
    if (i < 0 || i >= d->digits)
        return 0;
    if (i < d->top_digits)
        return (int)(d->limb[d->n - 1] / pow10_u32[d->top_digits - 1 - i] % 10);
    int j = i - d->top_digits;
    return (int)(d->limb[d->n - 2 - j / 9] / pow10_u32[8 - j % 9] % 10);
}

// Round to nsig significant digits (as 0..9 into out, which holds nsig + 1).
// *e10 gets the exponent of out[0]. Returns how many digits were stored:
// nsig, or for nsig == 0 one digit '1' when the value rounds up to the next
// power of ten, else 0 (the value rounds to zero at that place).
static int dec_round(const dec_t* d, int nsig, char* out, int* e10) {
    *e10 = d->e;
    if (nsig < 0)
        return 0;

    for (int i = 0; i < nsig; i++)
        out[i] = (char)dec_digit(d, i);

    int r = dec_digit(d, nsig);
    bool up = r > 5;
    if (r == 5) {
        bool rest = false;
        for (int i = nsig + 1; i < d->digits && !rest; i++)
            rest = dec_digit(d, i) != 0;
        bool odd = (nsig > 0) && (out[nsig - 1] & 1);
        up = rest || odd;   // Exact ties go to even
    }

    if (nsig == 0) {
        if (up) {
            out[0] = 1;
            *e10 = d->e + 1;
            return 1;
        }
        return 0;
    }

    if (up) {
        int i = nsig - 1;
        while (i >= 0 && ++out[i] > 9) out[i--] = 0;
        if (i < 0) {        // 9.99 -> 10.0
            out[0] = 1;
            *e10 = d->e + 1;
        }
    }
    return nsig;
}

// Digit for place 10^k from a dec_round() result
static inline char digit_at(const char* d, int count, int e10, int k) {
    int i = e10 - k;
    return (char)('0' + ((i >= 0 && i < count) ? d[i] : 0));
}

// Exponent suffix: e+05, e-123
static size_t format_exponent(char* buf, int e, bool upper) {
    size_t n = 0;
    buf[n++] = upper ? 'E' : 'e';
    buf[n++] = (e < 0) ? '-' : '+';
    unsigned u = (e < 0) ? (unsigned)-e : (unsigned)e;
    char tmp[6];
    int t = 0;
    do { tmp[t++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (t < 2) tmp[t++] = '0';
    while (t > 0) buf[n++] = tmp[--t];
    return n;
}

// %f body. Returns its length.
static size_t render_fixed(char* buf, const char* d, int count, int e10, int prec, bool alt) {
    size_t n = 0;
    if (e10 < 0) {
        buf[n++] = '0';
    } else {
        for (int k = e10; k >= 0; k--) buf[n++] = digit_at(d, count, e10, k);
    }
    if (prec > 0 || alt) buf[n++] = '.';
    for (int k = -1; k >= -prec; k--) buf[n++] = digit_at(d, count, e10, k);
    return n;
}

// %e body (d holds prec + 1 significant digits)
static size_t render_sci(char* buf, const char* d, int count, int e10, int prec, bool alt, bool upper) {
    size_t n = 0;
    buf[n++] = (char)('0' + (count > 0 ? d[0] : 0));
    if (prec > 0 || alt) buf[n++] = '.';
    for (int i = 1; i <= prec; i++) buf[n++] = (char)('0' + (i < count ? d[i] : 0));
    n += format_exponent(buf + n, e10, upper);
    return n;
}

// %g without '#': drop trailing zeros after the point, and a bare point
static size_t strip_zeros(char* buf, size_t n) {
    size_t mantissa_end = n;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == 'e' || buf[i] == 'E') { mantissa_end = i; break; }
    }
    if (memchr(buf, '.', mantissa_end) == NULL)
        return n;

    size_t cut = mantissa_end;
    while (cut > 0 && buf[cut - 1] == '0') cut--;
    if (cut > 0 && buf[cut - 1] == '.') cut--;

    memmove(buf + cut, buf + mantissa_end, n - mantissa_end);
    return cut + (n - mantissa_end);
}

// %a / %A. Doubles print as glibc does (0x1.8p+1); long doubles use the
// x87 layout with its explicit integer bit, like glibc on x86 (0xcp-2).
static void format_hexfloat(out_t* o, long double v, bool is_long, int flags, int width,
                            int precision, bool upper, const char* sign, size_t sign_len) {
    const char* hexd = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint32_t lead;
    uint64_t frac;      // Fraction bits, left aligned
    int ndig, exp;

    if (!is_long) {
        union { double d; uint64_t u; } u = { (double)v };
        int e = (int)((u.u >> 52) & 0x7FF);
        frac = (u.u & ((1ULL << 52) - 1)) << 12;
        ndig = 13;
        lead = (e != 0);
        exp = (e != 0) ? e - 1023 : (frac ? -1022 : 0);
    } else {
        union { long double ld; struct { uint64_t m; uint16_t se; } p; } u;
        u.ld = v;
        int e = u.p.se & 0x7FFF;
        lead = (uint32_t)(u.p.m >> 60);
        frac = u.p.m << 4;
        ndig = 15;
        exp = (u.p.m == 0) ? 0 : (e == 0 ? 1 : e) - 16383 - 3;
    }

    uint8_t dig[16];
    for (int i = 0; i < ndig; i++)
        dig[i] = (uint8_t)((frac >> (60 - 4 * i)) & 0xF);

    int p = precision;
    if (p < 0) {
        p = ndig;
        while (p > 0 && dig[p - 1] == 0) p--;   // Shortest exact form
    } else if (p < ndig) {
        uint64_t rest = frac << (4 * p);        // Bits after the last kept digit
        uint64_t half = 1ULL << 63;
        bool odd = (p > 0) ? (dig[p - 1] & 1) : (lead & 1);
        if (rest > half || (rest == half && odd)) {
            int i = p - 1;
            while (i >= 0 && ++dig[i] > 15) dig[i--] = 0;
            if (i < 0) lead++;
        }
    }

    char body[48];
    size_t n = 0;
    if (lead >= 16) body[n++] = hexd[lead >> 4];
    body[n++] = hexd[lead & 0xF];
    if (p > 0 || (flags & FL_ALT)) body[n++] = '.';
    for (int i = 0; i < p && n < 24; i++) body[n++] = (i < ndig) ? hexd[dig[i]] : '0';
    body[n++] = upper ? 'P' : 'p';
    body[n++] = (exp < 0) ? '-' : '+';
    char tmp[8];
    int t = 0;
    unsigned ue = (exp < 0) ? (unsigned)-exp : (unsigned)exp;
    do { tmp[t++] = (char)('0' + ue % 10); ue /= 10; } while (ue);
    while (t > 0) body[n++] = tmp[--t];

    // Precision beyond the stored digits: the zeros go out separately
    int extra = (p > 24) ? p - 24 : 0;

    char prefix[4];
    size_t prefix_len = 0;
    for (size_t i = 0; i < sign_len; i++) prefix[prefix_len++] = sign[i];
    prefix[prefix_len++] = '0';
    prefix[prefix_len++] = upper ? 'X' : 'x';

    if (extra == 0) {
        emit_field(o, flags, width, prefix, prefix_len, 0, body, n);
        return;
    }
    // Rare: a long run of zeros goes out between the digits and the 'p'
    size_t ppos = 0;
    while (body[ppos] != 'p' && body[ppos] != 'P') ppos++;
    int len = (int)(prefix_len + n) + extra;
    int fill = (width > len) ? width - len : 0;
    if (!(flags & FL_LEFT) && !(flags & FL_ZERO)) pad(o, ' ', fill);
    out(o, prefix, prefix_len);
    if (!(flags & FL_LEFT) && (flags & FL_ZERO)) pad(o, '0', fill);
    out(o, body, ppos);
    pad(o, '0', extra);
    out(o, body + ppos, n - ppos);
    if (flags & FL_LEFT) pad(o, ' ', fill);
}

// False only when out of memory for an extreme value or precision
static bool format_float(out_t* o, long double v, char conv, int flags, int width, int precision, bool is_long) {
    bool upper = (conv == 'F' || conv == 'E' || conv == 'G' || conv == 'A');
    bool negative = __builtin_signbit(v) != 0;
    if (negative) v = -v;

    char prefix[1];
    size_t prefix_len = 0;
    if (negative)               prefix[prefix_len++] = '-';
    else if (flags & FL_PLUS)   prefix[prefix_len++] = '+';
    else if (flags & FL_SPACE)  prefix[prefix_len++] = ' ';

    if (__builtin_isnan(v) || __builtin_isinf(v)) {
        const char* s = __builtin_isnan(v) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        emit_field(o, flags & ~FL_ZERO, width, prefix, prefix_len, 0, s, 3);
        return true;
    }

    if (conv == 'a' || conv == 'A') {
        if (precision > FLOAT_MAX_PREC) precision = FLOAT_MAX_PREC;
        format_hexfloat(o, v, is_long, flags, width, precision, upper, prefix, prefix_len);
        return true;
    }

    if (precision < 0) precision = 6;
    if (precision > FLOAT_MAX_PREC) precision = FLOAT_MAX_PREC;
    bool alt = (flags & FL_ALT) != 0;

    uint32_t stack_limbs[DEC_STACK_LIMBS];
    dec_t dec = { NULL, 0, 0, 0, 0, false };
    if (v != 0.0L && !dec_init(&dec, v, stack_limbs))
        return false;

    // Digits and rendered text share one buffer: digits first, text after
    int e_hi = (v != 0.0L) ? dec.e + 1 : 0;         // Largest exponent after rounding
    int int_digits = (e_hi > 0) ? e_hi + 1 : 1;
    size_t digits_cap = (size_t)(int_digits + precision + 2);
    size_t text_cap = (size_t)(int_digits + precision + 16);

    char stack_buf[FLOAT_STACK_BUF];
    char* buf = stack_buf;
    if (digits_cap + text_cap > sizeof(stack_buf)) {
        buf = (char*)malloc(digits_cap + text_cap);
        if (buf == NULL) {
            dec_free(&dec);
            return false;
        }
    }
    char* d = buf;
    char* text = buf + digits_cap;

    int count = 0, e10 = 0;
    size_t n;

    switch (conv) {
        case 'f': case 'F':
            if (v != 0.0L) count = dec_round(&dec, dec.e + 1 + precision, d, &e10);
            n = render_fixed(text, d, count, e10, precision, alt);
            break;

        case 'e': case 'E':
            if (v != 0.0L) count = dec_round(&dec, precision + 1, d, &e10);
            n = render_sci(text, d, count, e10, precision, alt, upper);
            break;

        default: {  // g, G (C11 7.21.6.1: style by exponent X after rounding to P digits)
            int p = (precision == 0) ? 1 : precision;
            if (v != 0.0L) count = dec_round(&dec, p, d, &e10);
            if (p > e10 && e10 >= -4) {
                int fprec = p - 1 - e10;
                if (v != 0.0L) count = dec_round(&dec, dec.e + 1 + fprec, d, &e10);
                n = render_fixed(text, d, count, e10, fprec, alt);
            } else {
                n = render_sci(text, d, count, e10, p - 1, alt, upper);
            }
            if (!alt) n = strip_zeros(text, n);
            break;
        }
    }

    emit_field(o, flags, width, prefix, prefix_len, 0, text, n);

    if (buf != stack_buf) free(buf);
    dec_free(&dec);
    return true;
}

//
// The format walker
//

int __vformat(__emit_fn emit, void* ctx, const char* fmt, va_list ap_in) {
    out_t o = { emit, ctx, 0, false };
    va_list ap;
    va_copy(ap, ap_in);

    while (*fmt) {
        // Literal run up to the next '%'
        const char* run = fmt;
        while (*fmt && *fmt != '%') fmt++;
        out(&o, run, (size_t)(fmt - run));
        if (*fmt == '\0')
            break;

        const char* spec = fmt++;   // At the '%'

        int flags = 0;
        for (;; fmt++) {
            if      (*fmt == '-') flags |= FL_LEFT;
            else if (*fmt == '+') flags |= FL_PLUS;
            else if (*fmt == ' ') flags |= FL_SPACE;
            else if (*fmt == '#') flags |= FL_ALT;
            else if (*fmt == '0') flags |= FL_ZERO;
            else break;
        }

        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flags |= FL_LEFT; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                if (width < INT_MAX / 10) width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                if (precision < 0) precision = -1;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    if (precision < INT_MAX / 10) precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        int len = LEN_NONE;
        switch (*fmt) {
            case 'h': fmt++; if (*fmt == 'h') { fmt++; len = LEN_HH; } else len = LEN_H; break;
            case 'l': fmt++; if (*fmt == 'l') { fmt++; len = LEN_LL; } else len = LEN_L; break;
            case 'j': fmt++; len = LEN_J; break;
            case 'z': fmt++; len = LEN_Z; break;
            case 't': fmt++; len = LEN_T; break;
            case 'L': fmt++; len = LEN_BIGL; break;
        }

        if (flags & FL_LEFT) flags &= ~FL_ZERO;
        if (flags & FL_PLUS) flags &= ~FL_SPACE;

        char conv = *fmt;
        if (conv == '\0') {
            out(&o, spec, (size_t)(fmt - spec));    // Dangling '%...': print as is
            break;
        }
        fmt++;

        switch (conv) {
            case 'd': case 'i': {
                intmax_t v;
                switch (len) {
                    case LEN_HH: v = (signed char)va_arg(ap, int);  break;
                    case LEN_H:  v = (short)va_arg(ap, int);        break;
                    case LEN_L:  v = va_arg(ap, long);              break;
                    case LEN_LL: v = va_arg(ap, long long);         break;
                    case LEN_J:  v = va_arg(ap, intmax_t);          break;
                    case LEN_Z:  v = va_arg(ap, ssize_t);           break;
                    case LEN_T:  v = va_arg(ap, ptrdiff_t);         break;
                    default:     v = va_arg(ap, int);               break;
                }
                uintmax_t mag = (v < 0) ? 0 - (uintmax_t)v : (uintmax_t)v;
                format_integer(&o, mag, v < 0, 10, flags, width, precision);
                break;
            }

            case 'u': case 'o': case 'x': case 'X': {
                uintmax_t v;
                switch (len) {
                    case LEN_HH: v = (unsigned char)va_arg(ap, unsigned int);  break;
                    case LEN_H:  v = (unsigned short)va_arg(ap, unsigned int); break;
                    case LEN_L:  v = va_arg(ap, unsigned long);                break;
                    case LEN_LL: v = va_arg(ap, unsigned long long);           break;
                    case LEN_J:  v = va_arg(ap, uintmax_t);                    break;
                    case LEN_Z:  v = va_arg(ap, size_t);                       break;
                    case LEN_T:  v = (uintmax_t)va_arg(ap, ptrdiff_t);         break;
                    default:     v = va_arg(ap, unsigned int);                 break;
                }
                unsigned base = (conv == 'u') ? 10 : (conv == 'o') ? 8 : 16;
                if (conv == 'X') flags |= FL_UPPER;
                format_integer(&o, v, false, base, flags & ~(FL_PLUS | FL_SPACE), width, precision);
                break;
            }

            case 'p': {
                void* ptr = va_arg(ap, void*);
                if (ptr == NULL) {
                    emit_field(&o, flags & ~FL_ZERO, width, NULL, 0, 0, "(nil)", 5);
                } else {
                    format_integer(&o, (uintptr_t)ptr, false, 16, (flags | FL_ALT) & ~(FL_PLUS | FL_SPACE),
                                   width, precision);
                }
                break;
            }

            case 'c': {
                char c = (char)va_arg(ap, int);
                emit_field(&o, flags & ~FL_ZERO, width, NULL, 0, 0, &c, 1);
                break;
            }

            case 's': {
                const char* s = va_arg(ap, const char*);
                if (s == NULL) s = "(null)";
                size_t n = (precision >= 0) ? strnlen(s, (size_t)precision) : strlen(s);
                emit_field(&o, flags & ~FL_ZERO, width, NULL, 0, 0, s, n);
                break;
            }

            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                long double v = (len == LEN_BIGL) ? va_arg(ap, long double) : (long double)va_arg(ap, double);
                if (!format_float(&o, v, conv, flags, width, precision, len == LEN_BIGL))
                    o.failed = true;
                break;
            }

            case 'n': {
                size_t t = o.total;
                switch (len) {
                    case LEN_HH: *va_arg(ap, signed char*) = (signed char)t; break;
                    case LEN_H:  *va_arg(ap, short*) = (short)t;             break;
                    case LEN_L:  *va_arg(ap, long*) = (long)t;               break;
                    case LEN_LL: *va_arg(ap, long long*) = (long long)t;     break;
                    case LEN_J:  *va_arg(ap, intmax_t*) = (intmax_t)t;       break;
                    case LEN_Z:  *va_arg(ap, size_t*) = t;                   break;
                    case LEN_T:  *va_arg(ap, ptrdiff_t*) = (ptrdiff_t)t;     break;
                    default:     *va_arg(ap, int*) = (int)t;                 break;
                }
                break;
            }

            case '%':
                out(&o, "%", 1);
                break;

            default:
                // Unknown conversion: print the spec unchanged
                out(&o, spec, (size_t)(fmt - spec));
                break;
        }
    }

    va_end(ap);

    if (o.failed) {
        errno = ENOMEM;
        return -1;
    }
    if (o.total > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (int)o.total;
}

//
// Memory targets
//

typedef struct {
    char*  buf;
    size_t size;    // Capacity, terminator included
    size_t len;     // Bytes stored (excluding the terminator)
} strbuf_t;

static void emit_strbuf(void* ctx, const char* s, size_t n) {
    strbuf_t* b = (strbuf_t*)ctx;
    if (b->size == 0)
        return;
    size_t room = b->size - 1 - b->len;
    if (n > room) n = room;
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

int vsnprintf(char* restrict buf, size_t size, const char* restrict fmt, va_list ap) {
    strbuf_t b = { buf, size, 0 };
    int r = __vformat(emit_strbuf, &b, fmt, ap);
    if (size > 0)
        buf[b.len] = '\0';
    return r;
}

int snprintf(char* restrict buf, size_t size, const char* restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char* restrict buf, const char* restrict fmt, va_list ap) {
    return vsnprintf(buf, (size_t)INT_MAX + 1, fmt, ap);
}

int sprintf(char* restrict buf, const char* restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

// Growable buffer for asprintf
typedef struct {
    char*  buf;
    size_t cap;
    size_t len;
    bool   failed;
} dynbuf_t;

static void emit_dynbuf(void* ctx, const char* s, size_t n) {
    dynbuf_t* b = (dynbuf_t*)ctx;
    if (b->failed)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + n + 1) cap *= 2;
        char* p = (char*)realloc(b->buf, cap);
        if (p == NULL) {
            b->failed = true;
            return;
        }
        b->buf = p;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

int vasprintf(char** restrict out_str, const char* restrict fmt, va_list ap) {
    dynbuf_t b = { NULL, 0, 0, false };
    int r = __vformat(emit_dynbuf, &b, fmt, ap);
    if (r < 0 || b.failed || (b.buf == NULL && (b.buf = (char*)malloc(1)) == NULL)) {
        free(b.buf);
        *out_str = NULL;
        if (b.failed) errno = ENOMEM;
        return -1;
    }
    b.buf[b.len] = '\0';
    *out_str = b.buf;
    return r;
}

int asprintf(char** restrict out_str, const char* restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(out_str, fmt, ap);
    va_end(ap);
    return r;
}


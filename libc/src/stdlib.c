#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//
// Integer parsing
//

// Shared core. Parses an optionally signed integer in `base` (0 = auto) and
// clamps the magnitude to `limit` (limit_neg for negative numbers), setting
// ERANGE on overflow. *neg tells the caller which way to apply the sign.
static unsigned long long parse_integer(const char* s, char** end, int base,
                                        unsigned long long limit,
                                        unsigned long long limit_neg, bool* neg) {
    const char* p = s;
    *neg = false;

    if (base < 0 || base == 1 || base > 36) {
        errno = EINVAL;
        if (end) *end = (char*)s;
        return 0;
    }

    while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') {
        *neg = (*p == '-');
        p++;
    }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }

    unsigned long long max = *neg ? limit_neg : limit;
    unsigned long long value = 0;
    bool any = false, overflow = false;

    for (;; p++) {
        int c = (unsigned char)*p, digit;
        if (isdigit(c))       digit = c - '0';
        else if (isalpha(c))  digit = (c | 32) - 'a' + 10;
        else                  break;
        if (digit >= base)    break;

        any = true;
        if (value > (max - (unsigned)digit) / (unsigned)base)
            overflow = true;
        else
            value = value * (unsigned)base + (unsigned)digit;
    }

    if (end) *end = (char*)(any ? p : s);
    if (overflow) {
        errno = ERANGE;
        return max;
    }
    return value;
}

long long strtoll(const char* restrict s, char** restrict end, int base) {
    bool neg;
    unsigned long long v = parse_integer(s, end, base, LLONG_MAX, (unsigned long long)LLONG_MAX + 1, &neg);
    return neg ? (long long)(0 - v) : (long long)v;
}

unsigned long long strtoull(const char* restrict s, char** restrict end, int base) {
    bool neg;
    unsigned long long v = parse_integer(s, end, base, ULLONG_MAX, ULLONG_MAX, &neg);
    return neg ? 0 - v : v;     // C: negation in the unsigned type
}

long strtol(const char* restrict s, char** restrict end, int base) {
    bool neg;
    unsigned long long v = parse_integer(s, end, base, LONG_MAX, (unsigned long long)LONG_MAX + 1, &neg);
    return neg ? (long)(0 - (unsigned long)v) : (long)v;
}

unsigned long strtoul(const char* restrict s, char** restrict end, int base) {
    bool neg;
    unsigned long long v = parse_integer(s, end, base, ULONG_MAX, ULONG_MAX, &neg);
    return neg ? 0 - (unsigned long)v : (unsigned long)v;
}

int       atoi(const char* s)  { return (int)strtol(s, NULL, 10); }
long      atol(const char* s)  { return strtol(s, NULL, 10); }
long long atoll(const char* s) { return strtoll(s, NULL, 10); }

//
// Floating-point parsing
//
// Decimal, hexadecimal (0x1.8p3), "inf"/"infinity" and "nan". Hex input is
// exact. For decimal input the first 19 significant digits are kept and the
// power of ten is applied in x87 extended precision: the result is correctly
// rounded in ordinary cases, and at worst one ulp off for long inputs that
// fall almost exactly halfway between two doubles.
//

static long double pow10_ld(int e) {
    static const long double p[] = { 1e1L, 1e2L, 1e4L, 1e8L, 1e16L, 1e32L, 1e64L, 1e128L, 1e256L,
                                     1e512L, 1e1024L, 1e2048L, 1e4096L };
    long double r = 1.0L;
    unsigned u = (e < 0) ? (unsigned)-e : (unsigned)e;
    for (int i = 0; u && i < 13; i++, u >>= 1) {
        if (u & 1) r *= p[i];
    }
    if (u) r = __builtin_infl();    // Far out of range either way
    return (e < 0) ? 1.0L / r : r;
}

long double strtold(const char* restrict s, char** restrict end) {
    const char* p = s;
    bool neg = false;

    while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }

    if (strncasecmp(p, "inf", 3) == 0) {
        p += (strncasecmp(p, "infinity", 8) == 0) ? 8 : 3;
        if (end) *end = (char*)p;
        return neg ? -__builtin_infl() : __builtin_infl();
    }
    if (strncasecmp(p, "nan", 3) == 0) {
        p += 3;
        if (*p == '(') {    // nan(n-char-sequence)
            const char* q = p + 1;
            while (isalnum((unsigned char)*q) || *q == '_') q++;
            if (*q == ')') p = q + 1;
        }
        if (end) *end = (char*)p;
        return neg ? -__builtin_nanl("") : __builtin_nanl("");
    }

    // Hexadecimal: 0x1.8p3. Exact: up to 16 hex digits fit the x87
    // significand, and any dropped nonzero digit becomes a sticky low bit,
    // so the final conversion to double rounds correctly.
    if (p[0] == '0' && (p[1] | 32) == 'x' &&
        (isxdigit((unsigned char)p[2]) || (p[2] == '.' && isxdigit((unsigned char)p[3])))) {
        p += 2;
        unsigned long long hmant = 0;
        int hdigits = 0, exp2 = 0;
        bool sticky = false, seen_point = false;
        for (;; p++) {
            int c = (unsigned char)*p, v;
            if (c == '.' && !seen_point) { seen_point = true; continue; }
            if (isdigit(c))       v = c - '0';
            else if (isxdigit(c)) v = (c | 32) - 'a' + 10;
            else                  break;
            if (hdigits < 16) {
                if (hmant || v) { hmant = (hmant << 4) | (unsigned)v; hdigits++; }
                if (seen_point) exp2 -= 4;
            } else {
                if (v) sticky = true;
                if (!seen_point) exp2 += 4;
            }
        }
        if (sticky) hmant |= 1;
        if ((*p | 32) == 'p') {
            const char* q = p + 1;
            bool eneg = false;
            if (*q == '+' || *q == '-') { eneg = (*q == '-'); q++; }
            if (isdigit((unsigned char)*q)) {
                int e = 0;
                for (; isdigit((unsigned char)*q); q++) {
                    if (e < 100000) e = e * 10 + (*q - '0');
                }
                exp2 += eneg ? -e : e;
                p = q;
            }
        }
        if (end) *end = (char*)p;

        // Scale by exact powers of two from a table. (Not 1.0L / (1ULL << n):
        // clang 18 folds that into exponent-bit surgery done with double's
        // layout on the 80-bit format, and the result is garbage.)
        static const long double up[]   = { 0x1p1L,  0x1p2L,  0x1p4L,  0x1p8L,  0x1p16L,  0x1p32L };
        static const long double down[] = { 0x1p-1L, 0x1p-2L, 0x1p-4L, 0x1p-8L, 0x1p-16L, 0x1p-32L };
        long double v = (long double)hmant;
        if (hmant != 0) {
            for (; exp2 > 60; exp2 -= 60)  v *= 0x1p60L;
            for (; exp2 < -60; exp2 += 60) v *= 0x1p-60L;
            const long double* table = (exp2 >= 0) ? up : down;
            unsigned bits = (exp2 >= 0) ? (unsigned)exp2 : (unsigned)-exp2;
            for (int i = 0; bits; i++, bits >>= 1) {
                if (bits & 1) v *= table[i];
            }
        }
        return neg ? -v : v;
    }

    unsigned long long mant = 0;
    int digits = 0, exp10 = 0;
    bool any = false;

    for (; isdigit((unsigned char)*p); p++) {
        any = true;
        if (digits < 19) {
            if (mant || *p != '0') { mant = mant * 10 + (unsigned)(*p - '0'); digits++; }
        } else {
            exp10++;    // Dropped digit: still counts toward the magnitude
        }
    }
    if (*p == '.') {
        p++;
        for (; isdigit((unsigned char)*p); p++) {
            any = true;
            if (digits < 19) {
                if (mant || *p != '0') { mant = mant * 10 + (unsigned)(*p - '0'); digits++; }
                exp10--;
            }
        }
    }

    if (!any) {
        if (end) *end = (char*)s;
        return 0.0L;
    }

    if ((*p | 32) == 'e') {
        const char* q = p + 1;
        bool eneg = false;
        if (*q == '+' || *q == '-') { eneg = (*q == '-'); q++; }
        if (isdigit((unsigned char)*q)) {
            int e = 0;
            for (; isdigit((unsigned char)*q); q++) {
                if (e < 100000) e = e * 10 + (*q - '0');
            }
            exp10 += eneg ? -e : e;
            p = q;
        }
    }
    if (end) *end = (char*)p;

    long double v = (long double)mant;
    if (mant != 0) {
        // Two steps keep 1e-4950 style intermediates from flushing to zero
        if (exp10 < -4000) { v *= pow10_ld(-4000); exp10 += 4000; }
        v *= pow10_ld(exp10);
    }
    return neg ? -v : v;
}

// ERANGE on overflow to infinity and on underflow (a zero or subnormal
// result that is not exact), as glibc reports them
double strtod(const char* restrict s, char** restrict end) {
    long double v = strtold(s, end);
    double d = (double)v;
    if (__builtin_isinf(d) && !__builtin_isinf(v))
        errno = ERANGE;
    else if (__builtin_fabs(d) < __DBL_MIN__ && (long double)d != v)
        errno = ERANGE;
    return d;
}

float strtof(const char* restrict s, char** restrict end) {
    long double v = strtold(s, end);
    float f = (float)v;
    if (__builtin_isinf(f) && !__builtin_isinf(v))
        errno = ERANGE;
    else if (__builtin_fabsf(f) < __FLT_MIN__ && (long double)f != v)
        errno = ERANGE;
    return f;
}

double atof(const char* s) { return strtod(s, NULL); }

//
// Arithmetic
//

int       abs(int x)        { return x < 0 ? -x : x; }
long      labs(long x)      { return x < 0 ? -x : x; }
long long llabs(long long x){ return x < 0 ? -x : x; }

div_t   div(int n, int d)              { return (div_t){ n / d, n % d }; }
ldiv_t  ldiv(long n, long d)           { return (ldiv_t){ n / d, n % d }; }
lldiv_t lldiv(long long n, long long d){ return (lldiv_t){ n / d, n % d }; }

//
// Random numbers: 64-bit LCG (Knuth MMIX constants), high bits out
//

static uint64_t rand_state = 1;

void srand(unsigned int seed) {
    rand_state = seed;
}

int rand(void) {
    rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)(rand_state >> 33);
}

//
// Sorting and searching
//

static void swap_bytes(char* a, char* b, size_t size) {
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

// Introsort-free quicksort: median of three, insertion sort for small runs,
// recursion only on the smaller side (stack depth O(log n))
void qsort(void* base, size_t count, size_t size, int (*cmp)(const void*, const void*)) {
    char* lo = (char*)base;

    while (count > 1) {
        if (count <= 8) {
            for (size_t i = 1; i < count; i++) {
                for (size_t j = i; j > 0 && cmp(lo + (j - 1) * size, lo + j * size) > 0; j--)
                    swap_bytes(lo + (j - 1) * size, lo + j * size, size);
            }
            return;
        }

        char* mid = lo + (count / 2) * size;
        char* hi  = lo + (count - 1) * size;
        if (cmp(mid, lo) < 0) swap_bytes(mid, lo, size);
        if (cmp(hi, lo) < 0)  swap_bytes(hi, lo, size);
        if (cmp(hi, mid) < 0) swap_bytes(hi, mid, size);

        // Pivot (median) to the second-to-last slot, then partition
        char* pivot = hi - size;
        swap_bytes(mid, pivot, size);

        char* i = lo;
        char* j = pivot;
        for (;;) {
            do { i += size; } while (cmp(i, pivot) < 0);
            do { j -= size; } while (cmp(j, pivot) > 0);
            if (i >= j) break;
            swap_bytes(i, j, size);
        }
        swap_bytes(i, pivot, size);

        size_t left  = (size_t)(i - lo) / size;
        size_t right = count - left - 1;
        if (left < right) {
            qsort(lo, left, size, cmp);
            lo = i + size;
            count = right;
        } else {
            qsort(i + size, right, size, cmp);
            count = left;
        }
    }
}

void* bsearch(const void* key, const void* base, size_t count, size_t size,
              int (*cmp)(const void*, const void*)) {
    const char* lo = (const char*)base;
    while (count > 0) {
        const char* mid = lo + (count / 2) * size;
        int r = cmp(key, mid);
        if (r == 0)
            return (void*)mid;
        if (r > 0) {
            lo = mid + size;
            count -= count / 2 + 1;
        } else {
            count /= 2;
        }
    }
    return NULL;
}


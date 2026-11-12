/*
 * math.h on the x87. Everything is computed in long double; the float and
 * double versions round the result once on return.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

//
// Bits
//

int __signbitf(float x) {
    union { float f; uint32_t u; } v = { x };
    return (int)(v.u >> 31);
}

int __signbit(double x) {
    union { double d; uint64_t u; } v = { x };
    return (int)(v.u >> 63);
}

int __signbitl(long double x) {
    union { long double l; struct { uint64_t mant; uint16_t se; } b; } v = { x };
    return v.b.se >> 15;
}

int __fpclassifyf(float x) {
    union { float f; uint32_t u; } v = { x };
    uint32_t e = (v.u >> 23) & 0xFF, m = v.u & 0x7FFFFF;
    if (e == 0xFF) return m ? FP_NAN : FP_INFINITE;
    if (e == 0)    return m ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

int __fpclassify(double x) {
    union { double d; uint64_t u; } v = { x };
    uint32_t e = (uint32_t)(v.u >> 52) & 0x7FF;
    uint64_t m = v.u & 0xFFFFFFFFFFFFFULL;
    if (e == 0x7FF) return m ? FP_NAN : FP_INFINITE;
    if (e == 0)     return m ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

int __fpclassifyl(long double x) {
    union { long double l; struct { uint64_t mant; uint16_t se; } b; } v = { x };
    uint32_t e = v.b.se & 0x7FFF;
    uint64_t m = v.b.mant & 0x7FFFFFFFFFFFFFFFULL;     // Without the explicit integer bit
    if (e == 0x7FFF) return m ? FP_NAN : FP_INFINITE;
    if (e == 0)      return v.b.mant ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

//
// Sign and magnitude
//

long double fabsl(long double x) {
    __asm__("fabs" : "+t"(x));
    return x;
}
double fabs(double x) { return (double)fabsl(x); }
float  fabsf(float x) { return (float)fabsl(x); }

long double copysignl(long double x, long double y) {
    x = fabsl(x);
    return __signbitl(y) ? -x : x;
}
double copysign(double x, double y) { return (double)copysignl(x, y); }
float  copysignf(float x, float y)  { return (float)copysignl(x, y); }

long double sqrtl(long double x) {
    __asm__("fsqrt" : "+t"(x));
    return x;
}
double sqrt(double x) { return (double)sqrtl(x); }
float  sqrtf(float x) { return (float)sqrtl(x); }

//
// Exponent
//

long double ldexpl(long double x, int exp) {
    long double r;
    // fscale: ST0 * 2^trunc(ST1). ST1 is an input only, so the compiler pops it
    __asm__("fscale" : "=t"(r) : "0"(x), "u"((long double)exp));
    return r;
}
double ldexp(double x, int exp) { return (double)ldexpl(x, exp); }
float  ldexpf(float x, int exp) { return (float)ldexpl(x, exp); }

long double scalbnl(long double x, int exp) { return ldexpl(x, exp); }
double      scalbn(double x, int exp)       { return ldexp(x, exp); }
float       scalbnf(float x, int exp)       { return ldexpf(x, exp); }

long double frexpl(long double x, int* exp) {
    int c = __fpclassifyl(x);
    if (c == FP_ZERO || c == FP_NAN || c == FP_INFINITE) {
        *exp = 0;
        return x;
    }
    // fxtract: ST0 = significand in [1, 2), ST1 = unbiased exponent.
    // Subnormals are normalized by the instruction itself.
    long double sig, e;
    __asm__("fxtract" : "=t"(sig), "=u"(e) : "0"(x));
    *exp = (int)e + 1;
    return sig / 2;
}
double frexp(double x, int* exp) { return (double)frexpl(x, exp); }
float  frexpf(float x, int* exp) { return (float)frexpl(x, exp); }

//
// Rounding
//

#define RC_NEAREST  0x0000
#define RC_DOWN     0x0400
#define RC_UP       0x0800
#define RC_ZERO     0x0C00

// frndint under rounding control `rc`, then the caller's mode back
static long double round_mode(long double x, uint16_t rc) {
    uint16_t saved, cw;
    __asm__ volatile("fnstcw %0" : "=m"(saved));
    cw = (uint16_t)((saved & ~0x0C00) | rc);
    __asm__ volatile("fldcw %0" : : "m"(cw));
    __asm__ volatile("frndint" : "+t"(x));
    __asm__ volatile("fldcw %0" : : "m"(saved));
    return x;
}

long double floorl(long double x) { return round_mode(x, RC_DOWN); }
long double ceill(long double x)  { return round_mode(x, RC_UP); }
long double truncl(long double x) { return round_mode(x, RC_ZERO); }
double floor(double x) { return (double)floorl(x); }
float  floorf(float x) { return (float)floorl(x); }
double ceil(double x)  { return (double)ceill(x); }
float  ceilf(float x)  { return (float)ceill(x); }
double trunc(double x) { return (double)truncl(x); }
float  truncf(float x) { return (float)truncl(x); }

long double roundl(long double x) {
    long double t = truncl(x);
    // Exact in extended precision, so 0.49999999999999994 stays below a half
    if (fabsl(x - t) >= 0.5L)
        t += __signbitl(x) ? -1.0L : 1.0L;
    return t;
}
double round(double x) { return (double)roundl(x); }
float  roundf(float x) { return (float)roundl(x); }

long double modfl(long double x, long double* ip) {
    long double t = truncl(x);
    *ip = t;
    if (isinf(x))
        return copysignl(0.0L, x);
    return copysignl(x - t, x);     // Keeps -0.0 for negative whole numbers
}
double modf(double x, double* ip) {
    long double i;
    double f = (double)modfl(x, &i);
    *ip = (double)i;
    return f;
}
float modff(float x, float* ip) {
    long double i;
    float f = (float)modfl(x, &i);
    *ip = (float)i;
    return f;
}

//
// Remainder
//

long double fmodl(long double x, long double y) {
    long double r;
    uint16_t sw;
    // fprem reduces the exponent gap by at most 63 per pass: repeat while
    // the status word's C2 (partial) bit is set
    __asm__ volatile(
        "1: fprem\n"
        "   fnstsw %%ax\n"
        "   testw $0x400, %%ax\n"
        "   jnz 1b\n"
        : "=t"(r), "=a"(sw) : "0"(x), "u"(y));
    (void)sw;
    return r;
}
double fmod(double x, double y) { return (double)fmodl(x, y); }
float  fmodf(float x, float y)  { return (float)fmodl(x, y); }

//
// Trigonometry
//

// pi/2 in three parts. The first two have 32-bit significands, so k * part
// is exact for |k| < 2^32 and the reduction keeps all of x's precision.
#define PIO2_1  0xc90fdaa2p-31L
#define PIO2_2  0x85a308d3p-65L
#define PIO2_3  0x98cc51701b839a25p-132L

long double cosl(long double x) {
    if (!isfinite(x))
        return NAN;
    long double r = x, s, c;
    int quadrant = 0;
    // fcos reduces with a 66-bit pi, which loses digits for large x: reduce
    // to [-pi/4, pi/4] here instead. Past 2^31 * pi/2 fcos's own reduction
    // (or fprem for |x| >= 2^63) is used, which is only approximate.
    if (fabsl(x) > 0.785398163397448309615L && fabsl(x) < 0x1p31L * PIO2_1) {
        long double k = roundl(x / (PIO2_1 + PIO2_2));
        r = ((x - k * PIO2_1) - k * PIO2_2) - k * PIO2_3;
        quadrant = (int)((int64_t)k & 3);
    } else if (fabsl(x) >= 0x1p63L) {
        long double two_pi;
        __asm__("fldpi\n fadd %%st(0), %%st" : "=t"(two_pi));
        r = fmodl(x, two_pi);
    }
    // fsincos: ST0 = cos, ST1 = sin
    __asm__("fsincos" : "=t"(c), "=u"(s) : "0"(r));
    switch (quadrant) {
    case 1:  return -s;
    case 2:  return -c;
    case 3:  return s;
    default: return c;
    }
}
double cos(double x) { return (double)cosl(x); }
float  cosf(float x) { return (float)cosl(x); }

long double acosl(long double x) {
    long double r;
    // atan2(sqrt(1 - x^2), x); |x| > 1 gives NaN through the sqrt
    long double s = sqrtl((1.0L - x) * (1.0L + x));
    // fpatan: atan(ST1 / ST0) with the quadrant from both signs, popping ST1
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(s) : "st(1)");
    return r;
}
double acos(double x) { return (double)acosl(x); }
float  acosf(float x) { return (float)acosl(x); }

//
// Powers
//

// 2^t for any finite t: f2xm1 covers |f| <= 1, fscale the whole part
static long double exp2_x87(long double t) {
    if (t > 20000.0L) return INFINITY;
    if (t < -20000.0L) return 0.0L;
    long double n = roundl(t), f = t - n, r;
    __asm__("f2xm1" : "+t"(f));
    f += 1.0L;
    __asm__("fscale" : "=t"(r) : "0"(f), "u"(n));
    return r;
}

// 0: not an integer, 1: odd integer, 2: even integer
static int int_kind(long double y) {
    if (!isfinite(y) || truncl(y) != y) return 0;
    return fmodl(y, 2.0L) != 0.0L ? 1 : 2;
}

long double powl(long double x, long double y) {
    if (y == 0.0L || x == 1.0L) return 1.0L;
    if (isnan(x) || isnan(y))   return NAN;

    long double ax = fabsl(x);
    if (isinf(y)) {
        if (ax == 1.0L) return 1.0L;
        return ((ax < 1.0L) == (y > 0.0L)) ? 0.0L : INFINITY;
    }

    int kind = int_kind(y);
    if (x == 0.0L || isinf(x)) {
        // 0^y and inf^y: magnitude from the sign of y (flipped for zero),
        // sign of x kept only for odd integer y
        bool big = (x == 0.0L) ? (y < 0.0L) : (y > 0.0L);
        long double r = big ? INFINITY : 0.0L;
        return (kind == 1) ? copysignl(r, x) : r;
    }
    if (x < 0.0L && kind == 0) return NAN;

    // |x|^y = 2^(y * log2|x|); fyl2x: ST1 * log2(ST0), popping ST1
    long double t;
    __asm__("fyl2x" : "=t"(t) : "0"(ax), "u"(y) : "st(1)");
    long double r = exp2_x87(t);
    return (x < 0.0L && kind == 1) ? -r : r;
}
double pow(double x, double y) { return (double)powl(x, y); }
float  powf(float x, float y)  { return (float)powl(x, y); }

//
// Exponentials and logarithms
//

long double exp2l(long double x) {
    if (isnan(x)) return x;
    return exp2_x87(x);     // Infinities fall in its range checks
}
double exp2(double x) { return (double)exp2l(x); }
float  exp2f(float x) { return (float)exp2l(x); }

long double expl(long double x) {
    if (isnan(x)) return x;
    if (isinf(x)) return x > 0.0L ? x : 0.0L;
    long double log2e;
    __asm__("fldl2e" : "=t"(log2e));
    return exp2_x87(x * log2e);
}
double exp(double x) { return (double)expl(x); }
float  expf(float x) { return (float)expl(x); }

// scale * log2(x): fyl2x does ST1 * log2(ST0), popping ST1
static long double log_x87(long double x, long double scale) {
    if (isnan(x) || x < 0.0L) return NAN;
    if (x == 0.0L)            return -INFINITY;
    if (isinf(x))             return x;
    long double r;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(scale) : "st(1)");
    return r;
}

long double logl(long double x) {
    long double ln2;
    __asm__("fldln2" : "=t"(ln2));
    return log_x87(x, ln2);
}
double log(double x) { return (double)logl(x); }
float  logf(float x) { return (float)logl(x); }

long double log2l(long double x) { return log_x87(x, 1.0L); }
double log2(double x) { return (double)log2l(x); }
float  log2f(float x) { return (float)log2l(x); }

long double log10l(long double x) {
    long double lg2;
    __asm__("fldlg2" : "=t"(lg2));
    return log_x87(x, lg2);
}
double log10(double x) { return (double)log10l(x); }
float  log10f(float x) { return (float)log10l(x); }

//
// Rounding to integers
//

long      lroundl(long double x)  { return (long)roundl(x); }
long      lround(double x)        { return (long)roundl(x); }
long      lroundf(float x)        { return (long)roundl(x); }
long long llroundl(long double x) { return (long long)roundl(x); }
long long llround(double x)       { return (long long)roundl(x); }
long long llroundf(float x)       { return (long long)roundl(x); }

//
// Neighbors
//

double nextafter(double x, double y) {
    if (isnan(x) || isnan(y)) return x + y;
    if (x == y)               return y;
    union { double d; uint64_t u; } v = { x };
    if (x == 0.0)
        v.u = (signbit(y) ? 0x8000000000000000ULL : 0) | 1;     // Smallest subnormal toward y
    else if ((x < y) == (x > 0.0))
        v.u++;
    else
        v.u--;
    return v.d;
}

float nextafterf(float x, float y) {
    if (isnan(x) || isnan(y)) return x + y;
    if (x == y)               return y;
    union { float f; uint32_t u; } v = { x };
    if (x == 0.0f)
        v.u = (signbit(y) ? 0x80000000u : 0) | 1;
    else if ((x < y) == (x > 0.0f))
        v.u++;
    else
        v.u--;
    return v.f;
}

#include <stdint.h>
#include <stddef.h>
#include <limits.h>

#include <kernel/util/math.h>

// Integers

int abs(int j) {
    // Note: abs(INT_MIN) is undefined, as in standard C
    return (j < 0) ? -j : j;
}

int max(int a, int b) {
    return (a > b) ? a : b;
}

int min(int a, int b) {
    return (a < b) ? a : b;
}

int clamp(int value, int min_val, int max_val) {
    return min(max(value, min_val), max_val);
}

unsigned int isqrt(unsigned int num) {
    unsigned int res = 0;
    // Highest power of 4 representable; the old "1 << 30" overflowed on
    // 16-bit-int targets such as AVR.
    unsigned int bit = 1u << (sizeof(unsigned int) * CHAR_BIT - 2);

    while (bit > num) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (num >= res + bit) {
            num -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

// Exponentiation by squaring for integers
long long ipow(long long base, int exp) {
    long long res = 1;
    if (exp < 0) return (base == 1) ? 1 : ((base == -1) ? ((exp & 1) ? -1 : 1) : 0);
    while (exp > 0) {
        if (exp & 1) res *= base;
        exp >>= 1;
        if (exp) base *= base;  // skip the final (unused, possibly overflowing) square
    }
    return res;
}

uint32_t iatan2(int32_t y, int32_t x) {
    if (x == 0 && y == 0) return 0;

    // Magnitudes in unsigned 32-bit: negating in unsigned arithmetic maps
    // INT32_MIN cleanly to 2^31 with no overflow.
    uint32_t abs_y = y < 0 ? 0u - (uint32_t)y : (uint32_t)y;
    uint32_t abs_x = x < 0 ? 0u - (uint32_t)x : (uint32_t)x;
    
    uint32_t min_val = (abs_x < abs_y) ? abs_x : abs_y;
    uint32_t max_val = (abs_x > abs_y) ? abs_x : abs_y;
    
    // Scale both down until min_val << 12 fits in 32 bits. Only the ratio
    // matters, and max_val stays >= 2^19 so precision is unaffected. This
    // keeps everything to a 32-bit divide (no 64-bit libgcc helper needed).
    while (max_val >= (1u << 20)) {
        min_val >>= 1;
        max_val >>= 1;
    }
    
    // Fixed-point ratio scaled to 12-bit fraction [0, 4096]
    int32_t ratio = (int32_t)((min_val << 12) / max_val);
    
    // Polynomial integer approximation for atan(r) where r is in [0, 1]
    int32_t base = ratio * 2;
    int32_t correction = (((ratio * (4096 - ratio)) >> 12) * 2928) >> 12;
    uint32_t angle = (uint32_t)(base + correction);

    // Octant swapping if slope > 1
    if (abs_y > abs_x) {
        angle = 16384 - angle;
    }

    // Map quadrant to full circle [0, 65535]
    if (x < 0) {
        if (y < 0) {
            angle = 32768 + angle; // Quadrant 3
        } else {
            angle = 32768 - angle; // Quadrant 2
        }
    } else if (y < 0) {
        angle = 65536 - angle;     // Quadrant 4
    }

    return angle & 0xFFFF;
}

// Floating point (x87 Inline Assembly)

#if defined(__i386__) || defined(__x86_64__)

// 2^52: every double with magnitude >= this is already an integer
#define TWO_POW_52 4503599627370496.0

static inline int is_nan(double x) { return x != x; }

double sin(double x) {
    // fsin is only valid for |x| < 2^63
    __asm__ ("fsin" : "+t" (x));
    return x;
}

double cos(double x) {
    __asm__ ("fcos" : "+t" (x));
    return x;
}

double tan(double x) {
    // fptan pushes 1.0 after the result; pop it
    __asm__ ("fptan\n\t"
             "fstp %%st(0)"
             : "+t" (x));
    return x;
}

double fabs(double x) {
    __asm__ ("fabs" : "+t" (x));
    return x;
}

double sqrt(double x) {
    if (x < 0) return NAN;
    __asm__ ("fsqrt" : "+t" (x));
    return x;
}

double floor(double x) {
    // Large values, infinities and NaN would overflow the long long cast (UB)
    if (is_nan(x) || x >= TWO_POW_52 || x <= -TWO_POW_52) return x;
    long long i = (long long)x;
    double d = (double)i;
    return (x < d) ? d - 1.0 : d;
}

double ceil(double x) {
    if (is_nan(x) || x >= TWO_POW_52 || x <= -TWO_POW_52) return x;
    long long i = (long long)x;
    double d = (double)i;
    return (x > d) ? d + 1.0 : d;
}

// Arc tangent of y/x using full quadrant sign tracking
double atan2(double y, double x) {
    double res;
    __asm__ (
        "fpatan"
        : "=t" (res)
        : "0" (x), "u" (y)
        : "st(1)"
    );
    return res;
}

// Natural logarithm (ln) using instruction fyl2x: st(1) * log2(st(0))
double log(double x) {
    if (is_nan(x)) return x;
    if (x <= 0.0) return (x == 0.0) ? -INFINITY : NAN;
    double res;
    __asm__ (
        "fldln2\n\t"
        "fxch\n\t"
        "fyl2x"
        : "=t" (res)
        : "0" (x)
    );
    return res;
}

// Exponential function (e^x) leveraging x87 f2xm1 (calculates 2^frac - 1)
double exp(double x) {
    if (is_nan(x)) return x;
    if (x > 709.782712893384) return INFINITY;  // beyond DBL_MAX
    if (x < -745.1332191019412) return 0.0;     // below smallest denormal

    double res;
    double const l2e = 1.4426950408889634074; // log2(e)
    double scaled = x * l2e;

    double int_part = floor(scaled + 0.5);
    double frac_part = scaled - int_part;     // in [-0.5, 0.5], valid for f2xm1

    __asm__ (
        "f2xm1\n\t"       // st(0) = 2^frac_part - 1
        "fld1\n\t"        // st(0) = 1, st(1) = 2^frac_part - 1
        "faddp\n\t"       // st(0) = 2^frac_part
        "fscale"          // st(0) = 2^frac_part * 2^int_part
        : "=t" (res)
        : "0" (frac_part), "u" (int_part)
    );
    return res;
}

// Power function combining exp and log
double pow(double base, double exponent) {
    if (exponent == 0.0) return 1.0;
    if (is_nan(base) || is_nan(exponent)) return NAN;

    if (base < 0.0) {
        // Fractional powers of negative numbers produce imaginary numbers
        if (exponent != floor(exponent)) return NAN;

        double res = exp(exponent * log(-base));
        // Integers this large are always even; avoids an overflowing cast
        if (exponent >= TWO_POW_52 || exponent <= -TWO_POW_52) return res;
        return (((long long)exponent) & 1) ? -res : res;
    }
    if (base == 0.0) {
        return (exponent > 0.0) ? 0.0 : INFINITY;
    }
    return exp(exponent * log(base));
}

#endif

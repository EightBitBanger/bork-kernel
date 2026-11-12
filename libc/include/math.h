#ifndef _MATH_H
#define _MATH_H

/*
 * x87-backed basics (src/math.c): exponent handling, rounding, remainders,
 * sqrt, fabs, cos, acos, pow, exp and log. No sin or tan yet.
 * errno is not set; results follow the x87 (NaN in, NaN out; overflow
 * gives an infinity).
 */

#if defined(__GNUC__) || defined(__clang__)
#define HUGE_VAL    __builtin_huge_val()
#define HUGE_VALF   __builtin_huge_valf()
#define HUGE_VALL   __builtin_huge_vall()
#define INFINITY    __builtin_inff()
#define NAN         __builtin_nanf("")
#else
#define HUGE_VAL    (1e5000)
#define HUGE_VALF   (1e5000f)
#define HUGE_VALL   (1e5000L)
#define INFINITY    (1e5000f)
#define NAN         (0.0f / 0.0f)
/* Radians; acos returns [0, pi], NaN outside [-1, 1] */
double      cos(double x);
float       cosf(float x);
long double cosl(long double x);
double      acos(double x);
float       acosf(float x);
long double acosl(long double x);

/* C99 special cases: pow(x, 0) = pow(1, y) = 1, negative x needs integer y */
double      pow(double x, double y);
float       powf(float x, float y);
long double powl(long double x, long double y);

#endif

#define M_E         2.7182818284590452354
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_SQRT2     1.41421356237309504880

#define FP_NAN          0
#define FP_INFINITE     1
#define FP_ZERO         2
#define FP_SUBNORMAL    3
#define FP_NORMAL       4

/* Type-generic, written so any compiler (GCC, TinyCC) can expand them */
#define isnan(x)        ((x) != (x))
#define isinf(x)        (!isnan(x) && isnan((x) - (x)))
#define isfinite(x)     (!isnan((x) - (x)))
#define isnormal(x)     (fpclassify(x) == FP_NORMAL)
#define signbit(x)      (sizeof(x) == sizeof(float)  ? __signbitf((float)(x)) : \
                         sizeof(x) == sizeof(double) ? __signbit((double)(x)) : \
                                                       __signbitl((long double)(x)))
#define fpclassify(x)   (sizeof(x) == sizeof(float)  ? __fpclassifyf((float)(x)) : \
                         sizeof(x) == sizeof(double) ? __fpclassify((double)(x)) : \
                                                       __fpclassifyl((long double)(x)))

int __signbitf(float x);
int __signbit(double x);
int __signbitl(long double x);
int __fpclassifyf(float x);
int __fpclassify(double x);
int __fpclassifyl(long double x);

double      fabs(double x);
float       fabsf(float x);
long double fabsl(long double x);
double      copysign(double x, double y);
float       copysignf(float x, float y);
long double copysignl(long double x, long double y);

double      sqrt(double x);
float       sqrtf(float x);
long double sqrtl(long double x);

/* x * 2^exp */
double      ldexp(double x, int exp);
float       ldexpf(float x, int exp);
long double ldexpl(long double x, int exp);
double      scalbn(double x, int exp);
float       scalbnf(float x, int exp);
long double scalbnl(long double x, int exp);

/* x = m * 2^*exp with 0.5 <= |m| < 1 (m = x, *exp = 0 for 0, inf, NaN) */
double      frexp(double x, int* exp);
float       frexpf(float x, int* exp);
long double frexpl(long double x, int* exp);

double      floor(double x);
float       floorf(float x);
long double floorl(long double x);
double      ceil(double x);
float       ceilf(float x);
long double ceill(long double x);
double      trunc(double x);
float       truncf(float x);
long double truncl(long double x);
double      round(double x);        /* Halfway cases away from zero */
float       roundf(float x);
long double roundl(long double x);

/* Integer and fractional parts, both with the sign of x */
double      modf(double x, double* ip);
float       modff(float x, float* ip);
long double modfl(long double x, long double* ip);

/* x - n*y, n = trunc(x/y): the sign of x */
double      fmod(double x, double y);
float       fmodf(float x, float y);
long double fmodl(long double x, long double y);

/* Radians; acos returns [0, pi], NaN outside [-1, 1] */
double      cos(double x);
float       cosf(float x);
long double cosl(long double x);
double      acos(double x);
float       acosf(float x);
long double acosl(long double x);

/* C99 special cases: pow(x, 0) = pow(1, y) = 1, negative x needs integer y */
double      pow(double x, double y);
float       powf(float x, float y);
long double powl(long double x, long double y);

/* e^x, 2^x; natural, base 2 and base 10 logarithms: log(0) = -inf,
   log(x < 0) = NaN */
double      exp(double x);
float       expf(float x);
long double expl(long double x);
double      exp2(double x);
float       exp2f(float x);
long double exp2l(long double x);
double      log(double x);
float       logf(float x);
long double logl(long double x);
double      log2(double x);
float       log2f(float x);
long double log2l(long double x);
double      log10(double x);
float       log10f(float x);
long double log10l(long double x);

/* Rounded as round() does, to an integer type. Out of range is undefined. */
long        lround(double x);
long        lroundf(float x);
long        lroundl(long double x);
long long   llround(double x);
long long   llroundf(float x);
long long   llroundl(long double x);

/* The next representable value after x in the direction of y */
double      nextafter(double x, double y);
float       nextafterf(float x, float y);

#endif

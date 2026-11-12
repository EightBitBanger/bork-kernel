#ifndef KERNEL_UTIL_MATH_H
#define KERNEL_UTIL_MATH_H

#include <stdint.h>
#include <stddef.h>

#define M_PI       3.14159265358979323846
#define M_E        2.71828182845904523536
#define INFINITY   (__builtin_inf())
#define NAN        (__builtin_nan(""))

int abs(int j);
int max(int a, int b);
int min(int a, int b);

int clamp(int value, int min_val, int max_val);
unsigned int isqrt(unsigned int num);
long long ipow(long long base, int exp);

// Returns an angle in binary-angle units: 0..65535 maps to 0..2*pi
uint32_t iatan2(int32_t y, int32_t x);

// Floating point (x87 only)

#if defined(__i386__) || defined(__x86_64__)

double sin(double x);
double cos(double x);
double tan(double x);

double fabs(double x);
double sqrt(double x);
double floor(double x);
double ceil(double x);
double atan2(double y, double x);

double log(double x);
double exp(double x);
double pow(double base, double exponent);

#endif

// 64-bit division helpers required by GCC on 32-bit targets
uint64_t __umoddi3(uint64_t num, uint64_t den);
uint64_t __udivdi3(uint64_t num, uint64_t den);
uint64_t __udivmoddi4(uint64_t num, uint64_t den, uint64_t *rem);

#endif

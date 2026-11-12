/*
 * 64-bit division helpers GCC emits on i386 for long long '/' and '%'.
 * Normally they come from libgcc; providing them here means user programs
 * link with nothing but libc.a. This file must not divide 64-bit values
 * itself (it would call itself).
 *
 * Weak, so a compiler's own runtime library wins when both are linked:
 * TinyCC's libtcc1.a defines the same four helpers.
 */

#include <stdint.h>

__attribute__((weak)) uint64_t __udivmoddi4(uint64_t num, uint64_t den, uint64_t* rem) {
    if (den == 0) {
        // Undefined in C; the hardware would fault, so do the same
        __asm__ volatile("ud2");
    }

    // Fast path: both fit 32 bits
    if ((num >> 32) == 0 && (den >> 32) == 0) {
        uint32_t n = (uint32_t)num, d = (uint32_t)den;
        if (rem) *rem = n % d;
        return n / d;
    }

    uint64_t quot = 0;
    if (den <= num) {
        int shift = __builtin_clzll(den) - __builtin_clzll(num);
        den <<= shift;
        for (; shift >= 0; shift--) {
            quot <<= 1;
            if (num >= den) {
                num -= den;
                quot |= 1;
            }
            den >>= 1;
        }
    }
    if (rem) *rem = num;
    return quot;
}

__attribute__((weak)) uint64_t __udivdi3(uint64_t num, uint64_t den) {
    return __udivmoddi4(num, den, 0);
}

__attribute__((weak)) uint64_t __umoddi3(uint64_t num, uint64_t den) {
    uint64_t r;
    __udivmoddi4(num, den, &r);
    return r;
}

__attribute__((weak)) int64_t __divdi3(int64_t num, int64_t den) {
    int neg = (num < 0) ^ (den < 0);
    uint64_t q = __udivmoddi4(num < 0 ? 0 - (uint64_t)num : (uint64_t)num,
                              den < 0 ? 0 - (uint64_t)den : (uint64_t)den, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}

__attribute__((weak)) int64_t __moddi3(int64_t num, int64_t den) {
    uint64_t r;
    __udivmoddi4(num < 0 ? 0 - (uint64_t)num : (uint64_t)num,
                 den < 0 ? 0 - (uint64_t)den : (uint64_t)den, &r);
    return num < 0 ? -(int64_t)r : (int64_t)r;
}

__attribute__((weak)) int64_t __divmoddi4(int64_t num, int64_t den, int64_t* rem) {
    int64_t q = __divdi3(num, den);
    if (rem) *rem = num - q * den;
    return q;
}


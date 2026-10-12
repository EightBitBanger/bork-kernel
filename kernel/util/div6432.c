#include <stdint.h>
#include <stddef.h>

#include <kernel/util/math.h>

/*
 * libgcc-compatible 64-bit unsigned division helpers. GCC emits calls to
 * these on 32-bit targets for any uint64_t '/' or '%'.
 *
 * This file must not itself perform 64-bit division (it would recurse).
 */

uint64_t __udivmoddi4(uint64_t num, uint64_t den, uint64_t *rem) {
    uint64_t quot = 0, qbit = 1;
    
    if (den == 0) {
        // Division by zero is undefined; return 0 and leave a defined remainder
        if (rem) *rem = num;
        return 0;
    }
    
    // Align den with num (stop before den's top bit would shift out)
    while ((int64_t)den >= 0 && den <= num) {
        den <<= 1;
        qbit <<= 1;
    }
    
    while (qbit > 0) {
        if (num >= den) {
            num -= den;
            quot |= qbit;
        }
        den >>= 1;
        qbit >>= 1;
    }
    
    if (rem) *rem = num;
    return quot;
}

uint64_t __udivdi3(uint64_t num, uint64_t den) {
    return __udivmoddi4(num, den, NULL);
}

uint64_t __umoddi3(uint64_t num, uint64_t den) {
    uint64_t rem;
    __udivmoddi4(num, den, &rem);
    return rem;
}

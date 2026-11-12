/* Test stub: what the kernel's gfx code needs from <kernel/util/math.h> */
#ifndef _KSTUB_MATH_H
#define _KSTUB_MATH_H
#include <stdint.h>
#include <stdlib.h>

/* Angle of (x, y) measured from the +y axis toward +x, as 0 .. 65536 for a
   full turn (the scale gfx_draw_pie expects). Approximate: tests only. */
static inline uint32_t iatan2(int32_t x, int32_t y) {
    double ax = x < 0 ? -(double)x : x, ay = y < 0 ? -(double)y : y;
    if (ax == 0 && ay == 0) return 0;
    double z = (ax < ay) ? ax / ay : ay / ax;
    double a = z * (0.7853981634 + 0.273 * (1.0 - z));      /* atan(z), z <= 1 */
    if (ax > ay) a = 1.5707963268 - a;                       /* angle from +y */
    if (y < 0) a = 3.1415926536 - a;
    if (x < 0) a = 6.2831853072 - a;
    return (uint32_t)(a * (65536.0 / 6.2831853072));
}
#endif


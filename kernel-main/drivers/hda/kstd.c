//
// The few C library functions hda.c and hda_codec.c use, for the driver
// file (the kernel's own are out of reach: kdrv_module.h)
//

#include <stdarg.h>
#include "kshim.h"

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

size_t strnlen(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n] != '\0')
        n++;
    return n;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (*s == '\0')
            return (char*)last;
    }
}

int snprintf(char* out, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int n = kapi->vsnprintf(out, size, format, args);
    va_end(args);
    return n;
}

// 64-bit division (libgcc's, which a driver file doesn't link)
uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t* rem);
uint64_t __udivdi3(uint64_t n, uint64_t d);
uint64_t __umoddi3(uint64_t n, uint64_t d);

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t* rem) {
    uint64_t q = 0, r = 0;
    if (d == 0) {
        if (rem) *rem = 0;
        return 0;
    }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << i;
        }
    }
    if (rem) *rem = r;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) {
    return __udivmoddi4(n, d, NULL);
}

uint64_t __umoddi3(uint64_t n, uint64_t d) {
    uint64_t r;
    __udivmoddi4(n, d, &r);
    return r;
}

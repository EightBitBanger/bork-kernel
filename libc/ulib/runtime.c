/*
 * What the compiler and stb-style code expect from <string.h> and
 * <stdlib.h>, for user libraries (os/ulib.h). build-ulib.sh links this and
 * libc's math.c into every library instead of libc.a, so a library carries
 * no state of its own: no heap (memory comes from the program through
 * ulib_host_t), no stdio, no system calls.
 *
 * Built with -fno-builtin and (GCC) -fno-tree-loop-distribute-patterns, so
 * these loops aren't turned back into calls to themselves.
 */

#include <stddef.h>
#include <stdint.h>

void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0) {
        for (; n >= 4; n -= 4, d += 4, s += 4)
            *(uint32_t*)(void*)d = *(const uint32_t*)(const void*)s;
    }
    while (n-- > 0)
        *d++ = *s++;
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0)
        return dest;
    if (d < s || d >= s + n)
        return memcpy(dest, src, n);
    while (n-- > 0)
        d[n] = s[n];
    return dest;
}

void* memset(void* dest, int c, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    if (((uintptr_t)d & 3u) == 0) {
        uint32_t v = (uint8_t)c * 0x01010101u;
        for (; n >= 4; n -= 4, d += 4)
            *(uint32_t*)(void*)d = v;
    }
    while (n-- > 0)
        *d++ = (uint8_t)c;
    return dest;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

int abs(int x) {
    return x < 0 ? -x : x;
}

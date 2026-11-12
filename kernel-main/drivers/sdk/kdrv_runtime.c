//
// What GCC may call by name even in a freestanding driver: struct copies
// and clears become memcpy / memset calls. Linked into every driver file, so
// a driver has no imports at all (everything else is in struct KernelApi).
//

#include <stddef.h>
#include <stdint.h>

void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = dest;
    const uint8_t* s = src;
    while (n--) *d++ = *s++;
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = dest;
    const uint8_t* s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        while (n--) d[n] = s[n];
    }
    return dest;
}

void* memset(void* dest, int value, size_t n) {
    uint8_t* d = dest;
    while (n--) *d++ = (uint8_t)value;
    return dest;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = a;
    const uint8_t* y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y)
            return *x - *y;
    }
    return 0;
}

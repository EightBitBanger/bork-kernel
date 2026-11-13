//
// The two C library functions athn_pci.c uses, for the driver file (the
// kernel's own are out of reach: kdrv_module.h)
//

#include <stdarg.h>
#include "kshim.h"

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

int snprintf(char* out, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int n = kapi->vsnprintf(out, size, format, args);
    va_end(args);
    return n;
}

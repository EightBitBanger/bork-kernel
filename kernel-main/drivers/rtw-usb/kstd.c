//
// The one C library function rtw_usb.c uses, for the driver file (the
// kernel's own are out of reach: kdrv_module.h)
//

#include "kshim.h"

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

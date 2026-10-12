#ifndef KERNEL_UTIL_SNPRINT_H
#define KERNEL_UTIL_SNPRINT_H

#include <stdarg.h>
#include <stddef.h>

/*
 * Supported: %c %s %d %i %u %x %X %p %%
 * Flags: '-' (left-justify), '0' (zero-pad)
 * Width: decimal number or '*'
 * Precision: '.N' or '.*' (max chars for %s, min digits for integers)
 * Length: hh h l ll z
 *
 * Returns the number of characters that WOULD have been written (excluding
 * the terminator), so a return value >= size means the output was truncated.
 */
int vsnprintf(char *str, size_t size, const char *format, va_list args);
int snprintf(char *str, size_t size, const char *format, ...);

#endif

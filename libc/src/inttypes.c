/*
 * inttypes.h functions. intmax_t is long long, so they forward to the
 * stdlib ones.
 */

#include <inttypes.h>
#include <stdlib.h>

intmax_t imaxabs(intmax_t x) {
    return llabs(x);
}

imaxdiv_t imaxdiv(intmax_t num, intmax_t den) {
    imaxdiv_t r = { num / den, num % den };
    return r;
}

intmax_t strtoimax(const char* restrict s, char** restrict end, int base) {
    return strtoll(s, end, base);
}

uintmax_t strtoumax(const char* restrict s, char** restrict end, int base) {
    return strtoull(s, end, base);
}

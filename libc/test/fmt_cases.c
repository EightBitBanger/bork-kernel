/*
 * Built twice: against this libc (through the Linux shim) and against the
 * host glibc. The two outputs must be identical. Only conversions whose
 * output does not depend on the ABI (no %p, no 64-bit long) are used.
 */

#include <errno.h>
#include <stddef.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T(...) do { \
    char b_[1200]; \
    int n_ = snprintf(b_, sizeof b_, __VA_ARGS__); \
    printf("%4d [%s] <- %s\n", n_, b_, #__VA_ARGS__); \
} while (0)

static void strtod_case(const char* s) {
    char* end;
    errno = 0;
    double d = strtod(s, &end);
    union { double d; unsigned long long u; } u = { d };
    printf("strtod(\"%s\") = %.17g bits=%016llx used=%d erange=%d\n",
           s, d, u.u, (int)(end - s), errno == ERANGE);
}

static void strtol_case(const char* s, int base) {
    char* end;
    errno = 0;
    long long v = strtoll(s, &end, base);
    int e1 = errno;
    errno = 0;
    unsigned long long uv = strtoull(s, &end, base);
    printf("strtoll(\"%s\",%d) = %lld used=%d erange=%d | strtoull = %llu erange=%d\n",
           s, base, v, (int)(end - s), e1 == ERANGE, uv, errno == ERANGE);
    errno = 0;
    int iv = (int)strtol(s, &end, base);   /* 32-bit long in this libc: only compare in range */
    (void)iv;
}

int main(void) {
    // Integers
    T("%d|%i|%u", 0, -1, 4000000000u);
    T("%d|%d", INT_MIN, INT_MAX);
    T("%5d|%-5d|%05d|%+d|% d|%+05d", 42, 42, 42, 42, 42, -42);
    T("%.3d|%.0d|%5.0d|%-+8.4d|%08.3d", 7, 0, 0, 12, -5);
    T("%x|%X|%#x|%#X|%#o|%o|%#o", 255u, 255u, 255u, 0u, 8u, 0u, 0u);
    T("%#.3o|%#5x|%#08x|%-#8x|", 8u, 1u, 1u, 1u);
    T("%hhd|%hhu|%hd|%hu", 300, 300, 70000, 70000);
    T("%lld|%llu|%llx", LLONG_MIN, ULLONG_MAX, 0x123456789abcdefULL);
    T("%jd|%zu|%td", (intmax_t)-5, (size_t)7, (ptrdiff_t)-3);
    T("%*d|%-*d|%.*d|%*.*d", 6, 1, 6, 2, 4, 3, 6, 3, 4);
    T("%*d", -6, 9);

    // Characters and strings
    T("%c|%5c|%-5c|", 'a', 'b', 'c');
    T("%s|%10s|%-10s|%.2s|%10.3s", "hello", "hi", "hi", "hello", "hello");
    T("%s", (char*)0);
    T("100%%|%%d");
    T("%5%|");

    // Floating point
    T("%f|%f|%f|%f", 0.0, -0.0, 1.0, -1.5);
    T("%f|%.0f|%.1f|%.2f|%.10f", 3.14159265358979, 2.5, 0.05, 0.125, 0.1);
    T("%.0f|%.0f|%.0f|%.0f|%.0f", 0.5, 1.5, 2.5, 3.5, -0.5);
    T("%.20f", 0.1);
    T("%.3f|%.3f|%.2f", 1.0005, 2.0005, 1.005);
    T("%e|%E|%.0e|%#.0e|%.3e", 12345.678, 0.000123, 5e10, 5e10, -9.9996);
    T("%g|%g|%g|%g|%g|%g", 100000.0, 1000000.0, 0.0001, 0.00001, 123.456, 0.0);
    T("%G|%#g|%.3g|%.10g|%g", 1e-10, 1.0, 1234567.0, 1.0/3, 1e300);
    T("%f", 1e21);
    T("%.0f", 1e23);
    T("%f", 1.7976931348623157e308);
    T("%e|%g|%.20e", 5e-324, 2.2250738585072014e-308, 5e-324);
    T("%.1074f", 5e-324);
    T("%10.3f|%-10.3f|%010.3f|%+.2f|% .2f|%+010.2f", 3.14159, 3.14159, -3.14159, 2.0, 2.0, 2.5);
    T("%f|%F|%e|%g|%5f|%-6f|%06f", 1.0/0.0, -1.0/0.0, 0.0/0.0, -(0.0/0.0), 1.0/0.0, 1.0/0.0, 1.0/0.0);
    T("%a|%a|%a|%a|%A", 1.0, 0.1, -2.5, 0.0, 255.5);
    T("%a|%a|%a", 5e-324, 1.7976931348623157e308, 2.2250738585072014e-308);
    T("%.0a|%.1a|%.3a|%.20a|%#.0a|%12.2a|%-12a|%012a", 1.5, 1.96875, 0.1, 0.1, 1.0, 3.0, 1.0, 1.0);
    T("%La|%La|%.3La", 1.0L, 0.1L, 1.0L/3);
    T("%.15g|%.17g|%.16g", 0.1, 0.1, 0.3);
    T("%#.0f|%#g|%#.3g", 3.0, 100000.0, 1.0);
    T("%g|%g|%g", 9.9999995, 0.000099999995, 999999.5);
    T("%Lf|%Le|%.25Lg", 1.0L/3, 2.0L/3, 1.0L/3);
    T("%Le", 1e4000L);
    T("%Lg", 1e-4000L);

    // Mixed and counts
    T("%s=%d (%5.1f%%)", "load", 3, 42.25);
    int n = 0;
    T("abc%n|%d", &n, 5);
    printf("%%n stored %d\n", n);

    // Truncation and return value
    {
        char small[8];
        int r = snprintf(small, sizeof small, "%s", "truncated output");
        printf("snprintf trunc: r=%d buf=[%s]\n", r, small);
        r = snprintf(NULL, 0, "%d-%s", 12345, "xyz");
        printf("snprintf NULL,0: r=%d\n", r);
    }

    // strtod
    const char* doubles[] = {
        "0", "1", "-2.5", "3.14159265358979323846", "1e10", "1E-5", "  +12.5e+2xyz",
        ".5", "5.", "0.1", "0.2", "0.3", "123456789012345678901234567890", "1e308",
        "1.7976931348623157e308", "1e309", "2.2250738585072014e-308", "4.9e-324",
        "1e-400", "inf", "-Infinity", "nan", "abc", "-0", "0x10", "0x1.8p3", "-0X.4P-2", "0x1p-1074", "0x1.fffffffffffff8p1023", "0x", "0x.p1", "1e", "1e+", "9007199254740993",
        "0.000000000000000000000000000001234", "6.02214076e23",
    };
    for (size_t i = 0; i < sizeof doubles / sizeof *doubles; i++)
        strtod_case(doubles[i]);

    // strtoll / strtoull
    strtol_case("0", 10);
    strtol_case("  -123abc", 10);
    strtol_case("0x1fZ", 0);
    strtol_case("0x1f", 16);
    strtol_case("017", 0);
    strtol_case("zz", 36);
    strtol_case("9223372036854775807", 10);
    strtol_case("9223372036854775808", 10);
    strtol_case("-9223372036854775808", 10);
    strtol_case("-9223372036854775809", 10);
    strtol_case("18446744073709551615", 10);
    strtol_case("18446744073709551616", 10);
    strtol_case("-1", 10);
    strtol_case("", 10);
    strtol_case("0x", 16);
    strtol_case("+", 10);
    return 0;
}


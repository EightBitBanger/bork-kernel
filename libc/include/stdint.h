#ifndef _STDINT_H
#define _STDINT_H

/*
 * i386 ILP32, the same types GCC's own stdint.h picks, so code built with
 * GCC and with TinyCC (which ships no stdint.h) agrees on the ABI.
 */

typedef signed char         int8_t;
typedef short               int16_t;
typedef int                 int32_t;
typedef long long           int64_t;
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

typedef int8_t              int_least8_t;
typedef int16_t             int_least16_t;
typedef int32_t             int_least32_t;
typedef int64_t             int_least64_t;
typedef uint8_t             uint_least8_t;
typedef uint16_t            uint_least16_t;
typedef uint32_t            uint_least32_t;
typedef uint64_t            uint_least64_t;

typedef signed char         int_fast8_t;
typedef int                 int_fast16_t;
typedef int                 int_fast32_t;
typedef long long           int_fast64_t;
typedef unsigned char       uint_fast8_t;
typedef unsigned int        uint_fast16_t;
typedef unsigned int        uint_fast32_t;
typedef unsigned long long  uint_fast64_t;

typedef int                 intptr_t;
typedef unsigned int        uintptr_t;
typedef long long           intmax_t;
typedef unsigned long long  uintmax_t;

#define INT8_MIN            (-128)
#define INT16_MIN           (-32767 - 1)
#define INT32_MIN           (-2147483647 - 1)
#define INT64_MIN           (-9223372036854775807LL - 1)
#define INT8_MAX            127
#define INT16_MAX           32767
#define INT32_MAX           2147483647
#define INT64_MAX           9223372036854775807LL
#define UINT8_MAX           255
#define UINT16_MAX          65535
#define UINT32_MAX          4294967295U
#define UINT64_MAX          18446744073709551615ULL

#define INT_LEAST8_MIN      INT8_MIN
#define INT_LEAST16_MIN     INT16_MIN
#define INT_LEAST32_MIN     INT32_MIN
#define INT_LEAST64_MIN     INT64_MIN
#define INT_LEAST8_MAX      INT8_MAX
#define INT_LEAST16_MAX     INT16_MAX
#define INT_LEAST32_MAX     INT32_MAX
#define INT_LEAST64_MAX     INT64_MAX
#define UINT_LEAST8_MAX     UINT8_MAX
#define UINT_LEAST16_MAX    UINT16_MAX
#define UINT_LEAST32_MAX    UINT32_MAX
#define UINT_LEAST64_MAX    UINT64_MAX

#define INT_FAST8_MIN       INT8_MIN
#define INT_FAST16_MIN      INT32_MIN
#define INT_FAST32_MIN      INT32_MIN
#define INT_FAST64_MIN      INT64_MIN
#define INT_FAST8_MAX       INT8_MAX
#define INT_FAST16_MAX      INT32_MAX
#define INT_FAST32_MAX      INT32_MAX
#define INT_FAST64_MAX      INT64_MAX
#define UINT_FAST8_MAX      UINT8_MAX
#define UINT_FAST16_MAX     UINT32_MAX
#define UINT_FAST32_MAX     UINT32_MAX
#define UINT_FAST64_MAX     UINT64_MAX

#define INTPTR_MIN          INT32_MIN
#define INTPTR_MAX          INT32_MAX
#define UINTPTR_MAX         UINT32_MAX
#define INTMAX_MIN          INT64_MIN
#define INTMAX_MAX          INT64_MAX
#define UINTMAX_MAX         UINT64_MAX

#define PTRDIFF_MIN         INT32_MIN
#define PTRDIFF_MAX         INT32_MAX
#define SIZE_MAX            UINT32_MAX
#define SIG_ATOMIC_MIN      INT32_MIN
#define SIG_ATOMIC_MAX      INT32_MAX

#ifdef __WCHAR_MAX__
#define WCHAR_MAX           __WCHAR_MAX__
#define WCHAR_MIN           __WCHAR_MIN__
#else
#define WCHAR_MAX           2147483647
#define WCHAR_MIN           (-2147483647 - 1)
#endif
#define WINT_MAX            4294967295U
#define WINT_MIN            0U

#define INT8_C(c)           c
#define INT16_C(c)          c
#define INT32_C(c)          c
#define INT64_C(c)          c ## LL
#define UINT8_C(c)          c
#define UINT16_C(c)         c
#define UINT32_C(c)         c ## U
#define UINT64_C(c)         c ## ULL
#define INTMAX_C(c)         c ## LL
#define UINTMAX_C(c)        c ## ULL

#endif

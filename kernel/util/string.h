#ifndef KERNEL_UTIL_STRING_H
#define KERNEL_UTIL_STRING_H

#include <stdint.h>
#include <stddef.h>

// Strings

int snprintf(char *str, size_t size, const char *format, ...);

size_t strlen(const char* str);
size_t strnlen(const char *str, size_t maxlen);

char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);

char* strcat(char* dest, const char* src);

// Standard C strncat: appends at most n chars of src, always null-terminates.
// dest must have room for strlen(dest) + n + 1 bytes.
char* strncat(char* dest, const char* src, size_t n);

// BSD strlcat: size is the TOTAL size of dest. Returns the length it tried to
// create (strlen(dest) + strlen(src)); truncation occurred if result >= size.
size_t strlcat(char* dest, const char* src, size_t size);

int strcmp(const char* str1, const char* str2);
int strncmp(const char *s1, const char *s2, size_t n);

// Not reentrant (uses static state). Prefer cstr_tok_* from tok.h.
char* strtok(char* str, const char* delim);

char* strchr(const char* str, int character);
char* strnchr(const char* str, size_t n, int character);
char* strrchr(const char* str, int character);

char* strstr(const char* haystack, const char* needle);

size_t strspn(const char *str, const char *accept);
size_t strcspn(const char *str, const char *reject);

// Memory

void* memset(void* str, int value, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);

// Translation
// dest buffers: itos >= 12 bytes, utos >= 11, u64tos >= 21, itos_commas >= 14.

void itos(int32_t value, char* dest);
void utos(uint32_t value, char* dest);
void u64tos(uint64_t value, char* dest);
int32_t stoi(const char* str);
uint32_t stou(const char* str);
void itos_commas(uint32_t val, char* dest);
uint32_t stoi_commas(const char* str);

// Hex (uppercase, no prefix, no padding)
// dest buffers: u8tox >= 3 bytes, u16tox >= 5, u32tox >= 9.

void u8tox(uint8_t value, char* dest);
void u16tox(uint16_t value, char* dest);
void u32tox(uint32_t value, char* dest);

// Fortified (_chk) variants
//
// These are the out-of-line checkers GCC/Clang emit calls to when a
// __builtin___*_chk can't be resolved at compile time (or when code built with
// _FORTIFY_SOURCE against glibc-style headers is linked into the kernel).
// `destlen` is the compiler's view of the destination object size;
// (size_t)-1 means "unknown" and disables the check. On overflow they call
// __chk_fail(), which never returns.
//
// __chk_fail() has a weak default in string.c that traps. Override it with a
// strong definition that panics with a useful message.

__attribute__((noreturn)) void __chk_fail(void);

void* __memcpy_chk(void* dest, const void* src, size_t n, size_t destlen);
void* __memmove_chk(void* dest, const void* src, size_t n, size_t destlen);
void* __memset_chk(void* dest, int value, size_t n, size_t destlen);

char* __strcpy_chk(char* dest, const char* src, size_t destlen);
char* __strncpy_chk(char* dest, const char* src, size_t n, size_t destlen);
char* __strcat_chk(char* dest, const char* src, size_t destlen);
char* __strncat_chk(char* dest, const char* src, size_t n, size_t destlen);
size_t __strlcat_chk(char* dest, const char* src, size_t size, size_t destlen);

// Optional: only if you provide vsnprintf (define KERNEL_HAVE_VSNPRINTF).
#ifdef KERNEL_HAVE_VSNPRINTF
#include <stdarg.h>
int vsnprintf(char* str, size_t size, const char* format, va_list ap);
int __vsnprintf_chk(char* str, size_t maxlen, int flag, size_t slen,
                    const char* format, va_list ap);
int __snprintf_chk(char* str, size_t maxlen, int flag, size_t slen,
                   const char* format, ...);
#endif

// Compile-time fortification
//
// Opt in with -DKERNEL_FORTIFY_SOURCE=1|2|3 (deliberately NOT keyed off
// _FORTIFY_SOURCE: Ubuntu's GCC predefines that even with -ffreestanding).
//   1: whole-object size for everything
//   2: closest-subobject size for str* functions (catches struct-member overflow)
//   3: as 2, but uses __builtin_dynamic_object_size (malloc'd / VLA sizes)
// Calls with a provably-safe size compile to the plain function; only
// unresolvable ones go through the _chk version. string.c defines
// KERNEL_STRING_IMPL so the definitions themselves aren't wrapped.
// Use (memcpy)(...) to bypass the wrapper at a single call site.

#if defined(KERNEL_FORTIFY_SOURCE) && KERNEL_FORTIFY_SOURCE > 0 && \
    !defined(KERNEL_STRING_IMPL) && defined(__GNUC__)

#if KERNEL_FORTIFY_SOURCE >= 3 && defined(__has_builtin)
#  if __has_builtin(__builtin_dynamic_object_size)
#    define __kfortify_bos(p, t) __builtin_dynamic_object_size((p), (t))
#  endif
#endif
#ifndef __kfortify_bos
#  define __kfortify_bos(p, t) __builtin_object_size((p), (t))
#endif

#define __kfortify_bos0(p) __kfortify_bos((p), 0)
#if KERNEL_FORTIFY_SOURCE >= 2
#  define __kfortify_bos1(p) __kfortify_bos((p), 1)
#else
#  define __kfortify_bos1(p) __kfortify_bos((p), 0)
#endif

#define memcpy(d, s, n)   __builtin___memcpy_chk((d), (s), (n), __kfortify_bos0(d))
#define memmove(d, s, n)  __builtin___memmove_chk((d), (s), (n), __kfortify_bos0(d))
#define memset(d, v, n)   __builtin___memset_chk((d), (v), (n), __kfortify_bos0(d))

#define strcpy(d, s)      __builtin___strcpy_chk((d), (s), __kfortify_bos1(d))
#define strncpy(d, s, n)  __builtin___strncpy_chk((d), (s), (n), __kfortify_bos1(d))
#define strcat(d, s)      __builtin___strcat_chk((d), (s), __kfortify_bos1(d))
#define strncat(d, s, n)  __builtin___strncat_chk((d), (s), (n), __kfortify_bos1(d))

// GCC has no __builtin___strlcat_chk, so route through the checker directly
// when the destination size is known.
static inline __attribute__((always_inline))
size_t __kfortify_strlcat(char* dest, const char* src, size_t size, size_t bos) {
    if (bos == (size_t)-1)
        return (strlcat)(dest, src, size);
    return __strlcat_chk(dest, src, size, bos);
}
#define strlcat(d, s, n)  __kfortify_strlcat((d), (s), (n), __kfortify_bos1(d))

#ifdef KERNEL_HAVE_VSNPRINTF
#define snprintf(str, size, ...) \
    __builtin___snprintf_chk((str), (size), KERNEL_FORTIFY_SOURCE - 1, \
                             __kfortify_bos1(str), __VA_ARGS__)
#define vsnprintf(str, size, fmt, ap) \
    __builtin___vsnprintf_chk((str), (size), KERNEL_FORTIFY_SOURCE - 1, \
                              __kfortify_bos1(str), (fmt), (ap))
#endif

#endif // KERNEL_FORTIFY_SOURCE

#endif

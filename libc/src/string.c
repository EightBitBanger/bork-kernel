/*
 * Build with -fno-builtin (and on GCC -fno-tree-loop-distribute-patterns):
 * otherwise the byte loops below may be turned into calls to themselves.
 */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

//
// Memory
//

void* memcpy(void* restrict dest, const void* restrict src, size_t n) {
    void* ret = dest;
    size_t words = n >> 2;
    __asm__ volatile("cld\n\t"
                     "rep movsl\n\t"
                     "movl %3, %%ecx\n\t"
                     "rep movsb"
                     : "+D"(dest), "+S"(src), "+c"(words)
                     : "r"(n & 3)
                     : "memory");
    return ret;
}

void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;

    if (d == s || n == 0)
        return dest;
    if (d < s || d >= s + n)
        return memcpy(dest, src, n);    // Forward copy is safe

    // Overlap with dest above src: copy backwards
    d += n - 1;
    s += n - 1;
    __asm__ volatile("std\n\t"
                     "rep movsb\n\t"
                     "cld"
                     : "+D"(d), "+S"(s), "+c"(n)
                     :
                     : "memory");
    return dest;
}

void* memset(void* dest, int c, size_t n) {
    void* ret = dest;
    uint32_t pattern = (uint8_t)c * 0x01010101U;
    size_t words = n >> 2;
    __asm__ volatile("cld\n\t"
                     "rep stosl\n\t"
                     "movl %3, %%ecx\n\t"
                     "rep stosb"
                     : "+D"(dest), "+c"(words)
                     : "a"(pattern), "r"(n & 3)
                     : "memory");
    return ret;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    for (; n > 0; n--, p++, q++) {
        if (*p != *q)
            return *p - *q;
    }
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    for (; n > 0; n--, p++) {
        if (*p == (unsigned char)c)
            return (void*)p;
    }
    return NULL;
}

void* memrchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s + n;
    while (n-- > 0) {
        if (*--p == (unsigned char)c)
            return (void*)p;
    }
    return NULL;
}

//
// Length and copying
//

size_t strlen(const char* s) {
    const char* p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char* s, size_t maxlen) {
    size_t i = 0;
    while (i < maxlen && s[i]) i++;
    return i;
}

char* strcpy(char* restrict dest, const char* restrict src) {
    char* d = dest;
    while ((*d++ = *src++) != '\0') { }
    return dest;
}

char* stpcpy(char* restrict dest, const char* restrict src) {
    while ((*dest = *src++) != '\0') dest++;
    return dest;
}

char* strncpy(char* restrict dest, const char* restrict src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

char* strcat(char* restrict dest, const char* restrict src) {
    strcpy(dest + strlen(dest), src);
    return dest;
}

char* strncat(char* restrict dest, const char* restrict src, size_t n) {
    char* d = dest + strlen(dest);
    while (n-- > 0 && *src) *d++ = *src++;
    *d = '\0';
    return dest;
}

size_t strlcpy(char* restrict dest, const char* restrict src, size_t size) {
    size_t len = strlen(src);
    if (size > 0) {
        size_t n = (len < size - 1) ? len : size - 1;
        memcpy(dest, src, n);
        dest[n] = '\0';
    }
    return len;
}

size_t strlcat(char* restrict dest, const char* restrict src, size_t size) {
    size_t dlen = strnlen(dest, size);
    if (dlen == size)
        return size + strlen(src);
    return dlen + strlcpy(dest + dlen, src, size - dlen);
}

char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char* strndup(const char* s, size_t n) {
    size_t len = strnlen(s, n);
    char* p = (char*)malloc(len + 1);
    if (p) {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

//
// Comparison
//

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (; n > 0; n--, a++, b++) {
        if (*a != *b || *a == '\0')
            return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}

int strcoll(const char* a, const char* b) {
    return strcmp(a, b);    // "C" locale only
}

int strcasecmp(const char* a, const char* b) {
    int ca, cb;
    do {
        ca = tolower((unsigned char)*a++);
        cb = tolower((unsigned char)*b++);
    } while (ca && ca == cb);
    return ca - cb;
}

int strncasecmp(const char* a, const char* b, size_t n) {
    for (; n > 0; n--) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb || ca == 0)
            return ca - cb;
    }
    return 0;
}

//
// Searching
//

char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char*)s;
        if (*s == '\0')    return NULL;
    }
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (*s == '\0')    return (char*)last;
    }
}

char* strstr(const char* haystack, const char* needle) {
    size_t n = strlen(needle);
    if (n == 0)
        return (char*)haystack;
    for (; (haystack = strchr(haystack, *needle)) != NULL; haystack++) {
        if (strncmp(haystack, needle, n) == 0)
            return (char*)haystack;
    }
    return NULL;
}

size_t strspn(const char* s, const char* accept) {
    size_t i = 0;
    while (s[i] && strchr(accept, s[i])) i++;
    return i;
}

size_t strcspn(const char* s, const char* reject) {
    size_t i = 0;
    while (s[i] && !strchr(reject, s[i])) i++;
    return i;
}

char* strpbrk(const char* s, const char* accept) {
    s += strcspn(s, accept);
    return *s ? (char*)s : NULL;
}

char* strtok_r(char* restrict s, const char* restrict delim, char** restrict save) {
    if (s == NULL)
        s = *save;
    if (s == NULL)
        return NULL;

    s += strspn(s, delim);
    if (*s == '\0') {
        *save = NULL;
        return NULL;
    }

    char* end = s + strcspn(s, delim);
    if (*end) {
        *end = '\0';
        *save = end + 1;
    } else {
        *save = NULL;
    }
    return s;
}

char* strtok(char* restrict s, const char* restrict delim) {
    static char* save;
    return strtok_r(s, delim, &save);
}


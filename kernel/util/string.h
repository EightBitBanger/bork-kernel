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
// (This is what the old size_t-returning strncat actually implemented.)
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
// dest buffers: itos >= 12 bytes, utos >= 11, itos_commas >= 14.

void itos(int32_t value, char* dest);
void utos(uint32_t value, char* dest);
int32_t stoi(const char* str);
uint32_t stou(const char* str);
void itos_commas(uint32_t val, char* dest);
uint32_t stoi_commas(const char* str);

// Hex (uppercase, no prefix, no padding)
// dest buffers: u8tox >= 3 bytes, u16tox >= 5, u32tox >= 9.

void u8tox(uint8_t value, char* dest);
void u16tox(uint16_t value, char* dest);
void u32tox(uint32_t value, char* dest);

#endif

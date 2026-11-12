#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

void*  memcpy(void* restrict dest, const void* restrict src, size_t n);
void*  memmove(void* dest, const void* src, size_t n);
void*  memset(void* dest, int c, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
void*  memchr(const void* s, int c, size_t n);
void*  memrchr(const void* s, int c, size_t n);

size_t strlen(const char* s);
size_t strnlen(const char* s, size_t maxlen);
char*  strcpy(char* restrict dest, const char* restrict src);
char*  strncpy(char* restrict dest, const char* restrict src, size_t n);
char*  stpcpy(char* restrict dest, const char* restrict src);
char*  strcat(char* restrict dest, const char* restrict src);
char*  strncat(char* restrict dest, const char* restrict src, size_t n);
size_t strlcpy(char* restrict dest, const char* restrict src, size_t size);
size_t strlcat(char* restrict dest, const char* restrict src, size_t size);

int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
int    strcoll(const char* a, const char* b);
int    strcasecmp(const char* a, const char* b);
int    strncasecmp(const char* a, const char* b, size_t n);

char*  strchr(const char* s, int c);
char*  strrchr(const char* s, int c);
char*  strstr(const char* haystack, const char* needle);
char*  strpbrk(const char* s, const char* accept);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char*  strtok(char* restrict s, const char* restrict delim);
char*  strtok_r(char* restrict s, const char* restrict delim, char** restrict save);

char*  strdup(const char* s);
char*  strndup(const char* s, size_t n);
char*  strerror(int errnum);

#endif


#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS    0
#define EXIT_FAILURE    1
#define RAND_MAX        0x7FFFFFFF
#define MB_CUR_MAX      1

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

/* Memory (heap grows through SYS_SBRK, see src/malloc.c) */
void*  malloc(size_t size);
void*  calloc(size_t count, size_t size);
void*  realloc(void* ptr, size_t size);
void*  reallocarray(void* ptr, size_t count, size_t size);
void   free(void* ptr);
void*  aligned_alloc(size_t alignment, size_t size);
int    posix_memalign(void** out, size_t alignment, size_t size);
void*  memalign(size_t alignment, size_t size);
size_t malloc_usable_size(void* ptr);

/* Process */
void   exit(int status) __attribute__((noreturn));
void   _Exit(int status) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));
int    atexit(void (*fn)(void));
char*  getenv(const char* name);

/* Absolute path with "." and ".." folded (there are no links), for a path
   that exists. resolved NULL: malloc'd, free() it. Else PATH_MAX bytes. */
char*  realpath(const char* restrict path, char* restrict resolved);

/* Conversion */
int                atoi(const char* s);
long               atol(const char* s);
long long          atoll(const char* s);
double             atof(const char* s);
long               strtol(const char* restrict s, char** restrict end, int base);
unsigned long      strtoul(const char* restrict s, char** restrict end, int base);
long long          strtoll(const char* restrict s, char** restrict end, int base);
unsigned long long strtoull(const char* restrict s, char** restrict end, int base);
double             strtod(const char* restrict s, char** restrict end);
float              strtof(const char* restrict s, char** restrict end);
long double        strtold(const char* restrict s, char** restrict end);

/* Arithmetic */
int       abs(int x);
long      labs(long x);
long long llabs(long long x);
div_t     div(int num, int den);
ldiv_t    ldiv(long num, long den);
lldiv_t   lldiv(long long num, long long den);

/* Random numbers (not cryptographic) */
int  rand(void);
void srand(unsigned int seed);

/* Searching and sorting */
void  qsort(void* base, size_t count, size_t size, int (*cmp)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t count, size_t size,
              int (*cmp)(const void*, const void*));

#endif


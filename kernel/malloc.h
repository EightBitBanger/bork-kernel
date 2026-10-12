#ifndef _HEAP_MALLOC_H_
#define _HEAP_MALLOC_H_

#include <stdint.h>
#include <stddef.h>

void* malloc(size_t size);

void free(void* ptr);

void* realloc(void* ptr, size_t size);

#endif

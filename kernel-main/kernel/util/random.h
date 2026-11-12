#ifndef KERNEL_UTIL_RANDOM_H
#define KERNEL_UTIL_RANDOM_H

#include <stdint.h>
#include <stddef.h>

// rand() returns values in [0, RAND_MAX]. Not cryptographically secure.
#define RAND_MAX 32767

void rand_init(void);

int rand(void);

void rand_seed(unsigned int seed);

#endif

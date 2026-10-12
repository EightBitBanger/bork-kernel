#ifndef _RANDOM_NUMBER_GENERATOR_H_
#define _RANDOM_NUMBER_GENERATOR_H_

#include <stdbool.h>
#include <stdint.h>

bool rng_get_rand(uint32_t* out_val);
bool rng_get_seed(uint32_t *out_val);

#endif

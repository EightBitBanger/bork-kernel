#ifndef _RANDOM_NUMBER_GENERATOR_H_
#define _RANDOM_NUMBER_GENERATOR_H_

#include <stdbool.h>
#include <stdint.h>

// RDRAND / RDSEED. False when the CPU has no such instruction (checked
// with CPUID once: running it anyway would fault with #UD) or it gave up.
bool rng_get_rand(uint32_t* out_val);
bool rng_get_seed(uint32_t *out_val);

// The time stamp counter, 0 on a CPU without one (386, 486). Only a
// source of timing jitter for mixing, never a random number by itself.
uint64_t rng_cycles(void);

#endif

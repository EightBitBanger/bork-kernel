#include <stdint.h>

#include <kernel/util/random.h>

/*
 * Classic ANSI C LCG. The high 15 bits of the state are returned, since the
 * low bits of an LCG with a power-of-two modulus have very short periods.
 *
 * TODO: seed from a persistent file / hardware RNG (in the cryptography
 * module, not here).
 */

static uint32_t rand_state = 123456789;

void rand_seed(unsigned int seed) {
    rand_state = (uint32_t)seed;
}

int rand(void) {
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state >> 16) & RAND_MAX);
}

void rand_init(void) {
    rand_seed(42);
}

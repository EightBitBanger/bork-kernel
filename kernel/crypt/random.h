#ifndef KERNEL_CRYPT_RANDOM_H
#define KERNEL_CRYPT_RANDOM_H

#include <stddef.h>
#include <stdint.h>

//
// Random bytes for nonces and keys. A SHA-1 pool that callers stir with
// whatever is unpredictable to them: the CPU's RDRAND / RDSEED and cycle
// counter, the time packets arrive, radio noise. Output is SHA-1 of the pool
// and a counter, and every read stirs the pool forward, so earlier output
// can't be worked out from a later state.
//
// Locked (a mutex): the Wi-Fi station, the net thread (frame arrival
// times) and SYS_RANDOM (programs: TLS keys) all use it. Threads only, never
// from an interrupt.
//

void crypt_random_add(const void* data, size_t n);
void crypt_random_bytes(void* out, size_t n);

#endif

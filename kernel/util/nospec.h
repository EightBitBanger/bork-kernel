#ifndef _KERNEL_UTIL_NOSPEC_H_
#define _KERNEL_UTIL_NOSPEC_H_

#include <stdint.h>

//
// Spectre v1 (bounds check bypass) helpers
//
// After `if (index < size)` the CPU may predict the branch wrong and run
// the code behind it with an out-of-range index, reading (and leaving traces
// in the cache of) memory the check was meant to keep out. Indexing with
// array_index_nospec(index, size) instead gives that speculative path index
// 0: the clamp is computed with CMP/SBB, a data dependency the branch
// predictor cannot skip. Use it after the real check, on indexes that come
// from user space (syscall numbers, descriptors, handles).
//

// 0xFFFFFFFF when index < size, else 0. No branch.
static inline uint32_t array_index_mask_nospec(uint32_t index, uint32_t size) {
    uint32_t mask;
    __asm__ ("cmp %1, %2; sbb %0, %0" : "=r"(mask) : "g"(size), "r"(index) : "cc");
    return mask;
}

// index when index < size, else 0
static inline uint32_t array_index_nospec(uint32_t index, uint32_t size) {
    return index & array_index_mask_nospec(index, size);
}

#endif

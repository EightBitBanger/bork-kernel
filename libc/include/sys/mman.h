#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <stddef.h>

#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

/*
 * The kernel uses 32-bit paging without NX, so every mapped user page is
 * already readable and executable, and the heap is writable. mprotect()
 * checks its arguments and succeeds without changing anything; it does not
 * make memory read-only. Enough for JIT-style code (tcc -run) that writes
 * code to the heap and then asks for PROT_EXEC.
 */
int mprotect(void* addr, size_t length, int prot);

#endif

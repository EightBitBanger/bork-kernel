/*
 * sys/mman.h. See the header: protections are not enforced by the kernel's
 * page tables (no NX, user pages are mapped writable), so this only checks
 * that the range is user memory and the bits are known.
 */

#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>

#define USER_START  0x40000000u     // VM_USER_START
#define USER_END    0xC0000000u     // VM_USER_END

int mprotect(void* addr, size_t length, int prot) {
    uintptr_t a = (uintptr_t)addr;
    if ((a & 4095u) != 0 || (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (a < USER_START || a > USER_END || length > USER_END - a) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

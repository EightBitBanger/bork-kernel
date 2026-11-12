#ifndef _KDRV_MODULE_H_
#define _KDRV_MODULE_H_

// For driver source only (drivers/<name>/*.c), never for the kernel.
//
// A driver file's whole contact with the kernel: the types in driver.h,
// device.h and chardev.h, and the function table in kapi.h, handed to its
// entry point. Each driver defines exactly one
//
//   KDRV_ENTRY(api) {
//       kapi = api;        // keep it for later calls
//       ...
//       return &my_driver;
//   }
//
// and is built by build-drivers.sh into bin/drivers/<name>.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>
#include <kernel/driver/chardev.h>
#include <kernel/driver/kapi.h>
#include <kernel/syscall/syscall.h>     // SYS_E* results, SYS_SEC_* bits

// The entry point the loader calls; placed first in the image
#define KDRV_ENTRY(api) \
    __attribute__((section(".text.kdrv_entry"), used)) \
    const struct Driver* kdrv_entry(const struct KernelApi* api)

const struct Driver* kdrv_entry(const struct KernelApi* api);

// The runtime every driver links (kdrv_runtime.c)
void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);
void* memset(void* dest, int value, size_t n);
int   memcmp(const void* a, const void* b, size_t n);

#endif

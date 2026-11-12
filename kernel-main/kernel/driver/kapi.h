#ifndef _KERNEL_KAPI_H_
#define _KERNEL_KAPI_H_

// The kernel as a loaded driver sees it
//
// A driver file (driver/kdrv.h) links against nothing in the kernel: its
// entry point is handed this table and calls the kernel only through it.
// So the kernel can be rebuilt, stripped and rearranged without breaking
// drivers already built, as long as this table and the structures it passes
// (driver.h, device.h, chardev.h) keep their layout.
//
// Rules for the table:
//   - New entries only ever go at the end, and KAPI_VERSION goes up by one.
//     `size` tells a module how much of the table this kernel has.
//   - An entry is never removed or changed; a new one is added instead.
//   - A module states the lowest version it needs (KdrvHeader.kapi_version)
//     and the loader refuses it on an older kernel.
//
// What a module may and may not do with it:
//   - Class devices (chardev_register) belong in probe and come out again in
//     remove: an unloaded module's code is freed, so nothing may point into
//     it afterwards.
//   - kdrv_entry runs once, on the loading thread, with no lock held. It
//     may add the platform device its driver is for (device_add_platform)
//     and set up its own state. It must not register class devices.
//   - Every call has the rules of the kernel function it stands for (see
//     the header named next to it).

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>
#include <kernel/driver/chardev.h>

#define KAPI_VERSION        2

struct KernelApi {
    uint32_t version;               // KAPI_VERSION of this kernel
    uint32_t size;                  // sizeof(struct KernelApi) of this kernel

    // Kernel heap (malloc.h)
    void*    (*malloc)(size_t size);
    void     (*free)(void* ptr);

    // One line to COM1, prefixed with the module's name. printf formats as
    // util/snprint.h takes them; a line longer than 160 bytes is cut.
    void     (*log)(const char* format, ...);
    int      (*vsnprintf)(char* out, size_t size, const char* format, va_list args);

    // Time (util/timer.h, scheduler/scheduler.h). sleep: thread context only.
    uint64_t (*time_ms)(void);
    void     (*sleep_ms)(uint32_t ms);

    // Devices (driver/device.h)
    struct Device* (*device_add_platform)(const char* name);
    struct Device* (*device_find)(const char* name);
    void           (*device_remove)(struct Device* dev);

    // Character devices (driver/chardev.h)
    struct CharDevice* (*chardev_register)(const char* name, const struct CharDeviceOps* ops,
                                           void* driver_data, uint32_t security);
    void     (*chardev_unregister)(struct CharDevice* cd);
    bool     (*chardev_copy_from_user)(void* dst, uint32_t user_address, uint32_t length);
    bool     (*chardev_copy_to_user)(uint32_t user_address, const void* src, uint32_t length);

    // --- KAPI_VERSION 2 ---

    // Call a library driver's exported function (driver/drvcall.h): fill
    // call->argc / args (with their types) and out / out_size. Returns a
    // DrvCallResult (0: it ran, *result is what it returned). Not from
    // kdrv_entry, probe or remove.
    int32_t  (*driver_call)(const char* driver, const char* function, struct DriverCall* call,
                            int32_t* result);
};

// The one table, filled in by the loader (kdrv_loader.c)
const struct KernelApi* kapi_get(void);

#endif

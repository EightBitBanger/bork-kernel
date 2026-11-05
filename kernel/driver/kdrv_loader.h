#ifndef _KERNEL_KDRV_LOADER_H_
#define _KERNEL_KDRV_LOADER_H_

// Driver files: load one from disk, start it, unload it again
//
// The file format is driver/kdrv.h, what a module may call driver/kapi.h.
// A loaded driver is registered like a built-in one (driver_register), so
// it is offered every device nothing holds yet and is listed in /drv with
// origin "loaded".
//
// Loading runs ring 0 code from a file: whoever asks must be trusted with
// the whole machine. The syscalls (sys_driver.h) require SYS_SEC_ADMIN; the
// kernel console, a kernel thread, may load anything it can read. The file
// is read with the caller's clearance, once, into kernel memory, and only
// that copy is checked and run.
//
// One load or unload runs at a time, system-wide. Thread context only.

#include <stdint.h>
#include <stdbool.h>

#include <kernel/driver/driver.h>
#include <kernel/driver/kdrv.h>

#define KDRV_LOADED_MAX     8       // Loaded driver files at once

typedef enum {
    KDRV_OK = 0,
    KDRV_ERR_NOT_FOUND,     // No such file, or no loaded driver of that name
    KDRV_ERR_ACCESS,        // The file can't be read with the caller's clearance
    KDRV_ERR_IO,            // Reading the file failed part way
    KDRV_ERR_NOT_DRIVER,    // Does not start with "kdrv"
    KDRV_ERR_BAD_FORMAT,    // Starts with "kdrv" but the header or tables don't add up
    KDRV_ERR_CHECKSUM,      // Damaged: the CRC-32 is wrong
    KDRV_ERR_TOO_BIG,       // Over KDRV_FILE_MAX / KDRV_IMAGE_MAX
    KDRV_ERR_MISMATCH,      // Another CPU, DRIVER_ABI or a newer KAPI_VERSION
    KDRV_ERR_NO_MEMORY,
    KDRV_ERR_REFUSED,       // kdrv_entry returned NULL, or a driver that is not sane
    KDRV_ERR_EXISTS,        // A driver of that name is registered already
    KDRV_ERR_FULL,          // KDRV_LOADED_MAX loaded, or the driver table is full
    KDRV_ERR_BUILTIN,       // Unload: a built-in driver
    KDRV_ERR_BUSY,          // Unload: a call into the library is running
} KdrvResult;

// What a load produced
typedef struct {
    char     name[DRIVER_NAME_MAX];
    char     version[16];
    uint32_t image_size;    // Bytes of kernel memory it occupies (image + bss)
    uint32_t devices;       // Devices it took when it was registered
} KdrvInfo;

// Load the driver file at `path` (absolute), start it and register its
// driver. `info` (may be NULL) is filled in on KDRV_OK.
KdrvResult kdrv_load(const char* path, KdrvInfo* info);

// Unregister a loaded driver (remove() runs on each device it holds) and
// free its code. Built-in drivers can't be unloaded.
KdrvResult kdrv_unload(const char* name);

// Whether `drv` came from a file; the file's path into `path` (may be NULL).
// Takes the loader's lock: not from inside a driver's probe / remove / poll.
bool kdrv_origin(const struct Driver* drv, char* path, uint32_t size);

// A registered driver by name (built-in or loaded), kept loaded until
// kdrv_release: kdrv_unload says KDRV_ERR_BUSY meanwhile. NULL if there is
// no such driver. For calls into a library (drvcall.c). Takes the loader's
// lock briefly: not from inside a kdrv_entry, probe or remove.
const struct Driver* kdrv_hold(const char* name);
void kdrv_release(const struct Driver* drv);

// At boot, before the first load (driver_init)
void kdrv_loader_init(void);

// Short, static text for a result ("not a driver file")
const char* kdrv_error_string(KdrvResult result);

// Check a whole file in memory without running anything: magic, header,
// bounds, checksum, CPU and versions. For the loader and for tests.
KdrvResult kdrv_check(const uint8_t* file, uint32_t size);

#endif

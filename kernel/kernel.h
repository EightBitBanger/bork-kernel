#ifndef _KERNEL_CORE_H_
#define _KERNEL_CORE_H_

#include <kernel/device/driver.h>
#include <kernel/device/device.h>
#include <kernel/device/knode.h>
#include <kernel/device/kevent.h>

#include <kernel/console/virtual_key.h>
#include <kernel/console/print.h>
#include <kernel/console/console.h>
#include <kernel/console/keyboard.h>
#include <kernel/console/display.h>

#include <kernel/fs/fs.h>
#include <kernel/vfs/vfs.h>

#include <kernel/knode.h>
#include <kernel/events.h>
#include <kernel/syscall.h>

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// PATH and home hold paths, so they get the same limit as any path
#define PATH_LENGTH_MAX  VFS_PATH_MAX

struct LocalPaths {
    char path[PATH_LENGTH_MAX];     // Program search directories
    char home[PATH_LENGTH_MAX];     // Device local system directory "/sys"
};

// Working directory
//
// Kept as an absolute, normalized path ("/", "/mnt/ata0p1/usr"), not as node
// addresses. A path stays meaningful across a format, a remount or a
// "boot install"; a basefs address does not, and neither does a cached
// device root. Relative paths are turned into absolute ones against it with
// vfs_path_normalize().
//
// Nothing checks that it still exists: a directory can be removed or its
// device reformatted underneath it. Whoever uses it (the console) checks
// and falls back to the nearest existing ancestor.
//
// One working directory for the kernel for now. Once processes have their
// own, this moves into the process and these become its accessors.
//
// Thread context only (guarded by the VFS lock).

// Copy the working directory into `path`. False if it does not fit `size`
// ("/" is copied instead when there is room for it).
bool kernel_get_working_directory(char* path, size_t size);

// Set the working directory. `path` must be absolute; it is stored
// normalized. Existence is the caller's business. False if `path` is not
// absolute or too long (nothing changes then).
bool kernel_set_working_directory(const char* path);

void kernel_get_local_paths(struct LocalPaths* out_paths);
void kernel_set_local_paths(const struct LocalPaths* paths);

void kernel_init(void);

uint32_t create_device(const char* name);
void destroy_device(uint32_t address);

// Hardware identification

void hardware_identify_devices(uint32_t knode_device, uint32_t knode_mount, uint32_t nt_device);
uint32_t device_get_hardware_address(const char* name);
void device_get_hardware_data(uint32_t address, char* data_buffer, uint8_t size);

#endif


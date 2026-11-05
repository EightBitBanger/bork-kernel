#ifndef _KERNEL_CHARDEV_H_
#define _KERNEL_CHARDEV_H_

// Character devices
//
// A device a program talks to as a stream of bytes: /dev/<name>. The VFS
// passes open, close, read and write on the file straight to the driver,
// and SYS_IOCTL on a descriptor of it to ioctl. There is no size and no
// position: every read and write is handed to the driver as it comes, and
// seeking does nothing.
//
// Ops run with the VFS lock held (like a filesystem's), so they must not
// block for long or call back into the VFS. A device that waits for data
// returns what it has (0: nothing yet) instead of sleeping.
//
// Handles still open when a device is unregistered are revoked: their next
// call fails. close is not called for them.

#include <stdint.h>
#include <stdbool.h>

#define CHARDEV_NAME_MAX    16      // Including the terminator; also the /dev file name

struct CharDevice;

struct CharDeviceOps {
    // Optional (NULL: always allowed). `flags` is VFS_OPEN_*. Return 0 to
    // allow the open, or a SYS_E* code (SYS_EBUSY: only one user at a time).
    int32_t (*open)(struct CharDevice* cd, uint16_t flags);

    // Optional. A handle opened with open() closed.
    void    (*close)(struct CharDevice* cd);

    // Optional (NULL: the file can't be opened for reading / writing).
    // Return the bytes moved, 0 for none (read: nothing there right now, or
    // the end), or a SYS_E* code. `buffer` is a kernel buffer.
    int32_t (*read)(struct CharDevice* cd, void* buffer, uint32_t size);
    int32_t (*write)(struct CharDevice* cd, const void* buffer, uint32_t size);

    // Optional (NULL: SYS_ENOTTY). Device-specific requests. `arg` comes
    // from the caller as it is: a number, or a user address to go through
    // chardev_copy_from_user / chardev_copy_to_user. Return >= 0 on
    // success, SYS_ENOTTY for a request the device doesn't know.
    int32_t (*ioctl)(struct CharDevice* cd, uint32_t request, uint32_t arg);
};

// Lives in the kernel heap and sits in /dev as a file; `name` comes first,
// which is how the knode tree finds it by name.
struct CharDevice {
    char     name[CHARDEV_NAME_MAX];
    uint32_t security;              // SYS_SEC_* bits a process needs to open it (0: none extra)
    const struct CharDeviceOps* ops;
    void*    driver_data;
};

// Add /dev/<name>. NULL when the name is empty or too long, /dev already
// has an entry of that name, or out of memory.
struct CharDevice* chardev_register(const char* name, const struct CharDeviceOps* ops,
                                    void* driver_data, uint32_t security);

// Take it out of /dev and free it. Open handles are revoked first.
void chardev_unregister(struct CharDevice* cd);

// True if `address` (a kernel node from the VFS) is a character device
bool chardev_check(const void* address);

// Copy between a kernel buffer and the calling process's memory, for
// ioctl arguments. False for a bad user range or when called from a kernel
// thread.
bool chardev_copy_from_user(void* dst, uint32_t user_address, uint32_t length);
bool chardev_copy_to_user(uint32_t user_address, const void* src, uint32_t length);

#endif

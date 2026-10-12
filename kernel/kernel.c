#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/kernel.h>

#include <kernel/arch/x86/heap.h>
#include <kernel/util/string.h>

// Kernel-wide state that used to live in a kmalloc'd "system object" and was
// reached through kmem_read / kmem_write (a leftover from when kernel memory
// could be external RAM). Plain statics now, guarded by the VFS lock: both
// are VFS state, and the lock is recursive, so a caller already holding it
// can use these too.

static char kernel_cwd[VFS_PATH_MAX] = "/";
static struct LocalPaths kernel_paths;

bool kernel_get_working_directory(char* path, size_t size) {
    if (path == NULL || size == 0)
        return false;
    
    vfs_lock();
    size_t length = strlen(kernel_cwd);
    bool fits = length < size;
    if (fits) {
        memcpy(path, kernel_cwd, length + 1);
    } else if (size >= 2) {
        path[0] = '/';
        path[1] = '\0';
    } else {
        path[0] = '\0';
    }
    vfs_unlock();
    return fits;
}

bool kernel_set_working_directory(const char* path) {
    if (path == NULL || path[0] != '/')
        return false;
    
    // Stored in one canonical form, so comparing and printing it is simple
    char normalized[VFS_PATH_MAX];
    if (!vfs_path_normalize("/", path, normalized, sizeof(normalized)))
        return false;
    
    vfs_lock();
    memcpy(kernel_cwd, normalized, strlen(normalized) + 1);
    vfs_unlock();
    return true;
}

void kernel_get_local_paths(struct LocalPaths* out_paths) {
    if (out_paths == NULL)
        return;
    vfs_lock();
    memcpy(out_paths, &kernel_paths, sizeof(struct LocalPaths));
    vfs_unlock();
}

void kernel_set_local_paths(const struct LocalPaths* paths) {
    if (paths == NULL)
        return;
    vfs_lock();
    memcpy(&kernel_paths, paths, sizeof(struct LocalPaths));
    // Always terminated, whatever the caller left in the arrays
    kernel_paths.path[PATH_LENGTH_MAX - 1] = '\0';
    kernel_paths.home[PATH_LENGTH_MAX - 1] = '\0';
    vfs_unlock();
}

uint32_t create_device(const char* name) {
    struct Device device;
    memset(&device, 0x00, sizeof(struct Device));
    strcpy(device.name, name);
    
    uint32_t device_address = kmalloc(sizeof(struct Device));
    
    kmalloc_set_type(device_address, KMALLOC_TYPE_DEVICE);
    kmalloc_set_permissions(device_address, KMALLOC_PERMISSION_READ | KMALLOC_PERMISSION_WRITE);
    
    kmem_write(device_address, &device, sizeof(struct Device));
    return device_address;
}

void destroy_device(uint32_t address) {
    kfree(address);
}

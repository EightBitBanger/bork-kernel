#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/kernel.h>

#include <kernel/arch/x86/heap.h>
#include <kernel/util/string.h>

static char kernel_cwd[VFS_PATH_MAX] = "/";
static struct LocalPaths kernel_paths;

// The root of the in-memory node tree
// /proc - running processes
// /dev  - hardware descriptions
// /mnt  - storage devices are mounted
void kernel_init(void) {
    void* root_node = create_knode("", NULL);
    knode_set_root(root_node);
    
    void* proc = create_knode("proc", root_node);
    void* dev  = create_knode("dev", root_node);
    void* mnt  = create_knode("mnt", root_node);
    
    knode_set_permissions(proc, KMALLOC_PERMISSION_READ);
    knode_set_permissions(dev,  KMALLOC_PERMISSION_READ);
    knode_set_permissions(mnt,  KMALLOC_PERMISSION_READ);
    
    struct LocalPaths fs_paths;
    memset(&fs_paths, 0, sizeof(fs_paths));
    kernel_set_local_paths(&fs_paths);
    
    kernel_set_working_directory("/");
}

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

struct Device* create_device(const char* name) {
    // Zeroed by kmalloc
    struct Device* device = kmalloc(sizeof(struct Device));
    if (device == NULL)
        return NULL;
    
    strncpy(device->name, name, sizeof(device->name) - 1);
    
    kmalloc_set_type(device, KMALLOC_TYPE_DEVICE);
    kmalloc_set_permissions(device, KMALLOC_PERMISSION_READ | KMALLOC_PERMISSION_WRITE);
    return device;
}

void destroy_device(struct Device* device) {
    kfree(device);
}

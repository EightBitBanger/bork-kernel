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
// /drv  - drivers and the devices they hold (driver/driver_tree.c)
void kernel_init(void) {
    void* root_node = create_knode("", NULL);
    knode_set_root(root_node);
    
    void* proc = create_knode("proc", root_node);
    void* dev  = create_knode("dev", root_node);
    void* mnt  = create_knode("mnt", root_node);
    void* drv  = create_knode("drv", root_node);
    
    knode_set_permissions(proc, KMALLOC_PERMISSION_READ);
    knode_set_permissions(dev,  KMALLOC_PERMISSION_READ);
    knode_set_permissions(mnt,  KMALLOC_PERMISSION_READ);
    knode_set_permissions(drv,  KMALLOC_PERMISSION_READ);
    
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

struct KProperty* create_property(const char* text) {
    // Zeroed by kmalloc
    struct KProperty* property = kmalloc(sizeof(struct KProperty));
    if (property == NULL)
        return NULL;
    
    strncpy(property->name, text, sizeof(property->name) - 1);
    
    kmalloc_set_type(property, KMALLOC_TYPE_PROPERTY);
    kmalloc_set_permissions(property, KMALLOC_PERMISSION_READ | KMALLOC_PERMISSION_WRITE);
    return property;
}

void destroy_property(struct KProperty* property) {
    kfree(property);
}

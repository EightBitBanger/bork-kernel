#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/driver/chardev.h>

#include <kernel/arch/x86/heap.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/scheduler/process.h>
#include <kernel/knode.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>

static void* chardev_dev_directory(void) {
    void* root = knode_get_root();
    return (root != NULL) ? knode_find_by_name(root, "dev") : NULL;
}

struct CharDevice* chardev_register(const char* name, const struct CharDeviceOps* ops,
                                    void* driver_data, uint32_t security) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= CHARDEV_NAME_MAX || ops == NULL)
        return NULL;

    vfs_lock();
    void* dev_directory = chardev_dev_directory();
    if (dev_directory == NULL || knode_find_by_name(dev_directory, name) != NULL) {
        vfs_unlock();
        return NULL;
    }

    // Zeroed by kmalloc
    struct CharDevice* cd = kmalloc(sizeof(struct CharDevice));
    if (cd == NULL) {
        vfs_unlock();
        return NULL;
    }
    strncpy(cd->name, name, CHARDEV_NAME_MAX - 1);
    cd->security    = security;
    cd->ops         = ops;
    cd->driver_data = driver_data;

    // The file's permissions say which way it can be opened
    uint8_t permissions = 0;
    if (ops->read != NULL)  permissions |= KMALLOC_PERMISSION_READ;
    if (ops->write != NULL) permissions |= KMALLOC_PERMISSION_WRITE;
    kmalloc_set_type(cd, KMALLOC_TYPE_CHARDEV);
    kmalloc_set_permissions(cd, permissions);

    if (!knode_add_reference(dev_directory, cd)) {
        kfree(cd);
        vfs_unlock();
        return NULL;
    }
    vfs_unlock();
    return cd;
}

void chardev_unregister(struct CharDevice* cd) {
    if (!chardev_check(cd))
        return;

    vfs_lock();
    vfs_knode_revoke_handles(cd);
    void* dev_directory = chardev_dev_directory();
    if (dev_directory != NULL)
        knode_remove_reference(dev_directory, cd);
    kfree(cd);
    vfs_unlock();
}

bool chardev_check(const void* address) {
    return kmalloc_is_valid(address) && kmalloc_get_type(address) == KMALLOC_TYPE_CHARDEV;
}

bool chardev_copy_from_user(void* dst, uint32_t user_address, uint32_t length) {
    AddressSpace* space = process_current_space();
    if (space == NULL || !vmm_user_check(space, user_address, length, false))
        return false;
    return vmm_user_copy_from(space, dst, user_address, length);
}

bool chardev_copy_to_user(uint32_t user_address, const void* src, uint32_t length) {
    AddressSpace* space = process_current_space();
    if (space == NULL || !vmm_user_check(space, user_address, length, true))
        return false;
    return vmm_user_copy_to(space, user_address, src, length);
}

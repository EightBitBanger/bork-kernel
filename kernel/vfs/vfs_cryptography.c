#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>

#include <kernel/vfs/vfs.h>
#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/tok.h>
#include <kernel/util/list.h>

//
// Implementations (caller holds the VFS lock)
//
// Certificates only exist on files stored on a mounted device whose
// filesystem keeps them (basefs); kernel nodes and directories have none.
//

static bool resolve_device_file(const char* path, struct FsMount** out_mount, uint64_t* out_address) {
    if (path == NULL || path[0] == '\0')
        return false;
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address))
        return false;

    struct FsMount* mount = vfs_device_get_context(path);
    if (mount == NULL || !vfs_node_is_file(mount, address))
        return false;

    *out_mount = mount;
    *out_address = address;
    return true;
}

static bool do_file_get_certificate(const char* path, uint32_t* certificate) {
    if (certificate == NULL)
        return false;
    struct FsMount* mount;
    uint64_t address;
    if (!resolve_device_file(path, &mount, &address))
        return false;
    FsNodeAttr attr;
    if (!mount->type->get_attr(mount, address, &attr))
        return false;
    *certificate = attr.certificate;
    return true;
}

static bool do_file_certify(const char* path, uint32_t certificate) {
    struct FsMount* mount;
    uint64_t address;
    if (!resolve_device_file(path, &mount, &address))
        return false;
    if (!vfs_fs_node_allowed(mount, address))
        return false;
    if (certificate == 0 || certificate == 0xFFFFFFFFu)
        return false;
    if (mount->type->set_certificate == NULL)
        return false;   // The filesystem cannot store one
    return mount->type->set_certificate(mount, address, certificate);
}

//
// Public API
//

bool vfs_file_get_certificate(const char* path, uint32_t* certificate) {
    vfs_lock();
    bool result = do_file_get_certificate(path, certificate);
    vfs_unlock();
    return result;
}

bool vfs_file_certify(const char* path, uint32_t certificate) {
    vfs_lock();
    bool result = do_file_certify(path, certificate);
    vfs_unlock();
    return result;
}

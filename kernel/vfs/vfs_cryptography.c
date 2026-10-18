#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>
#include <kernel/fs/fs.h>

#include <kernel/vfs/vfs.h>
#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/tok.h>
#include <kernel/util/list.h>

//
// Implementations (caller holds the VFS lock)
//
// Certificates only exist on files stored on a mounted basefs device;
// kernel nodes and directories have none.
//

static bool resolve_device_file(const char* path, struct FSDeviceContext** out_ctx, uint32_t* out_address) {
    if (path == NULL || path[0] == '\0')
        return false;
    uint32_t address = resolve_path_to_address(path);
    if (address == 0xFFFFFFFF || address == 0)
        return false;

    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    if (ctx == NULL || !fs_file_check(ctx, address))
        return false;

    *out_ctx = ctx;
    *out_address = address;
    return true;
}

static bool do_file_get_certificate(const char* path, uint32_t* certificate) {
    if (certificate == NULL)
        return false;
    struct FSDeviceContext* ctx;
    uint32_t address;
    if (!resolve_device_file(path, &ctx, &address))
        return false;
    return fs_file_get_certificate(ctx, address, certificate);
}

static bool do_file_certify(const char* path, uint32_t certificate) {
    struct FSDeviceContext* ctx;
    uint32_t address;
    if (!resolve_device_file(path, &ctx, &address))
        return false;
    if (!vfs_fs_node_allowed(ctx, address))
        return false;
    // fs_file_set_certificate rejects 0 and 0xFFFFFFFF
    return fs_file_set_certificate(ctx, address, certificate);
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


// basefs as an FsType: the table the VFS drives basefs through.
//
// Everything here is a thin translation between the generic calls in
// fs/fs_type.h and the basefs API (fs.h, file.h, directory.h). Nodes are
// basefs addresses (64-bit disk offsets), whose "nothing" FS_NULL is the
// same value as FS_NODE_NULL.

#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/fs/fs.h>
#include <kernel/fs/fs_type.h>
#include <kernel/fs/basefs/check.h>
#include <kernel/block/block.h>
#include <kernel/block/partition.h>

#include <kernel/util/string.h>

_Static_assert(FS_NULL == FS_NODE_NULL, "basefs FS_NULL must equal FS_NODE_NULL");
_Static_assert(FS_NAME_MAX == VFS_NAME_MAX, "basefs names must hold every VFS name");

// m->data: the basefs context and the size of its sector window
// (ctx.device_address, fs_device_window_size bytes)
struct BasefsMount {
    struct FSDeviceContext ctx;
    uint32_t window_size;
};

static inline struct FSDeviceContext* basefs_ctx(struct FsMount* m) {
    return &((struct BasefsMount*)m->data)->ctx;
}

static uint8_t basefs_to_fs_permissions(uint8_t vfs) {
    uint8_t fs = 0;
    if (vfs & VFS_PERMISSION_EXECUTE) fs |= FS_PERMISSION_EXECUTE;
    if (vfs & VFS_PERMISSION_READ)    fs |= FS_PERMISSION_READ;
    if (vfs & VFS_PERMISSION_WRITE)   fs |= FS_PERMISSION_WRITE;
    return fs;
}

static uint8_t basefs_to_vfs_permissions(uint8_t fs) {
    uint8_t vfs = 0;
    if (fs & FS_PERMISSION_EXECUTE) vfs |= VFS_PERMISSION_EXECUTE;
    if (fs & FS_PERMISSION_READ)    vfs |= VFS_PERMISSION_READ;
    if (fs & FS_PERMISSION_WRITE)   vfs |= VFS_PERMISSION_WRITE;
    return vfs;
}

//
// Volume
//

static bool basefs_probe(const uint8_t* sector0, uint32_t sector_size) {
    return fs_device_probe(sector0, sector_size);
}

// A window big enough for `device`, reusing the current one when it fits
static bool basefs_window_for(struct BasefsMount* bm, struct BlockDevice* device) {
    uint32_t size = fs_device_window_size(device);
    if (bm->ctx.device_address != NULL && bm->window_size == size)
        return true;

    uint8_t* window = malloc(size);
    if (window == NULL)
        return false;
    memset(window, 0, size);

    if (bm->ctx.device_address != NULL)
        free(bm->ctx.device_address);
    bm->ctx.device_address = window;
    bm->window_size = size;
    return true;
}

static bool basefs_attach(struct FsMount* m, struct BlockDevice* device) {
    struct BasefsMount* bm = (struct BasefsMount*)m->data;
    if (bm == NULL) {
        bm = malloc(sizeof(struct BasefsMount));
        if (bm == NULL)
            return false;
        memset(bm, 0, sizeof(struct BasefsMount));
        m->data = bm;
    }
    if (!basefs_window_for(bm, device))
        return false;

    struct FSPartitionBlock part;
    bm->ctx    = fs_device_open(device, bm->ctx.device_address, &part);
    m->device  = device;
    m->is_open = bm->ctx.is_open;
    return true;
}

// Cut the context loose from its disk: it reports as unformatted and every
// basefs call on it becomes a no-op (fs_io_ready fails without a device)
static void basefs_detach(struct FsMount* m) {
    struct FSDeviceContext* ctx = basefs_ctx(m);
    ctx->is_open      = false;
    ctx->device       = NULL;
    ctx->pool_size    = 0;
    ctx->block_count  = 0;
    ctx->sector_frame = FS_INVALID_FRAME;
    ctx->sector_dirty = false;
    ctx->frame_offset = FS_INVALID_FRAME;
    ctx->frame_dirty  = false;
    ctx->used_scan    = 0;
    ctx->used_blocks  = 0;
    m->device  = NULL;
    m->is_open = false;
}

static void basefs_release(struct FsMount* m) {
    struct BasefsMount* bm = (struct BasefsMount*)m->data;
    if (bm == NULL)
        return;
    if (bm->ctx.device_address != NULL)
        free(bm->ctx.device_address);
    free(bm);
    m->data = NULL;
}

static bool basefs_sync(struct FsMount* m) {
    return fs_device_sync(basefs_ctx(m));
}

static bool basefs_volume_info(struct FsMount* m, FsVolumeInfo* out) {
    struct FSPartitionBlock partition;
    if (!m->is_open || fs_device_get_partition(basefs_ctx(m), &partition) != 0)
        return false;
    out->block_size = partition.sector_size;
    out->total_size = partition.total_size;
    return true;
}

static bool basefs_used_bytes(struct FsMount* m, uint64_t* out, uint32_t* percent) {
    return fs_used_bytes_step(basefs_ctx(m), out, percent);
}

static uint64_t basefs_max_capacity(struct BlockDevice* device) {
    return fs_device_max_capacity(device);
}

static bool basefs_format_check(struct BlockDevice* device, uint64_t capacity, uint32_t block_size) {
    return block_size >= FS_MIN_SECTOR_SIZE && (block_size % 4U) == 0 &&
           capacity >= block_size && capacity <= fs_device_max_capacity(device);
}

static bool basefs_format(struct FsMount* m, struct BlockDevice* device, uint64_t capacity, uint32_t block_size) {
    struct BasefsMount* bm = (struct BasefsMount*)m->data;
    if (bm == NULL || !basefs_window_for(bm, device))
        return false;
    return fs_device_format(device, bm->ctx.device_address, capacity, block_size);
}

static VfsCheckResult basefs_check(struct FsMount* m, bool repair, VfsCheckReport* report) {
    return fs_device_check(basefs_ctx(m), repair, report);
}

//
// Nodes
//

static uint64_t basefs_root(struct FsMount* m) {
    struct FSPartitionBlock partition;
    if (!m->is_open || fs_device_get_partition(basefs_ctx(m), &partition) != 0)
        return FS_NULL;     // Bad magic, another format version, or detached
    return partition.root_directory;
}

static uint8_t basefs_node_type(struct FsMount* m, uint64_t node) {
    struct FSDeviceContext* ctx = basefs_ctx(m);
    if (node == FS_NULL || node == 0)
        return FS_NODE_NONE;
    if (fs_check_directory_valid(ctx, node))
        return FS_NODE_DIRECTORY;
    if (fs_file_check(ctx, node))
        return FS_NODE_FILE;
    return FS_NODE_NONE;
}

// Every header records its parent; the root's is FS_NULL (or, on volumes
// from older builds, itself)
static uint64_t basefs_parent(struct FsMount* m, uint64_t node) {
    uint64_t parent = fs_file_get_parent(basefs_ctx(m), node);
    return (parent == node || parent == 0) ? FS_NULL : parent;
}

static uint64_t basefs_lookup(struct FsMount* m, uint64_t dir, const char* name) {
    uint64_t found = fs_directory_find(basefs_ctx(m), dir, name);
    return (found == 0) ? FS_NULL : found;
}

static uint32_t basefs_dir_count(struct FsMount* m, uint64_t dir) {
    return fs_directory_get_reference_count(basefs_ctx(m), dir);
}

static bool basefs_dir_entry(struct FsMount* m, uint64_t dir, uint32_t index,
                             uint64_t* node, char* name, size_t name_size) {
    return fs_directory_get_entry(basefs_ctx(m), dir, index, node, name, name_size, NULL);
}

static uint64_t basefs_create(struct FsMount* m, uint64_t dir, const char* name,
                              uint8_t node_type, uint8_t permissions) {
    struct FSDeviceContext* ctx = basefs_ctx(m);
    uint8_t perm = basefs_to_fs_permissions(permissions);
    if (node_type == FS_NODE_DIRECTORY)
        return fs_directory_create(ctx, name, perm, dir);
    if (node_type == FS_NODE_FILE)
        return fs_file_create(ctx, name, perm, 0, dir);
    return FS_NULL;
}

static bool basefs_remove(struct FsMount* m, uint64_t dir, uint64_t node) {
    struct FSDeviceContext* ctx = basefs_ctx(m);

    if (fs_file_check(ctx, node)) {
        // Unlink first: a crash then leaks blocks instead of leaving an
        // entry that points at freed storage
        if (fs_directory_remove_entry(ctx, dir, node) != FS_DIR_OK)
            return false;
        return fs_file_delete(ctx, node);
    }

    // Directories go with everything inside them; basefs unlinks the
    // directory from its parent before freeing anything
    if (fs_check_directory_valid(ctx, node))
        return fs_directory_delete_recursive(ctx, node, dir);

    return false;
}

static bool basefs_rename(struct FsMount* m, uint64_t dir, uint64_t node, const char* name) {
    return fs_directory_rename_entry(basefs_ctx(m), dir, node, name) == FS_DIR_OK;
}

static bool basefs_resize(struct FsMount* m, uint64_t node, uint64_t size) {
    return fs_file_resize(basefs_ctx(m), node, size);
}

static bool basefs_get_attr(struct FsMount* m, uint64_t node, FsNodeAttr* out) {
    struct FSDeviceContext* ctx = basefs_ctx(m);
    memset(out, 0, sizeof(FsNodeAttr));

    uint8_t type = basefs_node_type(m, node);
    if (type == FS_NODE_NONE)
        return false;

    uint8_t perm = 0;
    fs_file_get_permissions(ctx, node, &perm);
    out->permissions = basefs_to_vfs_permissions(perm);

    if (type == FS_NODE_FILE)
        out->size = fs_file_get_size(ctx, node);

    fs_file_get_security(ctx, node, &out->security);
    fs_file_get_certificate(ctx, node, &out->certificate);
    fs_file_get_times(ctx, node, &out->created, &out->modified);
    return true;
}

static bool basefs_set_permissions(struct FsMount* m, uint64_t node, uint8_t permissions) {
    return fs_file_set_permissions(basefs_ctx(m), node, basefs_to_fs_permissions(permissions));
}

static bool basefs_set_security(struct FsMount* m, uint64_t node, uint32_t security) {
    return fs_file_set_security_flag(basefs_ctx(m), node, security);
}

// fs_file_set_certificate rejects 0 and 0xFFFFFFFF
static bool basefs_set_certificate(struct FsMount* m, uint64_t node, uint32_t certificate) {
    return fs_file_set_certificate(basefs_ctx(m), node, certificate);
}

//
// Open files
//

static void* basefs_open(struct FsMount* m, uint64_t node, uint16_t mode) {
    uint8_t fs_mode = 0;
    if (mode & VFS_OPEN_READ)   fs_mode |= FS_FILE_MODE_READ;
    if (mode & VFS_OPEN_WRITE)  fs_mode |= FS_FILE_MODE_WRITE;
    if (mode & VFS_OPEN_APPEND) fs_mode |= FS_FILE_MODE_APPEND;

    FileHandle* handle = malloc(sizeof(FileHandle));
    if (handle == NULL)
        return NULL;
    if (!fs_file_open(handle, basefs_ctx(m), node, fs_mode)) {
        free(handle);
        return NULL;
    }
    return handle;
}

static void basefs_close(void* file) {
    fs_file_close((FileHandle*)file);
    free(file);
}

static uint32_t basefs_read(void* file, void* buffer, uint32_t size) {
    return fs_file_read((FileHandle*)file, buffer, size);
}

static uint32_t basefs_write(void* file, const void* buffer, uint32_t size) {
    return fs_file_write((FileHandle*)file, buffer, size);
}

static uint64_t basefs_seek(void* file, uint64_t position) {
    return fs_file_seek((FileHandle*)file, position);
}

static uint64_t basefs_tell(void* file) {
    return fs_file_tell((const FileHandle*)file);
}

const struct FsType fs_type_basefs = {
    .name            = "basefs",
    .mbr_types       = { MBR_TYPE_BASEFS },
    .probe           = basefs_probe,

    .attach          = basefs_attach,
    .detach          = basefs_detach,
    .release         = basefs_release,
    .sync            = basefs_sync,
    .volume_info     = basefs_volume_info,
    .used_bytes      = basefs_used_bytes,
    .max_capacity    = basefs_max_capacity,
    .format_check    = basefs_format_check,
    .format          = basefs_format,
    .check           = basefs_check,

    .root            = basefs_root,
    .node_type       = basefs_node_type,
    .parent          = basefs_parent,
    .lookup          = basefs_lookup,
    .dir_count       = basefs_dir_count,
    .dir_entry       = basefs_dir_entry,
    .create          = basefs_create,
    .remove          = basefs_remove,
    .rename          = basefs_rename,
    .resize          = basefs_resize,
    .get_attr        = basefs_get_attr,
    .set_permissions = basefs_set_permissions,
    .set_security    = basefs_set_security,
    .set_certificate = basefs_set_certificate,

    .open            = basefs_open,
    .close           = basefs_close,
    .read            = basefs_read,
    .write           = basefs_write,
    .seek            = basefs_seek,
    .tell            = basefs_tell,
};

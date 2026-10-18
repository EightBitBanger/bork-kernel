#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>
#include <kernel/fs/fs.h>

#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/tok.h>
#include <kernel/util/list.h>

// VFS_NAME_MAX is the public name limit and must match what basefs stores.
// (Array size goes negative, failing the build, if the two ever drift apart.)
typedef char vfs_name_max_matches_fs[(VFS_NAME_MAX == FS_NAME_MAX) ? 1 : -1];

bool vfs_name_is_valid(const char* name) {
    if (name == NULL || name[0] == '\0' || name[0] == ' ')
        return false;
    size_t length = strnlen(name, VFS_NAME_MAX + 1);
    if (length > VFS_NAME_MAX)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c < 0x20 || c == 0x7F)
            return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    return true;
}

bool vfs_path_fits(const char* path) {
    if (path == NULL || path[0] == '\0')
        return false;
    return strnlen(path, VFS_PATH_MAX) < VFS_PATH_MAX;
}

// If `knode` is a mount whose device is open, enter that device's root.
//
// A mount whose device is not usable (unformatted, another format version,
// detached by "boot install") has nothing inside it. It still resolves as
// the LAST component of a path, as the mount knode itself, so it can be
// listed (empty), stat'ed, entered with cd and formatted. Any path that
// tries to go through it returns false ("does not exist"): its knode
// references are the cache window and the context, not children, and
// walking them as directory entries would read raw memory as names.
static bool vfs_enter_mount(uint32_t knode, bool final, uint32_t* fs_node, struct FSDeviceContext** ctx, bool* in_file_system) {
    uint8_t flags = kmalloc_get_flags(knode);
    if (!(flags & KMALLOC_FLAG_MOUNT))
        return true;

    uint32_t device_address = knode_get_reference(knode, 0);
    uint32_t device_context = knode_get_reference(knode, 1);
    if (device_context == 0 || device_context == KMALLOC_NULL ||
        device_address == KMALLOC_NULL || device_address == 0)
        return final;

    struct FSDeviceContext* context = (struct FSDeviceContext*)device_context;
    struct FSPartitionBlock partition;
    if (!context->is_open || fs_device_get_partition(context, &partition) != 0)
        return final;   // Bad magic, another format version, or detached

    *fs_node        = partition.root_directory;
    *ctx            = context;
    *in_file_system = true;
    return true;
}

// ".." inside a device. Leaves the device when standing on its root.
static void vfs_fs_parent(uint32_t* knode, uint32_t* fs_node, struct FSDeviceContext** ctx, bool* in_file_system) {
    struct FSPartitionBlock partition;
    fs_device_get_partition(*ctx, &partition);

    if (*fs_node == partition.root_directory) {
        *in_file_system = false;
        *fs_node        = 0;
        *ctx            = NULL;
        *knode          = knode_get_parent(*knode);
    } else {
        *fs_node = fs_directory_get_parent(*ctx, *fs_node);
    }
}

bool vfs_path_normalize(const char* base, const char* path, char* out, size_t size) {
    if (path == NULL || out == NULL || size < 2)
        return false;

    // Build "<base>/<path>" (or just <path> when absolute) in a scratch copy
    // first, so `out` may alias either input
    char scratch[VFS_PATH_MAX];
    size_t path_length = strnlen(path, VFS_PATH_MAX);
    if (path_length >= VFS_PATH_MAX)
        return false;

    if (path[0] == '/') {
        memcpy(scratch, path, path_length + 1);
    } else {
        if (base == NULL || base[0] != '/')
            return false;
        size_t base_length = strnlen(base, VFS_PATH_MAX);
        if (base_length + 1 + path_length + 1 > sizeof(scratch))
            return false;
        memcpy(scratch, base, base_length);
        scratch[base_length] = '/';
        memcpy(scratch + base_length + 1, path, path_length + 1);
    }

    size_t length = 0;
    out[length++] = '/';

    cstr_tok_t tok;
    cstr_tok_init(&tok, scratch, "/");
    for (char* name = cstr_tok_next(&tok); name != NULL; name = cstr_tok_next(&tok)) {
        if (strcmp(name, ".") == 0)
            continue;

        if (strcmp(name, "..") == 0) {
            // Drop the last name and the slash before it; "/" stays "/"
            while (length > 1 && out[length - 1] != '/')
                length--;
            if (length > 1)
                length--;
            continue;
        }

        size_t name_length = strlen(name);
        size_t slash = (length > 1) ? 1 : 0;
        if (length + slash + name_length + 1 > size)
            return false;
        if (slash)
            out[length++] = '/';
        memcpy(out + length, name, name_length);
        length += name_length;
    }

    out[length] = '\0';
    return true;
}

uint32_t resolve_path_to_address(const char* path) {
    if (!vfs_path_fits(path))
        return 0xFFFFFFFF;

    uint32_t current_knode = knode_get_root();
    uint32_t current_fs_node = 0;
    bool in_file_system = false;
    struct FSDeviceContext* current_ctx = NULL;

    char path_scratch[VFS_PATH_MAX];
    strcpy(path_scratch, path);     // Fits: checked above

    cstr_tok_t tok;
    cstr_tok_init(&tok, path_scratch, "/");

    // One token of lookahead: a mount whose device is unusable may only be
    // the last component (see vfs_enter_mount)
    char* token = cstr_tok_next(&tok);
    while (token != NULL) {
        char* next = cstr_tok_next(&tok);

        if (strcmp(token, ".") == 0) {
            token = next;
            continue;
        }

        if (!in_file_system) {
            if (strcmp(token, "..") == 0) {
                current_knode = knode_get_parent(current_knode);
            } else {
                uint32_t next_node = knode_find_by_name(current_knode, token);
                if (next_node == 0xFFFFFFFF || next_node == 0) {
                    return 0xFFFFFFFF;
                }
                current_knode = next_node;
            }

            if (!vfs_enter_mount(current_knode, next == NULL, &current_fs_node, &current_ctx, &in_file_system))
                return 0xFFFFFFFF;
        } else {
            if (strcmp(token, "..") == 0) {
                vfs_fs_parent(&current_knode, &current_fs_node, &current_ctx, &in_file_system);
            } else {
                // Names live in the directory: this reads only its sectors
                uint32_t found = fs_directory_find(current_ctx, current_fs_node, token);
                if (found == 0 || found == FS_NULL) {
                    return 0xFFFFFFFF;
                }
                current_fs_node = found;
            }
        }

        token = next;
    }

    return in_file_system ? current_fs_node : current_knode;
}

uint32_t resolve_path_to_mount_point(const char* path) {
    if (!vfs_path_fits(path))
        return 0xFFFFFFFF;
    uint32_t current_knode = knode_get_root();

    if (kmalloc_get_flags(current_knode) & KMALLOC_FLAG_MOUNT) {
        return current_knode;
    }

    char path_scratch[VFS_PATH_MAX];
    strcpy(path_scratch, path);     // Fits: checked above
    cstr_tok_t tok;
    cstr_tok_init(&tok, path_scratch, "/");

    char* token = cstr_tok_next(&tok);
    while (token != NULL) {
        if (strcmp(token, ".") == 0) {
            token = cstr_tok_next(&tok);
            continue;
        }

        if (strcmp(token, "..") == 0) {
            current_knode = knode_get_parent(current_knode);
        } else {
            uint32_t next_node = knode_find_by_name(current_knode, token);
            if (next_node == 0xFFFFFFFF || next_node == 0) {
                return 0xFFFFFFFF;
            }
            current_knode = next_node;
        }

        uint8_t flags = kmalloc_get_flags(current_knode);
        if (flags & KMALLOC_FLAG_MOUNT) {
            return current_knode;
        }

        token = cstr_tok_next(&tok);
    }

    return current_knode;
}

uint32_t resolve_parent_path_to_address(const char* path) {
    if (!vfs_path_fits(path)) {
        return 0xFFFFFFFF;
    }

    char parent_path[VFS_PATH_MAX];
    const char* last_slash = strrchr(path, '/');
    if (last_slash == NULL) {
        strcpy(parent_path, ".");
    }
    else if (last_slash == path) {
        strcpy(parent_path, "/");
    }
    else {
        size_t parent_len = last_slash - path;
        memcpy(parent_path, path, parent_len);     // Shorter than path: fits
        parent_path[parent_len] = '\0';
    }

    return resolve_path_to_address(parent_path);
}

bool vfs_parse_path(const char* path, uint16_t flags, uint32_t* out_knode, uint32_t* out_fs_node, bool* out_in_fs, struct FSDeviceContext** out_ctx) {
    if (!vfs_path_fits(path))
        return false;

    uint32_t current_knode = knode_get_root();
    uint32_t current_fs_node = 0;
    bool in_file_system = false;
    struct FSDeviceContext* current_ctx = NULL;

    char path_scratch[VFS_PATH_MAX];
    strcpy(path_scratch, path);     // Fits: checked above

    cstr_tok_t tok;
    cstr_tok_init(&tok, path_scratch, "/");

    char* token = cstr_tok_next(&tok);
    while (token != NULL) {
        char* next = cstr_tok_next(&tok);

        if (strcmp(token, ".") == 0) {
            token = next;
            continue;
        }

        if (!in_file_system) {
            if (strcmp(token, "..") == 0) {
                current_knode = knode_get_parent(current_knode);
            } else {
                uint32_t next_node = knode_find_by_name(current_knode, token);
                if (next_node == KNODE_NULL || next_node == 0) {
                    return false;
                }
                current_knode = next_node;
            }

            if (!vfs_enter_mount(current_knode, next == NULL, &current_fs_node, &current_ctx, &in_file_system))
                return false;
        } else {
            if (strcmp(token, "..") == 0) {
                vfs_fs_parent(&current_knode, &current_fs_node, &current_ctx, &in_file_system);
            } else {
                uint32_t found = fs_directory_find(current_ctx, current_fs_node, token);

                if (found == 0 || found == FS_NULL) {
                    // Missing last component + VFS_OPEN_CREATE: make the file
                    if (next != NULL || !(flags & VFS_OPEN_CREATE))
                        return false;

                    // Refuse (never shorten) names that cannot be stored
                    if (!vfs_name_is_valid(token))
                        return false;

                    // Creating an entry changes the directory
                    uint8_t dir_perm = 0;
                    fs_file_get_permissions(current_ctx, current_fs_node, &dir_perm);
                    if (!(dir_perm & FS_PERMISSION_WRITE))
                        return false;

                    // ...and needs the directory's security clearance
                    if (!vfs_fs_node_allowed(current_ctx, current_fs_node))
                        return false;

                    uint8_t default_perms = FS_PERMISSION_READ | FS_PERMISSION_WRITE;
                    uint32_t new_file_address = fs_file_create(current_ctx, token, default_perms, 0, current_fs_node);
                    if (new_file_address == FS_NULL) {
                        return false;
                    }
                    current_fs_node = new_file_address;
                    break;
                }
                current_fs_node = found;
            }
        }
        token = next;
    }

    *out_knode = current_knode;
    *out_fs_node = current_fs_node;
    *out_in_fs = in_file_system;
    if (out_ctx) *out_ctx = current_ctx;
    return true;
}



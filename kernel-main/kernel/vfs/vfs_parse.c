#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>

#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/tok.h>
#include <kernel/util/list.h>

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
// reference is the FsMount, not a child, and walking it as a directory
// entry would read raw memory as names.
static bool vfs_enter_mount(void* knode, bool final, uint64_t* fs_node, struct FsMount** mount, bool* in_file_system) {
    uint8_t flags = kmalloc_get_flags(knode);
    if (!(flags & KMALLOC_FLAG_MOUNT))
        return true;

    struct FsMount* m = vfs_knode_mount(knode);
    if (m == NULL || !m->is_open)
        return final;

    uint64_t root = m->type->root(m);
    if (root == FS_NODE_NULL)
        return final;   // Bad magic, another format version, or detached

    *fs_node        = root;
    *mount          = m;
    *in_file_system = true;
    return true;
}

// ".." inside a device. Leaves the device when standing on its root.
static void vfs_fs_parent(void** knode, uint64_t* fs_node, struct FsMount** mount, bool* in_file_system) {
    struct FsMount* m = *mount;
    uint64_t parent = (*fs_node == m->type->root(m)) ? FS_NODE_NULL : m->type->parent(m, *fs_node);

    if (parent == FS_NODE_NULL) {
        *in_file_system = false;
        *fs_node        = 0;
        *mount          = NULL;
        *knode          = knode_get_parent(*knode);
    } else {
        *fs_node = parent;
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

uint64_t resolve_path_to_address(const char* path) {
    if (!vfs_path_fits(path))
        return FS_NODE_NULL;

    void* current_knode = knode_get_root();
    uint64_t current_fs_node = 0;
    bool in_file_system = false;
    struct FsMount* current_mount = NULL;

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
                void* next_node = knode_find_by_name(current_knode, token);
                if (next_node == NULL) {
                    return FS_NODE_NULL;
                }
                current_knode = next_node;
            }

            if (!vfs_enter_mount(current_knode, next == NULL, &current_fs_node, &current_mount, &in_file_system))
                return FS_NODE_NULL;
        } else {
            // Going through a directory, into a child or back out with "..",
            // needs the caller's clearance for it (the Unix search check).
            // The object at the end of the path is the caller's to check.
            if (!vfs_fs_node_allowed(current_mount, current_fs_node))
                return FS_NODE_NULL;

            if (strcmp(token, "..") == 0) {
                vfs_fs_parent(&current_knode, &current_fs_node, &current_mount, &in_file_system);
            } else {
                // Names live in the directory: this reads only its sectors
                uint64_t found = current_mount->type->lookup(current_mount, current_fs_node, token);
                if (found == 0 || found == FS_NODE_NULL) {
                    return FS_NODE_NULL;
                }
                current_fs_node = found;
            }
        }

        token = next;
    }

    return in_file_system ? current_fs_node : vfs_knode_address(current_knode);
}

void* resolve_path_to_mount_point(const char* path) {
    if (!vfs_path_fits(path))
        return NULL;
    void* current_knode = knode_get_root();

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
            void* next_node = knode_find_by_name(current_knode, token);
            if (next_node == NULL) {
                return NULL;
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

uint64_t resolve_parent_path_to_address(const char* path) {
    if (!vfs_path_fits(path)) {
        return FS_NODE_NULL;
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

bool vfs_parse_path(const char* path, uint16_t flags, void** out_knode, uint64_t* out_fs_node, bool* out_in_fs, struct FsMount** out_mount) {
    if (!vfs_path_fits(path))
        return false;

    void* current_knode = knode_get_root();
    uint64_t current_fs_node = 0;
    bool in_file_system = false;
    struct FsMount* current_mount = NULL;

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
                void* next_node = knode_find_by_name(current_knode, token);
                if (next_node == NULL) {
                    return false;
                }
                current_knode = next_node;
            }

            if (!vfs_enter_mount(current_knode, next == NULL, &current_fs_node, &current_mount, &in_file_system))
                return false;
        } else {
            // The search check, as in resolve_path_to_address()
            if (!vfs_fs_node_allowed(current_mount, current_fs_node))
                return false;

            if (strcmp(token, "..") == 0) {
                vfs_fs_parent(&current_knode, &current_fs_node, &current_mount, &in_file_system);
            } else {
                uint64_t found = current_mount->type->lookup(current_mount, current_fs_node, token);

                if (found == 0 || found == FS_NODE_NULL) {
                    // Missing last component + VFS_OPEN_CREATE: make the file
                    if (next != NULL || !(flags & VFS_OPEN_CREATE))
                        return false;

                    // Refuse (never shorten) names that cannot be stored
                    if (!vfs_name_is_valid(token))
                        return false;

                    // Creating an entry changes the directory
                    uint8_t dir_perm = vfs_node_permissions(current_mount, current_fs_node);
                    if (!(dir_perm & VFS_PERMISSION_WRITE))
                        return false;

                    // ...and needs the directory's security clearance
                    if (!vfs_fs_node_allowed(current_mount, current_fs_node))
                        return false;

                    uint8_t default_perms = VFS_PERMISSION_READ | VFS_PERMISSION_WRITE;
                    uint64_t new_file_address = current_mount->type->create(current_mount, current_fs_node, token,
                                                                            FS_NODE_FILE, default_perms);
                    if (new_file_address == FS_NODE_NULL) {
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
    if (out_mount) *out_mount = current_mount;
    return true;
}

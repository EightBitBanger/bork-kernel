
#ifndef BASE_DIRECTORY_H
#define BASE_DIRECTORY_H

#include <kernel/fs/fs.h>

#include <stddef.h>

// Directories own the names of everything in them. Each entry is a record
// (see FSDirEntry in structs.h) holding the child's address, its type and
// its name, so looking a name up only ever reads directory sectors.
//
// Names passed in are NUL-terminated, 1..FS_NAME_MAX bytes, and contain no
// '/'. Names handed back are NUL-terminated and need a buffer of at least
// FS_NAME_MAX + 1 bytes to be sure to fit.

// Result codes of the entry functions (0 = success)
#define FS_DIR_OK               0
#define FS_DIR_ERR_ARGUMENT     1   // Bad name, FS_NULL address, ...
#define FS_DIR_ERR_NOT_DIR      2   // Address is not a directory
#define FS_DIR_ERR_EXISTS       3   // The name is already taken
#define FS_DIR_ERR_NOT_FOUND    4   // No such entry
#define FS_DIR_ERR_NO_SPACE     5   // Could not allocate a new extent
#define FS_DIR_ERR_CORRUPT      6   // Malformed records or chain

// Create a directory. With a parent it is linked there under `name`; the
// partition root is created with parent FS_NULL (name is then ignored).
uint64_t fs_directory_create(struct FSDeviceContext* ctx, const char* name, uint8_t permissions, uint64_t parent_directory);

// Frees only the directory's own header and extents; entries are NOT freed
// and the directory is NOT unlinked from its parent.
// Use fs_directory_delete_recursive to remove a directory with its contents.
bool     fs_directory_delete(struct FSDeviceContext* ctx, uint64_t address);

// Unlinks the directory from parent_directory (if not FS_NULL), then frees
// every file and subdirectory below it and finally the directory itself.
// Refuses the partition root. Does not check permissions.
bool     fs_directory_delete_recursive(struct FSDeviceContext* ctx, uint64_t address, uint64_t parent_directory);

// Entries
//
// add:    link `child` under `name`. type is FS_ENTRY_TYPE_FILE or _DIRECTORY.
//         Fails with FS_DIR_ERR_EXISTS if the name is taken.
// remove: unlink the entry naming `child` (the child itself is untouched).
// rename: give `child` a new name in this directory. Fails with
//         FS_DIR_ERR_EXISTS if another entry already has that name.
uint8_t  fs_directory_add_entry(struct FSDeviceContext* ctx, uint64_t directory_address, const char* name, uint64_t child, uint8_t type);
uint8_t  fs_directory_remove_entry(struct FSDeviceContext* ctx, uint64_t directory_address, uint64_t child);
uint8_t  fs_directory_rename_entry(struct FSDeviceContext* ctx, uint64_t directory_address, uint64_t child, const char* new_name);

// Index-based access, in on-disk order. Indices run 0..count-1. Entries
// added or removed in between shift the order, so hold the VFS lock across
// a listing loop.
uint32_t fs_directory_get_reference_count(struct FSDeviceContext* ctx, uint64_t directory_address);
uint64_t fs_directory_get_reference(struct FSDeviceContext* ctx, uint64_t directory_address, uint32_t index);

// Entry `index`: any output pointer may be NULL. Returns false if there is
// no such entry, or if `name` is given and name_size cannot hold the name
// and its terminator (nothing is truncated).
bool     fs_directory_get_entry(struct FSDeviceContext* ctx, uint64_t directory_address, uint32_t index,
                                uint64_t* address, char* name, size_t name_size, uint8_t* type);

// Name under which `child` is linked in this directory
bool     fs_directory_get_name(struct FSDeviceContext* ctx, uint64_t directory_address, uint64_t child,
                               char* name, size_t name_size);

// Address of the entry called `name`, or FS_NULL
uint64_t fs_directory_find(struct FSDeviceContext* ctx, uint64_t directory_address, const char* name);

// Parent directory (FS_NULL for the partition root or on error)
uint64_t fs_directory_get_parent(struct FSDeviceContext* ctx, uint64_t directory_address);

#endif

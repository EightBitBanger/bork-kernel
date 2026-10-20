#ifndef _VFS_ADDITION_H_
#define _VFS_ADDITION_H_

// Totals for a directory tree (or a single file)
typedef struct {
    uint64_t bytes;         // Sum of every file's size
    uint32_t files;         // Files counted
    uint32_t folders;       // Folders below `path` (not counting itself)
    bool     complete;      // False if something was skipped (nesting too
                            // deep, or a storage device mounted inside)
} VfsDirectorySize;

// Add up everything below `path`. A file gives just its own size. The VFS
// lock is held for the whole walk, so on a very large tree this stalls
// other VFS users until it ends.
// Returns false when the path does not exist. See vfs_size.c.
bool vfs_directory_get_size(const char* path, VfsDirectorySize* out);

#endif

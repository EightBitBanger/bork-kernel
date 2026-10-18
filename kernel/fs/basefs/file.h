#ifndef BASE_FILE_H
#define BASE_FILE_H

#include <kernel/fs/fs.h>

// Files have no name of their own: the directory entry that links a file
// holds its name (see directory.h). A file is addressed by its header.

typedef struct {
    uint32_t address;
    uint32_t position;
    uint8_t  mode;
    uint8_t  is_open;
    uint8_t  written;       // Set by a write; close then stamps `modified`
    struct FSDeviceContext* ctx;

    // Cursor into the file: the part that last served a read or write and
    // the file offset of its first byte. cur_extent == address means the
    // inline data after the header; anything else is an FSFileExtent.
    // Sequential access then costs one extent hop per extent instead of a
    // walk from the head on every call. Only trusted while cur_generation
    // matches the header.
    uint32_t cur_extent;
    uint32_t cur_start;
    uint32_t cur_size;      // Its data size when last read (only ever grows)
    uint16_t cur_generation;
} FileHandle;

bool fs_file_check(struct FSDeviceContext* ctx, uint32_t address);

// Create a file of `size` zero bytes and link it into parent_directory
// under `name` (parent FS_NULL: create unlinked). Fails, leaving nothing
// behind, if the name is taken or space runs out.
uint32_t fs_file_create(struct FSDeviceContext* ctx, const char* name, uint8_t permissions, uint32_t size, uint32_t parent_directory);

// Frees the file's storage. Does NOT unlink it from its directory; do that
// first with fs_directory_remove_entry.
bool fs_file_delete(struct FSDeviceContext* ctx, uint32_t address);

bool fs_file_resize(struct FSDeviceContext* ctx, uint32_t address, uint32_t new_size);
uint32_t fs_file_get_size(struct FSDeviceContext* ctx, uint32_t address);

// The getters and setters below work on files and directories alike (they
// only touch the shared FSBlockHeader)
bool fs_file_get_permissions(struct FSDeviceContext* ctx, uint32_t address, uint8_t* permissions);
bool fs_file_set_permissions(struct FSDeviceContext* ctx, uint32_t address, uint8_t permissions);

bool fs_file_get_attributes(struct FSDeviceContext* ctx, uint32_t address, uint8_t* attributes);
bool fs_file_set_attributes(struct FSDeviceContext* ctx, uint32_t address, uint8_t attributes);

// Seconds since the Unix epoch (0 = unknown). Either pointer may be NULL.
bool fs_file_get_times(struct FSDeviceContext* ctx, uint32_t address, uint32_t* created, uint32_t* modified);

// Directory the object is linked in (FS_NULL for the root or on error)
uint32_t fs_file_get_parent(struct FSDeviceContext* ctx, uint32_t address);

bool fs_file_open(FileHandle* file, struct FSDeviceContext* ctx, uint32_t address, uint8_t mode);
void fs_file_close(FileHandle* file);

uint32_t fs_file_read(FileHandle* file, void* destination, uint32_t size);
uint32_t fs_file_write(FileHandle* file, const void* source, uint32_t size);

bool fs_file_get_certificate(struct FSDeviceContext* ctx, uint32_t address, uint32_t* cert);
bool fs_file_set_certificate(struct FSDeviceContext* ctx, uint32_t address, uint32_t cert);
bool fs_file_get_security_flag(struct FSDeviceContext* ctx, uint32_t address, uint8_t* security);  // Low byte only

// The full 32-bit security mask (files and directories alike)
bool fs_file_get_security(struct FSDeviceContext* ctx, uint32_t address, uint32_t* security);
bool fs_file_set_security_flag(struct FSDeviceContext* ctx, uint32_t address, uint32_t security);

uint32_t fs_file_seek(FileHandle* file, uint32_t position);
uint32_t fs_file_tell(const FileHandle* file);
uint32_t fs_file_get_address(const FileHandle* file);

#endif


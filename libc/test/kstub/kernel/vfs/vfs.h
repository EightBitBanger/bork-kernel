/* Test stub: the slice of the kernel's VFS the resource manager uses, on
   host files (gfx_shim.c). Same names, types and return conventions. */
#ifndef _KSTUB_VFS_H
#define _KSTUB_VFS_H
#include <stdint.h>
#include <stdbool.h>

typedef uint32_t File;
#define VFS_INVALID_FILE    0
#define VFS_OPEN_READ       0x01
#define VFS_OPEN_WRITE      0x02
#define VFS_OPEN_CREATE     0x04

File     vfs_open(const char* path, uint16_t flags);
void     vfs_close(File file);
int32_t  vfs_read(File file, void* buffer, uint32_t size);
int32_t  vfs_write(File file, const void* buffer, uint32_t size);
uint32_t vfs_seek(File file, uint32_t position);
uint32_t vfs_tell(File file);
uint32_t vfs_get_size(File file);
bool     vfs_truncate(const char* path, uint32_t new_size);
#endif


#ifndef _BOOT_ELF_DROPPER_H_
#define _BOOT_ELF_DROPPER_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>

#include <kernel/console/commands/boot_cabinate.h>
#define INPUT_ELF_SIZE       ((uint32_t)sizeof(input_demo))
#define GFX_DEMO_ELF_SIZE    ((uint32_t)sizeof(gfx_demo))
#define SPRITE_DEMO_ELF_SIZE ((uint32_t)sizeof(sprite_demo))

// Write `size` bytes of `bytes` to `path` (absolute, on a mounted device,
// e.g. "/mnt/ata0p1/usr/desktop/hello") and mark it executable. Overwrites an
// existing file. Thread context only (VFS). Returns true on success.
static inline bool drop_elf_bytes(const char* path, const uint8_t* bytes, uint32_t size) {
    // Create it if missing, then size it before writing: writes don't
    // reliably extend a file (same sequence as dwm_desktop_layout_save)
    File file = vfs_open(path, VFS_OPEN_CREATE | VFS_OPEN_READ | VFS_OPEN_WRITE);
    if (file == VFS_INVALID_FILE) return false;
    vfs_close(file);

    if (!vfs_truncate(path, size)) return false;

    file = vfs_open(path, VFS_OPEN_READ | VFS_OPEN_WRITE);
    if (file == VFS_INVALID_FILE) return false;

    vfs_seek(file, 0);
    uint32_t done = 0;
    while (done < size) {
        int32_t n = vfs_write(file, bytes + done, size - done);
        if (n <= 0) break;
        done += (uint32_t)n;
    }
    vfs_close(file);
    if (done != size) return false;

    return vfs_set_permissions(path, VFS_PERMISSION_READ | VFS_PERMISSION_WRITE | VFS_PERMISSION_EXECUTE);
}

// Join `dir` and `name` into `out` (capacity `cap`), inserting a '/' unless
// `dir` already ends in one. Returns false if it would not fit.
static inline bool drop_join_path(char* out, uint32_t cap, const char* dir, const char* name) {
    uint32_t dlen = (uint32_t)strlen(dir);
    uint32_t nlen = (uint32_t)strlen(name);
    bool slash = (dlen > 0 && dir[dlen - 1] == '/');
    uint32_t need = dlen + (slash ? 0 : 1) + nlen + 1;  // + NUL
    if (need > cap) return false;

    memcpy(out, dir, dlen);
    uint32_t pos = dlen;
    if (!slash) out[pos++] = '/';
    memcpy(out + pos, name, nlen);
    pos += nlen;
    out[pos] = '\0';
    return true;
}

// Drop both test programs into directory `dir` (absolute, on a mounted
// device, e.g. "/mnt/ata0p1/usr/desktop") as "hello" and "crash". Thread
// context only (VFS). Returns true only if both were written.
static inline bool dropper_all(const char* dir) {
    char path[256];
    bool ok = true;
    
    if (!drop_join_path(path, sizeof(path), dir, "input_demo")) return false;
    ok = drop_elf_bytes(path, input_demo, INPUT_ELF_SIZE) && ok;
    
    if (!drop_join_path(path, sizeof(path), dir, "gfx_demo")) return false;
    ok = drop_elf_bytes(path, gfx_demo, GFX_DEMO_ELF_SIZE) && ok;
    
    if (!drop_join_path(path, sizeof(path), dir, "sprite_demo")) return false;
    ok = drop_elf_bytes(path, sprite_demo, SPRITE_DEMO_ELF_SIZE) && ok;
    
    return ok;
}

#endif

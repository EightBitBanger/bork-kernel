#ifndef CONSOLE_CMD_BOOT_ASSETS_H
#define CONSOLE_CMD_BOOT_ASSETS_H

// Compiled-in assets that "boot install" / "boot repair" write to disk.
//
// Two kinds, two tables:
//
//   BootByteAsset     raw bytes written verbatim (ELF executables and the
//                     like). Dropped into <root>/sys.
//   BootSpriteAsset   a struct Sprite, written through the resource manager
//                     so it gets the 16-byte "RSRC" header and can be read
//                     back with resource_sprite_load(). Dropped into
//                     <root>/sys/images.
//
// `root` is the device root the command is working on: "/mnt/<part>" during
// install, "/mnt/<device>" during repair. Once that device is the home
// device the effective paths are /mnt/<home>/sys and /mnt/<home>/sys/images.
//
// Adding an asset is one new row (and its #include). The subdirectories are
// created by boot_make_layout() (they are in boot_system_directories), so
// the drop only writes files.
//
// Overwrite policy (same as boot_export_images_to's contract):
//   overwrite == false  a file that already exists is kept, so a copy edited
//                        on disk is never clobbered by the built-in one.
//   overwrite == true   an existing file is removed and written again
//                        (repair force).

#include <stdint.h>
#include <stdbool.h>

#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>

#include <kernel/console/print.h>

#include <kernel/resources/classes/sprite.h>
#include <kernel/resources/classes/image.h>

// The embedded assets
#ifdef ADD_IMAGE_LIB
 #include <kernel/boot_images.h>
#endif
#ifdef ADD_BINARY_LIB
 #include <kernel/boot_cabinate.h>
#endif


// Write `bytes` to `path` as an executable, creating it if missing.
//
// Create it, then size it before writing: writes don't reliably extend a
// file, so the file is truncated to `size` first (same sequence the resource
// manager and dwm_desktop_layout_save use). On success the file is left with
// read, write and execute permission.
static inline bool boot_write_executable(const char* path, const uint8_t* bytes, uint32_t size) {
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

// "<dir>/<name>" into out, with exactly one separator. False if it does not
// fit in `cap` (terminator included).
static inline bool boot_join_path(char* out, uint32_t cap, const char* dir, const char* name) {
    uint32_t dlen = (uint32_t)strlen(dir);
    uint32_t nlen = (uint32_t)strlen(name);
    bool slash = (dlen > 0 && dir[dlen - 1] == '/');
    uint32_t need = dlen + (slash ? 0 : 1) + nlen + 1;  // + NUL
    if (need > cap) return false;

    // Byte copies, not memcpy: the destination is a known-size stack buffer
    // and the lengths are runtime, so memcpy would be rewritten to the
    // fortified __memcpy_chk, which a freestanding (-nostdlib) kernel has no
    // definition for.
    uint32_t pos = 0;
    for (uint32_t i = 0; i < dlen; i++) out[pos++] = dir[i];
    if (!slash) out[pos++] = '/';
    for (uint32_t i = 0; i < nlen; i++) out[pos++] = name[i];
    out[pos] = '\0';
    return true;
}

//
// Asset tables
//

// A raw byte blob and the name it is saved under in <root>/sys
typedef struct {
    const char*    name;        // File name within the sys directory
    const uint8_t* bytes;
    uint32_t       size;
} BootByteAsset;

// A sprite and the name it is saved under in <root>/sys/images
typedef struct {
    const char*          name;  // File name within the images directory
    const struct Sprite* sprite;
} BootSpriteAsset;

// Byte assets, dropped into /sys.
//
// sizeof(array), not the xxd-generated <name>_len: the length symbol is a
// plain global variable, so reading it is not a constant expression and a
// static initializer rejects it. sizeof is a compile-time constant and
// equals the same byte count (the array's full definition is visible here
// through boot_cabinate.h).
static const BootByteAsset boot_byte_assets[] = {
    
#ifdef ADD_BINARY_LIB
    
    // OS core
    
    { "dwm",            dwm,             (uint32_t)sizeof(dwm) },
    { "prop",           properties,      (uint32_t)sizeof(properties) },
    { "fileop",         fileop,          (uint32_t)sizeof(fileop) },
    
    { "paint",          paint,           (uint32_t)sizeof(paint) },
    { "hexedit",        hexedit,         (uint32_t)sizeof(hexedit) },
    
    { "calc",           calc,            (uint32_t)sizeof(calc) },
    { "charmap",        charmap,         (uint32_t)sizeof(charmap) },
    { "minesweeper",    minesweeper,     (uint32_t)sizeof(minesweeper) },
    
    //{ "gd.ttf",         gd_ttf,          (uint32_t)sizeof(gd_ttf) },
    
    // Programs
    
    { "explorer",       explorer,        (uint32_t)sizeof(explorer) },
    { "notepad",        notepad,         (uint32_t)sizeof(notepad) },
    
#endif
};

#define BOOT_BYTE_ASSET_COUNT \
    (sizeof(boot_byte_assets) / sizeof(boot_byte_assets[0]))

// Sprite assets, dropped into /sys/images
static const BootSpriteAsset boot_sprite_assets[] = {
    
#ifdef ADD_IMAGE_LIB
    
    { "ui_back",             &rc_back },
    { "ui_new",              &rc_new },
    { "ui_close",            &rc_close },
    { "ui_minimize",         &rc_minimize },
    
    { "cur_corner",          &rc_angle },
    { "cur_edge",            &rc_edge },
    { "cur_pointer",         &rc_pointer },
    
    { "icon_document",       &rc_document },
    { "icon_error",          &rc_error },
    { "icon_file",           &rc_file },
    { "icon_folder",         &rc_folder },
    { "icon_storage",        &rc_storage },
    { "icon_storage_hdd",    &rc_storage_hdd },
    { "icon_system",         &rc_system },
    
#endif
};

#define BOOT_SPRITE_ASSET_COUNT \
    (sizeof(boot_sprite_assets) / sizeof(boot_sprite_assets[0]))

//
// Dropping
//

// Counts for one drop pass. `written` is files created or replaced, `kept`
// is files left alone (already there, not overwriting), `failed` is errors.
typedef struct {
    uint32_t written;
    uint32_t kept;
    uint32_t failed;
} BootAssetStats;

// "<root><subdir>/<name>" into out. False (out empty) if it does not fit.
static bool boot_asset_path(char* out, uint32_t cap, const char* root,
                            const char* subdir, const char* name) {
    char dir[192];
    if (!boot_join_path(dir, sizeof(dir), root, subdir))
        return false;
    return boot_join_path(out, cap, dir, name);
}

// If `path` exists: keep it (return true, *kept) unless overwriting, in which
// case remove it first. Returns false only if an existing file could not be
// removed. *kept is set true when the caller should skip the write.
static bool boot_asset_prepare(const char* path, bool overwrite, bool* kept) {
    *kept = false;
    if (!vfs_exists(path))
        return true;
    if (!overwrite) {
        *kept = true;
        return true;
    }
    return vfs_remove(path);
}

// Write every byte asset into <root>/sys
static void boot_drop_byte_assets(const char* root, bool overwrite, BootAssetStats* stats) {
    for (uint32_t i = 0; i < BOOT_BYTE_ASSET_COUNT; i++) {
        const BootByteAsset* asset = &boot_byte_assets[i];

        char path[256];
        if (!boot_asset_path(path, sizeof(path), root, "/sys", asset->name)) {
            stats->failed++;
            continue;
        }

        bool kept = false;
        if (!boot_asset_prepare(path, overwrite, &kept)) {
            stats->failed++;
            continue;
        }
        if (kept) {
            stats->kept++;
            continue;
        }

        if (boot_write_executable(path, asset->bytes, asset->size))
            stats->written++;
        else
            stats->failed++;
    }
}

// Write every sprite asset into <root>/sys/images
static void boot_drop_sprite_assets(const char* root, bool overwrite, BootAssetStats* stats) {
    for (uint32_t i = 0; i < BOOT_SPRITE_ASSET_COUNT; i++) {
        const BootSpriteAsset* asset = &boot_sprite_assets[i];

        char path[256];
        if (!boot_asset_path(path, sizeof(path), root, "/sys/images", asset->name)) {
            stats->failed++;
            continue;
        }

        bool kept = false;
        if (!boot_asset_prepare(path, overwrite, &kept)) {
            stats->failed++;
            continue;
        }
        if (kept) {
            stats->kept++;
            continue;
        }

        if (resource_sprite_save(path, asset->sprite))
            stats->written++;
        else
            stats->failed++;
    }
}

#endif

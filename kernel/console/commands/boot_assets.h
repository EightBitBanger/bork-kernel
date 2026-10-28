#ifndef CONSOLE_CMD_BOOT_ASSETS_H
#define CONSOLE_CMD_BOOT_ASSETS_H

// Assets that "boot install" / "boot repair" write to disk.
//
// All of them come from the asset archive: a ustar file (bin/assets.tar,
// built by build-assets.sh from assets.txt) that stage 2 reads off the boot
// disk behind the kernel and hands over in memory (boot_assets_get). Every
// file in it is written verbatim: ELF executables, maps, fonts, and the
// images, which the build already turned into resource files (16-byte
// "RSRC" header, build-sprites.sh) so resource_sprite_load() reads them.
//
// Every path is relative to the home root ("sys/dwm",
// "games/flight/maps/gulf.map"). `root` is the device root the command is
// working on: "/mnt/<part>" during install, "/mnt/<device>" during repair.
// Once that device is the home device "bin/calc" ends up at
// /mnt/<home>/bin/calc.
//
// Adding a file to the archive is one line in assets.txt, no kernel change.
// Folders are not pre-created: the drop walks each file's path and makes
// any parent folder that is missing (mkdir -p), so a new subdirectory needs
// no other change. Folder entries in the archive are made too (empty ones
// included). A file is executable on disk when its archive mode has an
// execute bit.
//
// Overwrite policy:
//   overwrite == false  a file that already exists is kept, so a copy edited
//                        on disk is never clobbered by the archive's one. A
//                        file standing where a folder belongs is left alone
//                        and the asset fails.
//   overwrite == true   an existing file is removed and written again, and a
//                        file in the way of a folder is removed (boot repair -f).

#include <stdint.h>
#include <stdbool.h>

#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>
#include <kernel/util/tar.h>
#include <kernel/boot/x86/boot_image.h>

#include <kernel/console/print.h>


// Write `bytes` to `path`, creating it if missing.
//
// Create it, then size it before writing: writes don't reliably extend a
// file, so the file is truncated to `size` first (same sequence the resource
// manager and dwm_desktop_layout_save use). On success the file is left with
// read and write permission, and execute with `executable`.
static inline bool boot_write_file(const char* path, const uint8_t* bytes, uint32_t size, bool executable) {
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

    uint8_t perm = VFS_PERMISSION_READ | VFS_PERMISSION_WRITE;
    if (executable)
        perm |= VFS_PERMISSION_EXECUTE;
    return vfs_set_permissions(path, perm);
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
// Dropping
//

// Counts for one drop pass. `written` is files created or replaced, `kept`
// is files left alone (already there, not overwriting), `failed` is errors.
typedef struct {
    uint32_t written;
    uint32_t kept;
    uint32_t failed;
} BootAssetStats;

// Make `dir` a folder the system can read and write. An existing folder
// only gets its permissions fixed. A file standing in the way is removed
// with `overwrite`, otherwise the folder cannot be made and this fails.
static bool boot_asset_make_dir(const char* dir, bool overwrite) {
    if (vfs_directory_check(dir)) {
        uint8_t perm = 0;
        if (!vfs_get_permissions(dir, &perm))
            return false;
        uint8_t want = (uint8_t)(perm | VFS_PERMISSION_READ | VFS_PERMISSION_WRITE);
        if (want != perm && !vfs_set_permissions(dir, want))
            return false;
        return true;
    }

    if (vfs_exists(dir)) {
        if (!overwrite) {
            print("  ! file in the way (use force): ");
            print(dir);
            print("\n");
            return false;
        }
        if (!vfs_remove(dir))
            return false;
    }

    return vfs_mkdir(dir);
}

// mkdir -p for the parent folders of `path`, leaving its first `skip`
// characters (the device root, which already exists) alone. "<root>/a/b/f"
// makes <root>/a and <root>/a/b. `path` is modified while walking and
// restored before returning.
static bool boot_asset_make_dirs(char* path, uint32_t skip, bool overwrite) {
    uint32_t len = (uint32_t)strlen(path);
    for (uint32_t i = skip + 1; i < len; i++) {
        if (path[i] != '/')
            continue;
        path[i] = '\0';
        bool ok = boot_asset_make_dir(path, overwrite);
        path[i] = '/';
        if (!ok)
            return false;
    }
    return true;
}

// "<root>/<rel>" into out, with every folder above the file created. A
// leading '/' on `rel` is ignored. False (out empty) if it does not fit or
// a parent folder could not be made.
static bool boot_asset_path(char* out, uint32_t cap, const char* root,
                            const char* rel, bool overwrite) {
    out[0] = '\0';
    while (*rel == '/')
        rel++;
    if (!boot_join_path(out, cap, root, rel))
        return false;

    // Start the walk at the separator joined in after the root
    uint32_t skip = (uint32_t)strlen(root);
    if (skip > 0 && root[skip - 1] == '/')
        skip--;
    if (!boot_asset_make_dirs(out, skip, overwrite)) {
        out[0] = '\0';
        return false;
    }
    return true;
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

// The asset archive stage 2 loaded, or NULL (*size = its length)
static inline const uint8_t* boot_archive_get(uint32_t* size) {
    const uint8_t* archive = boot_assets_get(size);
    if (archive == NULL || !tar_check(archive, *size))
        return NULL;
    return archive;
}

// Extract every file and folder of the asset archive to <root>/<path>.
// Files count in `stats`, folders only when they fail. A damaged archive
// stops the walk at the damage (one failure); what came before stays.
// False if there is no archive at all.
static bool boot_drop_archive(const char* root, bool overwrite, BootAssetStats* stats) {
    uint32_t size = 0;
    const uint8_t* archive = boot_archive_get(&size);
    if (archive == NULL)
        return false;

    TarReader reader;
    tar_open(&reader, archive, size);

    // Static, like boot.h's sector buffers: commands run one at a time
    static struct TarEntry entry;
    TarResult result;
    while ((result = tar_next(&reader, &entry)) == TAR_ENTRY) {
        if (entry.type != TAR_TYPE_FILE && entry.type != TAR_TYPE_DIR)
            continue;   // Links, devices, ...: nothing the system uses

        char path[256];
        if (!boot_asset_path(path, sizeof(path), root, entry.name, overwrite)) {
            stats->failed++;
            continue;
        }

        if (entry.type == TAR_TYPE_DIR) {
            if (!boot_asset_make_dir(path, overwrite))
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

        if (boot_write_file(path, entry.data, entry.size, (entry.mode & TAR_MODE_EXEC) != 0))
            stats->written++;
        else
            stats->failed++;
    }

    if (result == TAR_BROKEN) {
        print("  ! asset archive is damaged, extraction stopped\n");
        stats->failed++;
    }
    return true;
}

#endif

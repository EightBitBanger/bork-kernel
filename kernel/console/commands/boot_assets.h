#ifndef CONSOLE_CMD_BOOT_ASSETS_H
#define CONSOLE_CMD_BOOT_ASSETS_H

// Assets that "boot install" / "boot repair" write to disk.
//
// All of them come from the asset archive: a ustar file (bin/assets.tar,
// built by build-assets.sh from assets.txt) that stage 2 reads off the boot
// disk behind the kernel and hands over in memory (boot_assets_get). Every
// file in it is written verbatim: ELF executables, maps, fonts (.ttf) and
// images (.png), exactly as they are in the source tree.
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
// Folder policies: the archive may hold ".policy" at its root (written by
// build-assets.sh from "sys/  policy=admin" lines), one "<folder> <policy>"
// per line, parents first. It is never written to disk. After the drop,
// boot_apply_policies puts each folder and everything in it in that group
// policy, with the policy's permissions as their security mask, making the
// folder if it is missing. Install and every repair do it, so a policy
// changed by hand inside such a folder is set back.
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

#include <kernel/security/policy.h>

#include <kernel/console/print.h>

// The policy list's name in the archive (see the top)
#define BOOT_ASSET_POLICY_FILE  ".policy"


// Write `bytes` to `path`, creating it if missing.
//
// Create it, then size it before writing: writes don't reliably extend a
// file, so the file is truncated to `size` first (same sequence
// dwm_desktop_layout_save uses). On success the file is left with
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
        if (strcmp(entry.name, BOOT_ASSET_POLICY_FILE) == 0)
            continue;   // The policy list (boot_apply_policies)

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

//
// Folder policies
//

// Deepest nesting below a policy folder that is walked. Anything deeper
// counts as a failure.
#define BOOT_POLICY_MAX_DEPTH   16

typedef struct {
    uint32_t folders;   // Lines of the list applied
    uint32_t objects;   // Files and folders put in a policy
    uint32_t failed;
} BootPolicyStats;

// Put `path` and everything in it in `policy` with the policy's permissions
// as the mask. `path` (cap bytes) is extended while walking and restored.
static void boot_policy_tree(char* path, uint32_t cap, const Policy* policy,
                             uint32_t depth, BootPolicyStats* stats) {
    if (vfs_set_policy(path, policy->id) && vfs_set_security(path, policy->permissions)) {
        stats->objects++;
    } else {
        print("  ! cannot set policy: ");
        print(path);
        print("\n");
        stats->failed++;
    }

    if (!vfs_directory_check(path))
        return;
    if (depth >= BOOT_POLICY_MAX_DEPTH) {
        print("  ! too deep for a policy: ");
        print(path);
        print("\n");
        stats->failed++;
        return;
    }

    // Static: only used before the recursive call, and commands run one at
    // a time
    static char name[VFS_NAME_MAX + 1];
    uint32_t length = (uint32_t)strlen(path);
    uint32_t count  = vfs_directory_get_item_count(path);
    for (uint32_t i = 0; i < count; i++) {
        if (!vfs_directory_get_item(path, i, name, sizeof(name)))
            continue;
        if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        // "<path>/<name>", written in place
        if (!boot_join_path(path + length, cap - length, "", name)) {
            path[length] = '\0';
            stats->failed++;
            continue;
        }
        boot_policy_tree(path, cap, policy, depth + 1, stats);
        path[length] = '\0';
    }
}

// The folder of one list line is relative to home, with no "." or ".."
// component and no empty one
static bool boot_policy_folder_ok(const char* folder) {
    if (folder[0] == '\0' || folder[0] == '/')
        return false;
    const char* part = folder;
    while (*part != '\0') {
        const char* end = part;
        while (*end != '\0' && *end != '/')
            end++;
        uint32_t n = (uint32_t)(end - part);
        if (n == 0 || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.'))
            return false;
        part = (*end == '/') ? end + 1 : end;
    }
    return true;
}

// One line of the list: "<folder> <policy>"
static void boot_policy_line(const char* root, const char* line, BootPolicyStats* stats) {
    char folder[200];
    char name[32];
    uint32_t i = 0, f = 0, n = 0;

    while (line[i] == ' ' || line[i] == '\t') i++;
    if (line[i] == '\0')
        return;     // Blank
    while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t') {
        if (f + 1 >= sizeof(folder)) { stats->failed++; return; }
        folder[f++] = line[i++];
    }
    folder[f] = '\0';
    while (line[i] == ' ' || line[i] == '\t') i++;
    while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t') {
        if (n + 1 >= sizeof(name)) { stats->failed++; return; }
        name[n++] = line[i++];
    }
    name[n] = '\0';

    // A trailing '/' is the folder itself
    while (f > 0 && folder[f - 1] == '/')
        folder[--f] = '\0';

    const Policy* policy = policy_find_name(name);
    if (!boot_policy_folder_ok(folder) || policy == NULL) {
        print("  ! bad policy line: ");
        print(line);
        print("\n");
        stats->failed++;
        return;
    }

    // Made if missing (and the folders above it), as the drop would
    static char path[VFS_PATH_MAX];
    if (!boot_asset_path(path, sizeof(path), root, folder, false) ||
        !boot_asset_make_dir(path, false)) {
        print("  ! cannot make ");
        print(folder);
        print(" for its policy\n");
        stats->failed++;
        return;
    }

    boot_policy_tree(path, sizeof(path), policy, 0, stats);
    stats->folders++;
}

// Apply the archive's policy list to <root>. True with nothing to do (no
// archive, or no list in it); false if any line failed. *listed is set when
// the archive has a list.
static bool boot_apply_policies(const char* root, BootPolicyStats* stats, bool* listed) {
    stats->folders = 0;
    stats->objects = 0;
    stats->failed  = 0;
    *listed = false;

    uint32_t size = 0;
    const uint8_t* archive = boot_archive_get(&size);
    if (archive == NULL)
        return true;

    TarReader reader;
    tar_open(&reader, archive, size);
    static struct TarEntry entry;
    bool found = false;
    while (tar_next(&reader, &entry) == TAR_ENTRY) {
        if (entry.type == TAR_TYPE_FILE && strcmp(entry.name, BOOT_ASSET_POLICY_FILE) == 0) {
            found = true;
            break;
        }
    }
    if (!found)
        return true;
    *listed = true;

    // Line by line, in order: parents come first, so a folder inside one
    // with its own line ends up in its own policy
    char line[256];
    uint32_t length = 0;
    for (uint32_t i = 0; i <= entry.size; i++) {
        char c = (i < entry.size) ? (char)entry.data[i] : '\n';
        if (c == '\r')
            continue;
        if (c != '\n') {
            if (length + 1 < sizeof(line))
                line[length] = c;
            length++;
            continue;
        }
        if (length >= sizeof(line)) {
            print("  ! policy line too long\n");
            stats->failed++;
        } else {
            line[length] = '\0';
            boot_policy_line(root, line, stats);
        }
        length = 0;
    }
    return stats->failed == 0;
}

#endif

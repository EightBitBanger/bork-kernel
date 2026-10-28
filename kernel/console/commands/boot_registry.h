#ifndef CONSOLE_CMD_BOOT_REGISTRY_H
#define CONSOLE_CMD_BOOT_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/kernel.h>
#include <kernel/registry/registry.h>
#include <kernel/util/string.h>
#include <kernel/vfs/vfs.h>

//
// Registry defaults "boot install" and "boot repair" write into
// <root>/sys/hkr, the hive the kernel loads as hkey_root at boot.
//
// Today: the desktop's context menus, read by the DWM (programs/dwm/src/
// menus.h). Each menu is a key under software/shell/menus, one subkey per
// line named "00", "01", ... (the DWM sorts them by name), each with a
// "label" and a "command" string:
//
//     software/shell/menus/desktop/00   label "Paste"   command "paste"
//     software/shell/menus/icon/00      label "Open"    command "open"
//
// The lines are the DWM's built-in defaults (default_desktop_lines and
// default_icon_lines in its main.c): keep the two in step.
//
// A menu that already has lines is the user's and is kept; only a missing
// or empty one is written. With `force` (repair -f) every menu goes back to
// its defaults. Everything else in the hive is left as it is.
//
// When <root> is the disk the system booted from, its hive is the live
// hkey_root: that is changed in memory (under the registry lock) and saved,
// so the desktop sees the menus without a restart. Any other disk's hive
// is read from its file, changed and written back.
//

#define BOOT_REGISTRY_FILE      "/sys/hkr"
#define BOOT_REGISTRY_MENUS     "software/shell/menus"

typedef struct {
    const char* label;
    const char* command;
} BootMenuLine;

static const BootMenuLine boot_menu_desktop[] = {
    { "Paste",        "paste" },
    { "",             "separator" },
    { "New folder",   "newfolder" },
    { "New file",     "newfile" },
    { "",             "separator" },
    { "Select all",   "selectall" },
    { "Refresh",      "refresh" },
    { "",             "separator" },
    { "Quit desktop", "quit" },
};

static const BootMenuLine boot_menu_icon[] = {
    { "Open",         "open" },
    { "",             "separator" },
    { "Cut",          "cut" },
    { "Copy",         "copy" },
    { "Delete",       "delete" },
    { "Rename",       "rename" },
    { "",             "separator" },
    { "Select all",   "selectall" },
    { "Refresh",      "refresh" },
    { "",             "separator" },
    { "Format...",    "format" },
    { "Properties",   "properties" },
};

static const struct {
    const char*         name;
    const BootMenuLine* lines;
    uint32_t            count;
} boot_menus[] = {
    { "desktop", boot_menu_desktop, sizeof(boot_menu_desktop) / sizeof(boot_menu_desktop[0]) },
    { "icon",    boot_menu_icon,    sizeof(boot_menu_icon) / sizeof(boot_menu_icon[0]) },
};

#define BOOT_MENU_COUNT (sizeof(boot_menus) / sizeof(boot_menus[0]))

typedef struct {
    uint32_t written;   // Menus set to their defaults
    uint32_t kept;      // Menus that already had lines
    uint32_t failed;
} BootRegistryStats;

// "<a><b>" into out. False if it does not fit.
static bool boot_registry_join(char* out, size_t size, const char* a, const char* b) {
    if (strlen(a) + strlen(b) + 1 > size)
        return false;
    strcpy(out, a);
    strcat(out, b);
    return true;
}

static void boot_registry_problem(const char* what, const char* path) {
    print("  ! ");
    print(what);
    print(path);
    print("\n");
}

// Write menu `m`'s default lines into `menu` (emptied first). False when out
// of memory, with the menu possibly half written.
static bool boot_registry_write_menu(struct RegistryKey* menu, uint32_t m) {
    const uint16_t perms = REGISTRY_PERMISSION_READ | REGISTRY_PERMISSION_WRITE;
    registry_clear_key(menu);
    for (uint32_t i = 0; i < boot_menus[m].count; i++) {
        // "00" .. "99": they sort in the order written
        char name[3] = { (char)('0' + (i / 10) % 10), (char)('0' + i % 10), '\0' };
        struct RegistryKey* line = registry_create_key(menu, name, perms);
        if (line == NULL ||
            !registry_set_string(line, "label", perms, boot_menus[m].lines[i].label) ||
            !registry_set_string(line, "command", perms, boot_menus[m].lines[i].command))
            return false;
    }
    return true;
}

// Fill in the defaults in `hive` (see the top). *changed: something was
// written.
static void boot_registry_fill(struct RegistryHive* hive, bool force, BootRegistryStats* stats, bool* changed) {
    const uint16_t perms = REGISTRY_PERMISSION_READ | REGISTRY_PERMISSION_WRITE;
    for (uint32_t m = 0; m < BOOT_MENU_COUNT; m++) {
        char path[64];
        if (!boot_registry_join(path, sizeof(path), BOOT_REGISTRY_MENUS "/", boot_menus[m].name)) {
            stats->failed++;
            continue;
        }
        struct RegistryKey* menu = registry_open_key(hive->root, path, true, perms);
        if (menu == NULL) {
            stats->failed++;
            continue;
        }
        if (menu->child_keys != NULL && !force) {
            stats->kept++;
            continue;
        }
        *changed = true;
        if (boot_registry_write_menu(menu, m))
            stats->written++;
        else
            stats->failed++;
    }
}

// Write the registry defaults into <root>/sys/hkr (made if missing).
// False if anything failed: the hive file can't be read back or written,
// or a menu couldn't be filled in.
static bool boot_registry_defaults(const char* root, bool force, BootRegistryStats* stats) {
    stats->written = 0;
    stats->kept    = 0;
    stats->failed  = 0;

    char sys_dir[128], file[128];
    if (!boot_registry_join(sys_dir, sizeof(sys_dir), root, "/sys") ||
        !boot_registry_join(file, sizeof(file), root, BOOT_REGISTRY_FILE)) {
        stats->failed++;
        return false;
    }
    // The asset drops make sys; without an archive it may not be there yet
    if (!vfs_directory_check(sys_dir) && !vfs_mkdir(sys_dir)) {
        boot_registry_problem("cannot create ", sys_dir);
        stats->failed++;
        return false;
    }

    registry_lock();

    bool live = (hkey_root.root != NULL && strcmp(registry_hive_path(&hkey_root), file) == 0);
    struct RegistryHive local = { NULL };
    struct RegistryHive* hive = live ? &hkey_root : &local;

    // Another disk's hive: what is on it now, else a new one as
    // registry_hive_initiate() would make it (a damaged file is replaced)
    if (!live && (!vfs_exists(file) || !registry_hive_import(&local, file) || local.root == NULL)) {
        if (local.root != NULL)
            registry_free_key(local.root);
        local.root = registry_create_key(NULL, "hkey_root", REGISTRY_PERMISSION_READ | REGISTRY_PERMISSION_WRITE);
        if (local.root == NULL) {
            registry_unlock();
            stats->failed++;
            return false;
        }
    }

    bool changed = false;
    boot_registry_fill(hive, force, stats, &changed);

    bool saved = true;
    if (changed || !vfs_exists(file))
        saved = registry_hive_export(hive, file);
    if (!saved) {
        boot_registry_problem("cannot write ", file);
        stats->failed++;
    }

    if (!live)
        registry_free_key(local.root);
    registry_unlock();

    return stats->failed == 0;
}

#endif

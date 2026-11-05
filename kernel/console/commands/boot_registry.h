#ifndef CONSOLE_CMD_BOOT_REGISTRY_H
#define CONSOLE_CMD_BOOT_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/kernel.h>
#include <kernel/registry/registry.h>
#include <kernel/security/policy.h>
#include <kernel/util/string.h>
#include <kernel/vfs/vfs.h>

//
// Registry defaults "boot install" and "boot repair" write into
// <root>/sys/hkr, the hive the kernel loads as hkey_root at boot.
//
// Today: the desktop's context menus, the system's file types, the
// services the desktop starts and the group policies.
//
// Menus are read by the DWM (programs/dwm/src/menus.h). Each menu is a key under software/shell/menus, one subkey per
// line named "00", "01", ... (the DWM sorts them by name), each with a
// "label" and a "command" string:
//
//     software/shell/menus/desktop/00   label "Paste"   command "paste"
//     software/shell/menus/icon/00      label "Open"    command "open"
//
// The lines are the DWM's built-in defaults (default_desktop_lines and
// default_icon_lines in its main.c): keep the two in step.
//
// File types are read by the DWM and the explorer through libc's
// filetype_lookup() (programs/libc/include/os/filetype.h): one key per
// extension under "filetype", with a "type" and an "icon" string and
// optionally a "fallback" icon for when that image isn't installed:
//
//     filetype/ttf    type "Font"     icon "icon_font"     fallback "icon_file"
//     filetype/drv    type "Driver"   icon "icon_driver"   fallback "icon_system"
//     filetype/sh     type "Script"   icon "icon_script"   open "console --script"
//
// "open" names the program that opens the type's files, with the
// arguments that go before the file (the explorer and the desktop run
// "<home>/bin/console --script <file>" for a script). A repair that keeps
// an existing key still adds a missing "open" to it, so scripts open on a
// disk installed before there were openers; set it to "" to turn one off.
//
// A file is matched by its extension first, then by what its header says
// it is (a program with no extension is still filetype/elf). The lines are
// libc's built-in table (default_items in programs/libc/src/filetype.c):
// keep the two in step.
//
// Services are read by the DWM (programs/dwm/src/services.h): one string
// value per service under "system/services", named after it, holding the
// program's path below home:
//
//     system/services    netsrv "sys/services/netsrv"
//
// What starts with the desktop is a way to hook every login, so the key is
// only ever changed by an admin: hkey_root takes SYS_SEC_ADMIN to write
// (sys_registry.h) and is readable by all. The line is the DWM's built-in
// default (services_defaults in its main.c): keep the two in step.
//
// Group policies are the kernel's own table (kernel/security/policy.h),
// listed read-only under "system/policy". They are always written as the
// kernel has them, whatever was there (the kernel corrects the live hive
// at every boot as well):
//
//     system/policy/admin    id "2"   permissions "0x000003ef"   ...
//
// A menu that already has lines, or a file type key or the services key
// that already has values, is the user's and is kept; only a missing or
// empty one is written. A kept menu still gets the changes made to the
// defaults since (boot_menu_retired, boot_menu_added below): a line taken
// out, a line put in. With `force` (repair -f) every one goes back to its
// defaults, but file types and services the user added are left.
// Everything else in the hive is left as it is.
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
    { "Open console here", "console" },
};

static const BootMenuLine boot_menu_icon[] = {
    { "Open",         "open" },
    { "Open console here", "console" },
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
    { "Unmount",      "unmount" },
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

// Changes to the default menus since they were first written, applied to a
// menu that is kept (the user's) as well, so "boot install" / "boot repair"
// bring an existing system up to date without undoing the user's own lines:
//
//   retired   lines with this command are taken out (and a separator left
//             at the end of the menu with them)
//   added     a line with this command goes in, right after the line with
//             command `after` (at the end without one), unless the menu
//             already has a line with this command anywhere
typedef struct {
    const char* menu;
    const char* command;
} BootMenuRetired;

static const BootMenuRetired boot_menu_retired[] = {
    { "desktop", "quit" },      // Shut down is a start menu button now
};

typedef struct {
    const char* menu;
    const char* label;
    const char* command;
    const char* after;
} BootMenuAdded;

static const BootMenuAdded boot_menu_added[] = {
    { "icon", "Unmount", "unmount", "format" },
};

#define BOOT_MENU_RETIRED_COUNT (sizeof(boot_menu_retired) / sizeof(boot_menu_retired[0]))
#define BOOT_MENU_ADDED_COUNT   (sizeof(boot_menu_added) / sizeof(boot_menu_added[0]))

#define BOOT_REGISTRY_FILETYPES "filetype"

typedef struct {
    const char* ext;            // Key name: the extension, lower case
    const char* type;
    const char* icon;
    const char* fallback;       // NULL: none
    const char* open;           // What opens it ("console --script"), NULL: none
} BootFileType;

static const BootFileType boot_filetypes[] = {
    // Programs and system files
    { "elf",  "Program",  "icon_executable", "icon_system"   },
    { "drv",  "Driver",   "icon_driver",     "icon_system"   },
    { "kdrv", "Driver",   "icon_driver",     "icon_system"   },
    { "sh",   "Script",   "icon_script",     "icon_document", "console --script" },

    // Fonts
    { "ttf",  "Font",     "icon_font",       "icon_file"     },
    { "otf",  "Font",     "icon_font",       "icon_file"     },
    { "ttc",  "Font",     "icon_font",       "icon_file"     },

    // Text
    { "txt",  "Text",     "icon_document",   "icon_file"     },
    { "md",   "Text",     "icon_document",   "icon_file"     },
    { "c",    "C source", "icon_source",     "icon_file"     },
    { "h",    "C header", "icon_header",     "icon_file"     },
    { "pdf",  "PDF",      "icon_document",   "icon_file"     },

    // Images
    { "png",  "PNG",      "icon_image",      "icon_file"     },
    { "jpg",  "JPEG",     "icon_image",      "icon_file"     },
    { "jpeg", "JPEG",     "icon_image",      "icon_file"     },
    { "gif",  "GIF",      "icon_image",      "icon_file"     },
    { "bmp",  "BMP",      "icon_image",      "icon_file"     },

    // Archives and audio
    { "zip",  "ZIP",      "icon_archive",    "icon_file"     },
    { "gz",   "gzip",     "icon_archive",    "icon_file"     },
    { "tar",  "tar",      "icon_archive",    "icon_file"     },
    { "wav",  "WAV",      "icon_audio",      "icon_file"     },
};

#define BOOT_FILETYPE_COUNT (sizeof(boot_filetypes) / sizeof(boot_filetypes[0]))

#define BOOT_REGISTRY_SERVICES  "system/services"

typedef struct {
    const char* name;           // Value name: the service
    const char* path;           // Its program, below home
} BootService;

static const BootService boot_services[] = {
    { "netsrv", "sys/services/netsrv" },    // The Wi-Fi icon on the taskbar
};

#define BOOT_SERVICE_COUNT (sizeof(boot_services) / sizeof(boot_services[0]))

typedef struct {
    uint32_t written;   // Menus, file types and services set to their defaults
    uint32_t kept;      // Menus, file types and services that were there already
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

// The line of `menu` whose command is `command`, NULL if none
static struct RegistryKey* boot_registry_menu_line(struct RegistryKey* menu, const char* command) {
    char text[32];
    for (struct RegistryKey* line = menu->child_keys; line != NULL; line = line->next) {
        if (registry_get_string(line, "command", text, sizeof(text)) && strcmp(text, command) == 0)
            return line;
    }
    return NULL;
}

// The line that sorts last (the DWM orders lines by key name), NULL if none
static struct RegistryKey* boot_registry_menu_last(struct RegistryKey* menu) {
    struct RegistryKey* last = NULL;
    for (struct RegistryKey* line = menu->child_keys; line != NULL; line = line->next) {
        if (last == NULL || strcmp(line->name, last->name) > 0)
            last = line;
    }
    return last;
}

// Bring kept menu `m` up to date (boot_menu_retired / boot_menu_added).
// True if anything changed; *failed when a line couldn't be written.
static bool boot_registry_update_menu(struct RegistryKey* menu, uint32_t m, bool* failed) {
    const uint16_t perms = REGISTRY_PERMISSION_READ | REGISTRY_PERMISSION_WRITE;
    bool changed = false;

    for (uint32_t r = 0; r < BOOT_MENU_RETIRED_COUNT; r++) {
        if (strcmp(boot_menu_retired[r].menu, boot_menus[m].name) != 0)
            continue;
        struct RegistryKey* line;
        bool removed = false;
        while ((line = boot_registry_menu_line(menu, boot_menu_retired[r].command)) != NULL) {
            char name[REGISTRY_MAX_NAME_LEN];
            strcpy(name, line->name);
            registry_remove_key(menu, name);
            removed = true;
        }
        // The separator that stood above it would now end the menu
        struct RegistryKey* last;
        while (removed && (last = boot_registry_menu_last(menu)) != NULL) {
            char text[16];
            if (!registry_get_string(last, "command", text, sizeof(text)) || strcmp(text, "separator") != 0)
                break;
            char name[REGISTRY_MAX_NAME_LEN];
            strcpy(name, last->name);
            registry_remove_key(menu, name);
        }
        changed = changed || removed;
    }

    for (uint32_t a = 0; a < BOOT_MENU_ADDED_COUNT; a++) {
        const BootMenuAdded* add = &boot_menu_added[a];
        if (strcmp(add->menu, boot_menus[m].name) != 0 || boot_registry_menu_line(menu, add->command) != NULL)
            continue;
        // A name that sorts right after the `after` line, before the next:
        // "11" -> "115" ("115" < "12"), then "1155", ... if that is taken
        struct RegistryKey* before = boot_registry_menu_line(menu, add->after);
        if (before == NULL)
            before = boot_registry_menu_last(menu);
        char name[REGISTRY_MAX_NAME_LEN];
        if (before == NULL)
            strcpy(name, "00");
        else
            strcpy(name, before->name);
        bool named = false;
        while (strlen(name) + 1 < sizeof(name)) {
            strcat(name, "5");
            if (registry_get_key(menu, name) == NULL) {
                named = true;
                break;
            }
        }
        struct RegistryKey* line = named ? registry_create_key(menu, name, perms) : NULL;
        if (line == NULL ||
            !registry_set_string(line, "label", perms, add->label) ||
            !registry_set_string(line, "command", perms, add->command)) {
            *failed = true;
            continue;
        }
        changed = true;
    }
    return changed;
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
            bool failed = false;
            if (boot_registry_update_menu(menu, m, &failed))
                *changed = true;
            if (failed)
                stats->failed++;
            else
                stats->kept++;
            continue;
        }
        *changed = true;
        if (boot_registry_write_menu(menu, m))
            stats->written++;
        else
            stats->failed++;
    }

    for (uint32_t f = 0; f < BOOT_FILETYPE_COUNT; f++) {
        const BootFileType* ft = &boot_filetypes[f];
        char path[64];
        if (!boot_registry_join(path, sizeof(path), BOOT_REGISTRY_FILETYPES "/", ft->ext)) {
            stats->failed++;
            continue;
        }
        struct RegistryKey* key = registry_open_key(hive->root, path, true, perms);
        if (key == NULL) {
            stats->failed++;
            continue;
        }
        if (key->values != NULL && !force) {
            // Kept, but given the opener a key from before them lacks
            if (ft->open != NULL && registry_get_value(key, "open") == NULL) {
                *changed = true;
                if (!registry_set_string(key, "open", perms, ft->open))
                    stats->failed++;
            }
            stats->kept++;
            continue;
        }
        *changed = true;
        registry_clear_key(key);
        if (registry_set_string(key, "type", perms, ft->type) &&
            registry_set_string(key, "icon", perms, ft->icon) &&
            (ft->fallback == NULL || registry_set_string(key, "fallback", perms, ft->fallback)) &&
            (ft->open == NULL || registry_set_string(key, "open", perms, ft->open)))
            stats->written++;
        else
            stats->failed++;
    }

    // Services: the whole key counts as one, like a menu
    struct RegistryKey* services = registry_open_key(hive->root, BOOT_REGISTRY_SERVICES, true, perms);
    if (services == NULL) {
        stats->failed++;
    } else if (services->values != NULL && !force) {
        stats->kept++;
    } else {
        *changed = true;
        bool ok = true;
        for (uint32_t s = 0; s < BOOT_SERVICE_COUNT; s++)
            ok = registry_set_string(services, boot_services[s].name, perms, boot_services[s].path) && ok;
        if (ok)
            stats->written++;
        else
            stats->failed++;
    }

    // Group policies: the kernel's, never the user's
    bool policy_failed = false;
    bool policy_written = policy_registry_publish(hive, &policy_failed);
    if (policy_written)
        *changed = true;
    if (policy_failed)
        stats->failed++;
    else if (policy_written)
        stats->written++;
    else
        stats->kept++;
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

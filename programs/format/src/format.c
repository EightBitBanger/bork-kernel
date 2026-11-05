/*
 * format: erase a storage device and give it an empty file system.
 *
 *     format [device] [-q] [-s <sector size>] [-f <file system>]
 *
 * Installed as <home>/sys/format. Opens a window to pick the device, the
 * file system (basefs, this system's own, or FAT32, which other systems
 * read: "-f fat32" or "-f fat"), the sector size (the allocation block:
 * 512, 1024, 2048 or 4096 bytes; for FAT32 the cluster size, or Automatic
 * to pick it by size as Windows does) and quick or full; the arguments only
 * preselect them. "device" is a name as listed in /mnt ("ata1", "usb0",
 * "ata0p1"), or its /mnt path.
 *
 *   whole disk   ("ata1") wiped from sector 0: boot sector, partition table
 *                and every partition on it go, and it gets one volume
 *   partition    ("ata0p1") formatted inside its own range
 *   quick (-q)   writes the file system's structures only. A whole disk
 *                still has its boot area, its last sectors and the head of
 *                every partition zeroed. Full format zeroes every sector
 *                first, which takes a while on a big device.
 *
 * Formatting needs administrator rights: Format asks the user for them
 * (os/security.h, sec_elevate) with a line saying exactly what will be
 * erased, formats, and gives the rights back. The kernel checks the request
 * again (SYS_FORMAT, os/disk.h). The device holding the system folders, and
 * the disk it is on, are listed but can't be picked; the kernel refuses
 * them too. Explorer windows and the desktop hear about the change.
 *
 * Exit code: 0 formatted, 1 failed, 2 bad arguments, 3 cancelled.
 *
 * Needs SEC_FILE_READ, SEC_GFX and SEC_IPC, and SEC_ELEVATE to ask for
 * admin (the desktop's launch mask has them all) unless it is admin
 * already. While the format runs the window does not repaint: the call
 * blocks until the device is done.
 */

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/disk.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/os.h>
#include <os/security.h>
#include <os/wm.h>
#include <os/wm_app.h>     // Not on the desktop: says so and exits before main()

#define MAX_DEVICES         WM_MENU_MAX_ITEMS     // All fit in the device menu
#define DEVICE_NAME_MAX     16                  // Terminator included (BLOCK_NAME_MAX)
#define DEFAULT_FS          "basefs"

static const uint32_t block_sizes[] = { 512, 1024, 2048, 4096 };
#define BLOCK_SIZE_COUNT    (sizeof(block_sizes) / sizeof(block_sizes[0]))

// The file systems the kernel formats. name is what SYS_FORMAT takes, alias
// what it may call it back ("fat" for fat32).
typedef struct {
    const char* name;
    const char* alias;
    const char* label;
} filesystem_t;

static const filesystem_t filesystems[] = {
    { "basefs", NULL,  "basefs (native)" },
    { "fat32",  "fat", "FAT32" },
};
#define FS_COUNT            (sizeof(filesystems) / sizeof(filesystems[0]))
#define FS_FAT32            1

// FAT32's smallest volume: 65526 clusters of 512 bytes and its FATs
#define FAT32_MIN_BYTES     (33ULL * 1024 * 1024)

// Layout (client coordinates)
#define WIN_W               360
#define WIN_H               236
#define PAD                 14
#define FIELD_X             118
#define FIELD_H             22
#define ROW_DEVICE          12
#define ROW_FS              42
#define ROW_BLOCK           72
#define ROW_QUICK           104
#define ROW_SUMMARY         132
#define ROW_WARNING         152
#define CHECK_SIZE          12
#define BUTTON_W            72
#define BUTTON_H            24

// Menu command ids
#define CMD_DEVICE          100
#define CMD_FS              200
#define CMD_BLOCK           300
#define CMD_BLOCK_AUTO      399

typedef struct {
    char          name[DEVICE_NAME_MAX];
    disk_format_t info;         // The default plan: sizes, whole disk, system
    bool          usable;       // false: the system device (listed, can't be picked)
} device_t;

typedef enum {
    ST_SETUP,                   // Picking
    ST_ASKING,                  // Waiting for the user's permission
    ST_RUNNING,                 // Formatting
} state_t;

static device_t devices[MAX_DEVICES];
static size_t   device_count;
static int      selected = -1;          // Index in devices, -1: none
static char     filesystem[DISK_FS_MAX] = DEFAULT_FS;
static uint32_t block_size;             // 0: the device's current one (else 512); FAT32: by size
static bool     quick;
static state_t  state = ST_SETUP;
static char     status[128];            // Last problem, shown in place of the summary
static bool     status_is_error;

static wm_window_t win;
static wm_theme_t  theme;
static int32_t     cw = 6, lh = 8;

//
// Helpers
//

static void set_status(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(bool error, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
    status_is_error = error;
}

// "128 MB", "1.5 GB", "512 KB"
static void format_size(uint64_t bytes, char* out, size_t size) {
    static const char* units[] = { "bytes", "KB", "MB", "GB", "TB" };
    unsigned unit = 0;
    uint64_t whole = bytes, tenth = 0;
    while (whole >= 1024 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        tenth = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        unit++;
    }
    if (unit == 0 || tenth == 0 || whole >= 100)
        snprintf(out, size, "%llu %s", (unsigned long long)whole, units[unit]);
    else
        snprintf(out, size, "%llu.%llu %s", (unsigned long long)whole, (unsigned long long)tenth, units[unit]);
}

// Block device names: lowercase letters and digits (what SYS_FORMAT takes)
static bool device_name_valid(const char* name) {
    size_t n = strlen(name);
    if (n == 0 || n >= DEVICE_NAME_MAX)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9')))
            return false;
    return true;
}

// Index in filesystems of a name or alias, -1: not one of them
static int find_filesystem(const char* name) {
    for (size_t i = 0; i < FS_COUNT; i++)
        if (strcmp(filesystems[i].name, name) == 0 ||
            (filesystems[i].alias != NULL && strcmp(filesystems[i].alias, name) == 0))
            return (int)i;
    return -1;
}

static bool is_fat(void) {
    return find_filesystem(filesystem) == FS_FAT32;
}

// "basefs (native)", "FAT32", or the name as is
static const char* filesystem_label(const char* name) {
    int index = find_filesystem(name);
    return (index >= 0) ? filesystems[index].label : name;
}

static int find_device(const char* name) {
    for (size_t i = 0; i < device_count; i++)
        if (strcmp(devices[i].name, name) == 0)
            return (int)i;
    return -1;
}

static const char* strip_mnt(const char* name) {
    if (strncmp(name, "/mnt/", 5) == 0)
        name += 5;
    return name;
}

static int add_device(const char* name);

// Every device in /mnt that can be formatted (or is the system one), in
// /mnt order. Partitions that aren't mounted can't be, and are left out.
static void scan_devices(void) {
    char keep[DEVICE_NAME_MAX] = "";
    if (selected >= 0)
        strlcpy(keep, devices[selected].name, sizeof(keep));
    device_count = 0;
    selected = -1;

    DIR* dir = opendir("/mnt");
    if (dir == NULL)
        return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && device_count < MAX_DEVICES) {
        if (!device_name_valid(entry->d_name))
            continue;
        device_t* d = &devices[device_count];
        memset(d, 0, sizeof(*d));
        strlcpy(d->name, entry->d_name, sizeof(d->name));
        disk_format_init(&d->info);
        if (disk_format_query(d->name, &d->info) == 0) {
            d->usable = true;
        } else if (errno == EACCES) {
            d->usable = false;          // The system device: shown, not offered
        } else {
            continue;
        }
        device_count++;
    }
    closedir(dir);

    if (keep[0] != '\0') {
        selected = find_device(keep);
        if (selected < 0)
            selected = add_device(keep);    // Picked from the command line, not in /mnt
    }
}

// A device not in /mnt: a whole disk whose partitions are mounted instead
// ("ahci0" with ahci0p1 in /mnt), named on the command line. Its index, or -1.
static int add_device(const char* name) {
    if (!device_name_valid(name) || device_count >= MAX_DEVICES)
        return -1;
    device_t* d = &devices[device_count];
    memset(d, 0, sizeof(*d));
    strlcpy(d->name, name, sizeof(d->name));
    disk_format_init(&d->info);
    if (disk_format_query(d->name, &d->info) == 0)
        d->usable = true;
    else if (errno != EACCES)
        return -1;
    return (int)device_count++;
}

// The sector size a file system starts with: FAT32 picks its cluster by
// size, basefs keeps the device's own
static void default_block_size(void) {
    if (is_fat())
        block_size = 0;
    else if (selected >= 0)
        block_size = devices[selected].info.block_size;
}

static void select_device(int index) {
    selected = index;
    status[0] = '\0';
    // Until one is picked
    if (selected >= 0)
        default_block_size();
}

// The request as picked now
static void current_request(disk_format_t* spec) {
    disk_format_init(spec);
    spec->block_size = block_size;
    spec->flags = quick ? DISK_FORMAT_QUICK : 0;
    strlcpy(spec->filesystem, filesystem, sizeof(spec->filesystem));
}

// Why the kernel turned the request down (EINVAL), as far as can be told
// from here: FAT32 has its own limits
static const char* invalid_reason(const device_t* d) {
    if (!is_fat())
        return "The file system doesn't take that sector size";
    if (d->info.device_sector_size != 0 && d->info.device_sector_size != 512)
        return "FAT32 needs a device with 512-byte sectors";
    if (d->info.device_bytes < FAT32_MIN_BYTES)
        return "Too small for FAT32: it needs at least 33 MB";
    if (block_size != 0)
        return "Cluster size too large for this device: try Automatic";
    return "Can't be formatted as FAT32";
}

static const char* format_error(int error) {
    switch (error) {
        case EBUSY:   return "Device busy: close files open on it (or wait for the other format)";
        case EACCES:  return "This device holds the system and can't be formatted here";
        case ENOENT:  return "The device is gone";
        case ENOSPC:  return "The device is too small for that";
        case EINVAL:  return "The file system doesn't take that sector size";
        case EIO:     return "The device failed. It may need formatting again";
        case EPERM:   return "Not allowed to format";
        default:      return strerror(error);
    }
}

//
// Drawing
//

static gfx_rect_t field_rect(int32_t row) {
    return gfx_rect(FIELD_X, row, WIN_W - FIELD_X - PAD, FIELD_H);
}

static gfx_rect_t check_rect(void) {
    return gfx_rect(FIELD_X, ROW_QUICK + (FIELD_H - CHECK_SIZE) / 2, CHECK_SIZE, CHECK_SIZE);
}

static gfx_rect_t quick_hit_rect(void) {
    return gfx_rect(FIELD_X - 4, ROW_QUICK, WIN_W - FIELD_X - PAD + 4, FIELD_H);
}

static gfx_rect_t cancel_rect(void) {
    return gfx_rect(WIN_W - PAD - BUTTON_W, WIN_H - PAD - BUTTON_H, BUTTON_W, BUTTON_H);
}

static gfx_rect_t format_rect(void) {
    gfx_rect_t r = cancel_rect();
    r.x -= BUTTON_W + 8;
    return r;
}

static bool in_rect(gfx_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static bool can_format(void) {
    return state == ST_SETUP && selected >= 0 && devices[selected].usable;
}

static void draw_button(gfx_surface_t* s, gfx_rect_t b, const char* label, uint32_t color) {
    gfx_fill_rect(s, b.x, b.y, b.w, b.h, theme.button.fill);
    gfx_draw_rect(s, b.x, b.y, b.w, b.h, theme.button.border);
    gfx_draw_text(s, b.x + (b.w - (int32_t)strlen(label) * cw) / 2, gfx_text_y(s, b.y, b.h), label, color);
}

// A drop-down field: the value and a "v" at the right
static void draw_field(gfx_surface_t* s, int32_t row, const char* label, const char* value, uint32_t value_color) {
    gfx_draw_text(s, PAD, gfx_text_y(s, row, FIELD_H), label, theme.client.text);
    gfx_rect_t f = field_rect(row);
    gfx_fill_rect(s, f.x, f.y, f.w, f.h, theme.edit.background);
    gfx_draw_rect(s, f.x, f.y, f.w, f.h, theme.edit.border);

    char text[64];
    size_t cols = (size_t)((f.w - 6 - 2 * cw) / cw);
    if (strlen(value) > cols && cols > 3)
        snprintf(text, sizeof(text), "%.*s..", (int)(cols - 2), value);
    else
        strlcpy(text, value, sizeof(text));
    gfx_draw_text(s, f.x + 6, gfx_text_y(s, f.y, f.h), text, value_color);
    gfx_draw_text(s, f.x + f.w - cw - 6, gfx_text_y(s, f.y, f.h), "v", theme.client.text_muted);
}

static void device_label(const device_t* d, char* out, size_t size) {
    char bytes[24];
    format_size(d->info.device_bytes, bytes, sizeof(bytes));
    snprintf(out, size, "%s  %s%s", d->name, bytes, d->usable ? "" : "  (system)");
}

// What will happen, in a line or two, under the fields. The last problem
// (status) takes the first line's place.
static void draw_summary(gfx_surface_t* s) {
    const device_t* d = (selected >= 0) ? &devices[selected] : NULL;
    char size[24], line[96];

    if (status[0] != '\0') {
        gfx_draw_text(s, PAD, ROW_SUMMARY, status, status_is_error ? theme.client.text_danger : theme.client.text_soft);
    } else if (d == NULL) {
        gfx_draw_text(s, PAD, ROW_SUMMARY, device_count ? "Pick a device to format" : "No storage devices found",
                      theme.client.text_muted);
    } else if (!d->usable) {
        gfx_draw_text(s, PAD, ROW_SUMMARY, "Holds the system folders: can't be formatted here", theme.client.text_muted);
    } else {
        format_size(d->info.device_bytes, size, sizeof(size));
        snprintf(line, sizeof(line), "%s %s, %s now", size,
                 (d->info.info & DISK_INFO_WHOLE_DISK) ? "disk" : "partition",
                 (d->info.info & DISK_INFO_FORMATTED) ? filesystem_label(d->info.filesystem) : "not formatted");
        gfx_draw_text(s, PAD, ROW_SUMMARY, line, theme.client.text_soft);
    }

    if (d == NULL || !d->usable)
        return;
    bool whole = (d->info.info & DISK_INFO_WHOLE_DISK) != 0;
    snprintf(line, sizeof(line), "Everything on %s will be erased%s", d->name, whole ? "," : ".");
    gfx_draw_text(s, PAD, ROW_WARNING, line, theme.client.text_danger);
    gfx_draw_text(s, PAD, ROW_WARNING + lh + 2,
                  whole ? "its partition table and partitions too." : "This can't be undone.",
                  theme.client.text_danger);
}

static void draw(void) {
    gfx_surface_t* s = wm_window_surface(win);
    if (s == NULL)
        return;
    cw = gfx_char_width(s);
    lh = gfx_line_height(s);
    gfx_clear(s, theme.client.background);

    if (state != ST_SETUP) {
        char line[96];
        const char* name = (selected >= 0) ? devices[selected].name : "";
        if (state == ST_ASKING) {
            snprintf(line, sizeof(line), "Asking for permission to format %s...", name);
            gfx_draw_text(s, PAD, 40, line, theme.client.text);
        } else {
            snprintf(line, sizeof(line), "Formatting %s...", name);
            gfx_draw_text(s, PAD, 40, line, theme.client.text);
            int32_t y = 40 + lh + 6;
            if (quick) {
                gfx_draw_text(s, PAD, y, "This takes a moment.", theme.client.text_muted);
            } else {
                gfx_draw_text(s, PAD, y, "A full format zeroes every sector,", theme.client.text_muted);
                y += lh + 2;
                gfx_draw_text(s, PAD, y, "which can take a while.", theme.client.text_muted);
            }
            gfx_draw_text(s, PAD, y + lh + 6, "Don't remove the device.", theme.client.text_muted);
        }
        wm_window_commit(win, NULL);
        return;
    }

    char value[64];
    if (selected >= 0)
        device_label(&devices[selected], value, sizeof(value));
    else
        strlcpy(value, device_count ? "(pick one)" : "(none)", sizeof(value));
    draw_field(s, ROW_DEVICE, "Device", value,
               (selected >= 0 && devices[selected].usable) ? theme.edit.text : theme.client.text_muted);

    draw_field(s, ROW_FS, "File system", filesystem_label(filesystem), theme.edit.text);

    if (is_fat() && block_size == 0)
        strlcpy(value, "Automatic", sizeof(value));
    else
        snprintf(value, sizeof(value), "%u bytes", (unsigned)(block_size ? block_size : block_sizes[0]));
    draw_field(s, ROW_BLOCK, is_fat() ? "Cluster size" : "Sector size", value, theme.edit.text);

    gfx_rect_t c = check_rect();
    gfx_fill_rect(s, c.x, c.y, c.w, c.h, theme.edit.background);
    gfx_draw_rect(s, c.x, c.y, c.w, c.h, theme.edit.border);
    if (quick)
        gfx_fill_rect(s, c.x + 3, c.y + 3, c.w - 6, c.h - 6, theme.client.accent);
    gfx_draw_text(s, c.x + c.w + 8, gfx_text_y(s, ROW_QUICK, FIELD_H), "Quick format", theme.client.text);

    draw_summary(s);

    draw_button(s, format_rect(), "Format", can_format() ? theme.client.text_danger : theme.client.text_muted);
    draw_button(s, cancel_rect(), "Cancel", theme.button.text);
    wm_window_commit(win, NULL);
}

//
// Menus
//

static void show_device_menu(void) {
    if (device_count == 0)
        return;
    wm_menu_item_t items[MAX_DEVICES];
    static char labels[MAX_DEVICES][48];
    for (size_t i = 0; i < device_count; i++) {
        device_label(&devices[i], labels[i], sizeof(labels[i]));
        items[i].label = labels[i];
        items[i].id    = CMD_DEVICE + (uint32_t)i;
        items[i].flags = (devices[i].usable ? 0 : WM_MENU_DISABLED) | ((int)i == selected ? WM_MENU_CHECKED : 0);
    }
    gfx_rect_t f = field_rect(ROW_DEVICE);
    wm_menu_show(win, f.x, f.y + f.h, items, device_count);
}

static void show_fs_menu(void) {
    wm_menu_item_t items[FS_COUNT];
    int current = find_filesystem(filesystem);
    for (size_t i = 0; i < FS_COUNT; i++) {
        items[i].label = filesystems[i].label;
        items[i].id    = CMD_FS + (uint32_t)i;
        items[i].flags = ((int)i == current) ? WM_MENU_CHECKED : 0;
    }
    gfx_rect_t f = field_rect(ROW_FS);
    wm_menu_show(win, f.x, f.y + f.h, items, FS_COUNT);
}

static void show_block_menu(void) {
    wm_menu_item_t items[BLOCK_SIZE_COUNT + 1];
    static char labels[BLOCK_SIZE_COUNT][24];
    size_t count = 0;
    bool fat = is_fat();
    if (fat) {
        items[count].label = "Automatic";
        items[count].id    = CMD_BLOCK_AUTO;
        items[count].flags = (block_size == 0) ? WM_MENU_CHECKED : 0;
        count++;
    }
    uint32_t current = block_size ? block_size : (fat ? 0 : block_sizes[0]);
    for (size_t i = 0; i < BLOCK_SIZE_COUNT; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%u bytes", (unsigned)block_sizes[i]);
        items[count].label = labels[i];
        items[count].id    = CMD_BLOCK + (uint32_t)i;
        items[count].flags = (block_sizes[i] == current) ? WM_MENU_CHECKED : 0;
        count++;
    }
    gfx_rect_t f = field_rect(ROW_BLOCK);
    wm_menu_show(win, f.x, f.y + f.h, items, count);
}

static void menu_command(uint32_t id) {
    if (id >= CMD_DEVICE && id < CMD_DEVICE + device_count) {
        if (devices[id - CMD_DEVICE].usable)
            select_device((int)(id - CMD_DEVICE));
    } else if (id >= CMD_BLOCK && id < CMD_BLOCK + BLOCK_SIZE_COUNT) {
        block_size = block_sizes[id - CMD_BLOCK];
        status[0] = '\0';
    } else if (id == CMD_BLOCK_AUTO) {
        block_size = 0;
        status[0] = '\0';
    } else if (id >= CMD_FS && id < CMD_FS + FS_COUNT) {
        bool was_fat = is_fat();
        strlcpy(filesystem, filesystems[id - CMD_FS].name, sizeof(filesystem));
        if (was_fat != is_fat())
            default_block_size();       // Sector sizes don't carry over to clusters
        status[0] = '\0';
    }
    draw();
}

//
// Formatting
//

// True once formatted. Otherwise status says why (or nothing, when the
// user just said no).
static bool run_format(void) {
    const device_t* d = &devices[selected];
    char name[DEVICE_NAME_MAX];
    strlcpy(name, d->name, sizeof(name));

    // The kernel's view of the request, checked before anyone is asked
    disk_format_t spec;
    current_request(&spec);
    if (disk_format_query(name, &spec) < 0) {
        set_status(true, "%s", (errno == EINVAL) ? invalid_reason(d) : format_error(errno));
        return false;
    }

    // Exactly what will go, in the prompt. The kernel adds who is asking.
    const char* unit = is_fat() ? "clusters" : "sectors";
    char size[24], reason[SYS_ELEVATE_REASON_MAX];
    format_size(spec.device_bytes, size, sizeof(size));
    snprintf(reason, sizeof(reason), "Erase everything on %s (%s%s) and format it as %s, %u-byte %s%s",
             name, size, (spec.info & DISK_INFO_WHOLE_DISK) ? ", whole disk" : "",
             filesystem_label(spec.filesystem), (unsigned)spec.block_size, unit, quick ? ", quick" : "");

    bool was_admin = sec_is_admin();
    state = ST_ASKING;
    draw();
    if (sec_elevate(reason) < 0) {
        int e = errno;
        state = ST_SETUP;
        switch (e) {
            case EACCES:    set_status(false, "%s", ""); break;   // Cancel at the prompt: nothing to say
            case ETIMEDOUT: set_status(false, "Not formatted: nobody answered"); break;
            case EBUSY:     set_status(true, "Asked too recently: try again in a few seconds"); break;
            case EPERM:     set_status(true, "This program may not ask for administrator rights"); break;
            default:        set_status(true, "Can't ask for permission: %s", strerror(e)); break;
        }
        return false;
    }

    state = ST_RUNNING;
    draw();
    current_request(&spec);
    int r = disk_format(name, &spec);
    int e = errno;
    if (!was_admin)
        sec_drop(SEC_ADMIN);        // Only for as long as it took
    state = ST_SETUP;

    char mount_path[8 + DEVICE_NAME_MAX];
    snprintf(mount_path, sizeof(mount_path), "/mnt/%s", name);
    wm_fs_changed(mount_path);      // Explorers and the desktop show it empty (or gone)

    if (r < 0) {
        const char* why = (e == EINVAL) ? invalid_reason(d) : format_error(e);
        set_status(true, "%s", why);
        wm_notify_ex(WM_NOTIFY_ERROR, "Format failed", why, WM_NOTIFY_DEFAULT_MS);
        scan_devices();
        return false;
    }

    char message[96];
    format_size(spec.capacity, size, sizeof(size));
    snprintf(message, sizeof(message), "%s: %s %s, %u-byte %s", name, size, filesystem_label(spec.filesystem),
             (unsigned)spec.block_size, unit);
    wm_notify("Formatted", message);
    return true;
}

//
// Main
//

static void default_theme(void) {
    theme.client.background = 0xFF141420; theme.client.background_danger = 0xFF100101;
    theme.client.text = 0xFFFFFFFF;       theme.client.text_value = 0xFF3FFF3F;
    theme.client.text_soft = 0xFFCCCCDD;  theme.client.text_muted = 0xFF8888AA;
    theme.client.text_danger = 0xFFC00404; theme.client.accent = 0xFF04C004;
    theme.button.fill = 0xFF1C1C2A;       theme.button.border = 0xFF444466;
    theme.button.text = 0xFF3FFF3F;
    theme.edit.background = 0xFF202020;   theme.edit.border = 0xFF404040;
    theme.edit.text = 0xFFFFFFFF;
}

static int usage(void) {
    wm_notify("format", "Usage: format [device] [-q] [-s <sector size>] [-f basefs|fat32]");
    return 2;
}

int main(int argc, char** argv) {
    const char* want_device = NULL;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "-q") == 0) {
            quick = true;
        } else if (strcmp(a, "-s") == 0 && i + 1 < argc) {
            char* end;
            unsigned long v = strtoul(argv[++i], &end, 10);
            bool ok = false;
            for (size_t k = 0; k < BLOCK_SIZE_COUNT; k++)
                ok = ok || (v == block_sizes[k]);
            if (*end != '\0' || !ok)
                return usage();
            block_size = (uint32_t)v;
        } else if (strcmp(a, "-f") == 0 && i + 1 < argc) {
            int index = find_filesystem(argv[++i]);
            if (index < 0)
                return usage();
            strlcpy(filesystem, filesystems[index].name, sizeof(filesystem));
        } else if (a[0] != '-' && want_device == NULL) {
            want_device = strip_mnt(a);
        } else {
            return usage();
        }
    }

    if (wm_get_theme(&theme) < 0)
        default_theme();

    uint32_t picked_block = block_size;
    scan_devices();
    if (want_device != NULL) {
        int index = find_device(want_device);
        if (index < 0)
            index = add_device(want_device);
        if (index < 0) {
            set_status(true, "No device \"%.40s\" that can be formatted", want_device);
        } else {
            select_device(index);
            if (!devices[index].usable)
                set_status(true, "%s holds the system folders: can't be formatted here", devices[index].name);
        }
    }
    if (picked_block != 0)
        block_size = picked_block;

    int32_t sw = 0, sh = 0;
    wm_screen_size(&sw, &sh);
    wm_window_desc_t desc = {
        .title  = "Format",
        .x      = (sw > WIN_W) ? (sw - WIN_W) / 2 : WM_POS_DEFAULT,
        .y      = (sh > WIN_H) ? (sh - WIN_H - 20) / 2 : WM_POS_DEFAULT,
        .width  = WIN_W,
        .height = WIN_H,
        .style  = 0,
    };
    win = wm_window_create(&desc);
    if (win == 0)
        return 1;
    draw();

    int result = 3;
    bool running = true;
    wm_event_t ev;
    while (running && wm_wait_event(&ev, -1) > 0) {
        switch (ev.type) {
            case WM_EVENT_CLOSE:
            case WM_EVENT_DESTROYED:
                running = false;
                break;
            case WM_EVENT_KEY:
                if (ev.key == KEY_ESCAPE)
                    running = false;
                break;
            case WM_EVENT_THEME_CHANGED:
                if (wm_get_theme(&theme) < 0)
                    default_theme();
                draw();
                break;
            case WM_EVENT_FS_CHANGED:
                // A device came or went
                scan_devices();
                draw();
                break;
            case WM_EVENT_MENU_COMMAND:
                menu_command(ev.param);
                break;
            case WM_EVENT_MOUSE_DOWN:
                if (ev.button != INPUT_BTN_LEFT)
                    break;
                if (in_rect(cancel_rect(), ev.x, ev.y)) {
                    running = false;
                } else if (in_rect(format_rect(), ev.x, ev.y)) {
                    if (can_format()) {
                        status[0] = '\0';
                        if (run_format()) {
                            result = 0;
                            running = false;
                        } else {
                            result = status_is_error ? 1 : 3;
                        }
                        draw();
                    }
                } else if (in_rect(field_rect(ROW_DEVICE), ev.x, ev.y)) {
                    scan_devices();
                    show_device_menu();
                } else if (in_rect(field_rect(ROW_FS), ev.x, ev.y)) {
                    show_fs_menu();
                } else if (in_rect(field_rect(ROW_BLOCK), ev.x, ev.y)) {
                    show_block_menu();
                } else if (in_rect(quick_hit_rect(), ev.x, ev.y)) {
                    quick = !quick;
                    draw();
                }
                break;
            default:
                break;
        }
    }

    wm_window_destroy(win);
    wm_disconnect();
    return result;
}

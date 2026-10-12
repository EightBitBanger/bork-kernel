// Desktop icon layout persistence.
//
// Icon positions are stored in <home>/usr/icons, next to usr/desktop.
// Every icon on the desktop (mounts, folders and files) is keyed by its full
// path, so one file covers icons from every mounted device.
//
// File format (little-endian, packed):
//
//   struct DesktopLayoutHeader   magic "ICON", version, entry count
//   struct DesktopLayoutEntry    x, y, path   (repeated `count` times)
//
// The header's count is authoritative: if the file was previously larger,
// any trailing bytes past the last entry are ignored on load.

#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/console/display.h>

#include <kernel/util/string.h>
#include <kernel/util/list.h>

#define DESKTOP_LAYOUT_VERSION      1
#define DESKTOP_LAYOUT_MAX_ENTRIES  1024
#define DESKTOP_LAYOUT_PATH_MAX     256

struct DesktopLayoutHeader {
    char     magic[4];      // "ICON"
    uint16_t version;
    uint16_t count;
} __attribute__((packed));

struct DesktopLayoutEntry {
    int16_t  x;
    int16_t  y;
    char     path[DWM_MAX_PATH_LEN];
} __attribute__((packed));

// Build "<home>/<suffix>". Returns false when no home device was found.
static bool layout_build_path(char* out, size_t size, const char* suffix) {
    struct LocalPaths paths;
    kernel_get_local_paths(&paths);

    if (paths.home[0] == '\0')
        return false;

    memset(out, '\0', size);
    strncpy(out, paths.home, size - 1);
    strncat(out, suffix, size - strlen(out) - 1);
    return true;
}

// Move an icon to (x, y), clamped to the screen, and repaint both spots
static void layout_move_icon(struct IconObject* icon, int x, int y) {
    int display_w = display_get_width();
    int display_h = display_get_height();

    // Same clamping rules as icon dragging (dwm_update_icon_dragging)
    if (x + icon->bounds_x < 0) x = -icon->bounds_x;
    if (x + icon->bounds_x + icon->bounds_w > display_w)
        x = display_w - icon->bounds_x - icon->bounds_w;
    if (y + icon->bounds_y < 0) y = -icon->bounds_y;
    if (y + icon->bounds_y + icon->bounds_h > display_h)
        y = display_h - icon->bounds_y - icon->bounds_h;

    if (icon->x == x && icon->y == y)
        return;

    dwm_invalidate_region(icon->x + icon->bounds_x, icon->y + icon->bounds_y,
                          icon->bounds_w, icon->bounds_h);
    icon->x = (uint16_t)x;
    icon->y = (uint16_t)y;
    dwm_invalidate_region(icon->x + icon->bounds_x, icon->y + icon->bounds_y,
                          icon->bounds_w, icon->bounds_h);
}

static struct IconObject* layout_find_icon(const char* path) {
    for (struct list_node* node = workspace.icon_head; node != NULL; node = node->next) {
        struct IconObject* icon = (struct IconObject*)node->data;
        if (strncmp(icon->path, path, DWM_MAX_PATH_LEN) == 0)
            return icon;
    }
    return NULL;
}

bool dwm_desktop_layout_load(void) {
    DWM_GUARD();
    char file_path[DESKTOP_LAYOUT_PATH_MAX];
    if (!layout_build_path(file_path, sizeof(file_path), "/usr/icons"))
        return false;

    if (!vfs_exists(file_path))
        return false;   // First boot: keep the default layout

    File file = vfs_open(file_path, VFS_OPEN_READ);
    if (file == VFS_INVALID_FILE)
        return false;

    uint32_t file_size = vfs_get_size(file);
    if (file_size < sizeof(struct DesktopLayoutHeader)) {
        vfs_close(file);
        return false;
    }

    uint8_t* buffer = (uint8_t*)malloc(file_size);
    if (buffer == NULL) {
        vfs_close(file);
        return false;
    }

    int32_t bytes_read = vfs_read(file, buffer, file_size);
    vfs_close(file);

    bool ok = false;
    struct DesktopLayoutHeader* header = (struct DesktopLayoutHeader*)buffer;

    if (bytes_read == (int32_t)file_size &&
        header->magic[0] == 'I' && header->magic[1] == 'C' &&
        header->magic[2] == 'O' && header->magic[3] == 'N' &&
        header->version == DESKTOP_LAYOUT_VERSION &&
        header->count <= DESKTOP_LAYOUT_MAX_ENTRIES &&
        sizeof(struct DesktopLayoutHeader) +
            (uint32_t)header->count * sizeof(struct DesktopLayoutEntry) <= file_size) {

        struct DesktopLayoutEntry* entries =
            (struct DesktopLayoutEntry*)(buffer + sizeof(struct DesktopLayoutHeader));

        for (uint16_t i = 0; i < header->count; i++) {
            struct DesktopLayoutEntry* entry = &entries[i];
            entry->path[DWM_MAX_PATH_LEN - 1] = '\0';

            // Entries for items that no longer exist are simply skipped
            struct IconObject* icon = layout_find_icon(entry->path);
            if (icon != NULL)
                layout_move_icon(icon, entry->x, entry->y);
        }
        ok = true;
    }

    free(buffer);
    return ok;
}

bool dwm_desktop_layout_save(void) {
    DWM_GUARD();
    char file_path[DESKTOP_LAYOUT_PATH_MAX];
    char usr_path[DESKTOP_LAYOUT_PATH_MAX];
    if (!layout_build_path(file_path, sizeof(file_path), "/usr/icons"))
        return false;
    if (!layout_build_path(usr_path, sizeof(usr_path), "/usr"))
        return false;

    // usr normally exists already (it holds usr/desktop)
    if (!vfs_directory_check(usr_path)) 
        return false;

    uint32_t count = 0;
    for (struct list_node* node = workspace.icon_head; node != NULL; node = node->next)
        count++;
    if (count > DESKTOP_LAYOUT_MAX_ENTRIES)
        count = DESKTOP_LAYOUT_MAX_ENTRIES;

    uint32_t total_size = sizeof(struct DesktopLayoutHeader) +
                          count * sizeof(struct DesktopLayoutEntry);

    uint8_t* buffer = (uint8_t*)malloc(total_size);
    if (buffer == NULL)
        return false;
    memset(buffer, 0, total_size);

    struct DesktopLayoutHeader* header = (struct DesktopLayoutHeader*)buffer;
    memcpy(header->magic, "ICON", 4);
    header->version = DESKTOP_LAYOUT_VERSION;
    header->count   = (uint16_t)count;

    struct DesktopLayoutEntry* entries =
        (struct DesktopLayoutEntry*)(buffer + sizeof(struct DesktopLayoutHeader));

    uint32_t i = 0;
    for (struct list_node* node = workspace.icon_head; node != NULL && i < count; node = node->next) {
        struct IconObject* icon = (struct IconObject*)node->data;
        entries[i].x = (int16_t)icon->x;
        entries[i].y = (int16_t)icon->y;
        strncpy(entries[i].path, icon->path, DWM_MAX_PATH_LEN - 1);
        entries[i].path[DWM_MAX_PATH_LEN - 1] = '\0';
        i++;
    }

    // Make sure the file exists (CREATE only creates it when it is missing)
    File file = vfs_open(file_path, VFS_OPEN_CREATE | VFS_OPEN_READ | VFS_OPEN_WRITE);
    if (file == VFS_INVALID_FILE) {
        free(buffer);
        return false;
    }
    vfs_close(file);

    // Size the file to exactly what is about to be written, BEFORE opening it
    // for the write. Writes do not reliably extend a file past the space it
    // already owns, so a layout that grew since the last save (new icons)
    // lost its tail: the newest icon's entry. Resizing first also drops stale
    // bytes when the layout shrinks.
    if (!vfs_truncate(file_path, total_size)) {
        free(buffer);
        return false;
    }

    file = vfs_open(file_path, VFS_OPEN_READ | VFS_OPEN_WRITE);
    if (file == VFS_INVALID_FILE) {
        free(buffer);
        return false;
    }

    vfs_seek(file, 0);
    int32_t written = vfs_write(file, buffer, total_size);
    uint32_t final_size = vfs_get_size(file);
    vfs_close(file);
    free(buffer);

    return written == (int32_t)total_size && final_size == total_size;
}

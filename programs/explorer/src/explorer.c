/*
 * Explorer: program entry, windows, folder contents and shared state.
 *
 *     explorer [path]       opens one window on path ("/" by default)
 *
 * One process can hold several explorer windows ("Open in New Window");
 * it exits when the last one closes.
 *
 * File icons come from the system's file types (os/filetype.h): the
 * registry's hkey_root/filetype/<extension> ("ttf" -> "icon_font"), else
 * what the file's header says it is (a program "icon_executable", a
 * driver "icon_driver"), whatever its name.
 *
 * Opening a file starts a program (os/process.h, needs SEC_EXEC): a type
 * with an opener in the registry opens with it (os/filetype.h: a ".sh"
 * script runs in "console --script"), else a file marked executable runs
 * itself (a console program in a console window, spawn_program), and
 * anything else opens in notepad.
 * Properties is prop, Format... (on a drive) is format. Helper programs
 * are looked for in <home>/sys, then <home>/bin (os_programpath). Either
 * way the program starts in the file's folder, with argv[1] = the file,
 * and gets at most the clearance in LAUNCH_MASK.
 *
 * Cut / Copy / Paste use the dwm's clipboard (os/wm.h), so they work across
 * explorer windows, programs and the desktop. Copy puts the file on it as
 * WM_CLIP_FILES; Paste takes text (saved as a new file) here, and files
 * through <home>/sys/fileop ("fileop paste <folder>"), which shows the
 * progress window and moves them after a cut. Delete (once confirmed) is
 * "fileop delete <path>". fileop says when it changed something
 * (WM_EVENT_FS_CHANGED) and every window re-reads its folder.
 *
 * Several items can be selected: Ctrl+click adds or takes one out, a
 * rectangle dragged over the empty grid takes in what it touches (Ctrl:
 * added to the selection), Ctrl+A takes everything. Cut, Copy, Delete and
 * dragging act on the whole selection.
 *
 * Drag and drop goes through the dwm (os/wm.h wm_drag_files): items
 * dragged out of a window can be let go on another explorer window, a
 * folder in one, the desktop or a folder icon there. Files dropped on a
 * window ("fileop move -r <src> <dst> ...", copy with Ctrl held or when
 * they come from another drive / partition) go into the folder under the
 * pointer, else the folder the window shows. Dragged
 * with the right button, the drop asks first: Move here / Copy here /
 * Cancel.
 *
 * Sort By (the folder's right-click menu) orders the contents by name,
 * type, size or date modified, ascending or descending, folders first
 * either way. The choice, and the size of the last window closed (new
 * windows open at it), are kept in the registry under EXPLORER_KEY.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/filetype.h>
#include <os/os.h>
#include <os/process.h>
#include <os/registry.h>
#include <os/resource.h>
#include <os/security.h>
#include <os/wm_app.h>     // Not on the desktop: says so and exits before main()

#include "internal.h"

// The dwm's built-in theme (src/dwm/theme.c); replaced by its live colors
wm_theme_t explorer_theme = {
    .desktop = { 0xFF000733, 0xFFFFFFFF, 0xC0000000, 0x5004C004, 0xFF04C004, 0x3004C004, 0xD004C004 },
    .frame   = { 0xFF2A2A2A, 0xFF6F6F6F, 0xFFEFEFEF, 0xFF007000, 0xFF001900, 0xFF505050, 0xFF101010 },
    .client  = { .background = 0xFF141420, .background_danger = 0xFF100101, .text = 0xFFFFFFFF,
                 .text_soft = 0xFFD0D0DF, .text_value = 0xFF3FFF3F, .text_muted = 0xFF8888AA,
                 .text_mount = 0xFFFDA008, .text_danger = 0xFFC00404, .accent = 0xFF04C004,
                 .divider_soft = 0xFF086008 },
    .button  = { 0xFF1C1C2A, 0xFF444466, 0xFF04C004, 0xFF3FFF3F, 0xFF04C004, 0xFF3FFF3F },
    .edit    = { 0xFF202020, 0xFF404040, 0xFF08F008, 0xFFF0F0F0, 0xFF2E5C2E },
    .menu    = { 0xE0222222, 0xE0444444, 0xE0111111, 0xE0777777, 0xF0E0E0E0 },
    .taskbar = { 0xFF000000, 0xFF00C000 },
};

static explorer_t* windows[MAX_WINDOWS];
static int         window_count;

static int     icons[ICON_COUNT] = { -1, -1, -1, -1, -1 };
static int     back_icon = -1;


//
// Shared state
//

int explorer_icon(int icon_index) {
    return (icon_index >= 0 && icon_index < ICON_COUNT) ? icons[icon_index] : -1;
}

// Images by name, each loaded once: a hit or a miss is remembered, so a
// folder of 256 files costs one load per type, not one per file
#define IMAGE_CACHE_MAX     32

static struct {
    char name[RESOURCE_NAME_MAX];
    int  handle;                    // -1: not installed
} image_cache[IMAGE_CACHE_MAX];
static int image_cache_count;

int explorer_image(const char* name) {
    if (name == NULL || name[0] == '\0')
        return -1;
    for (int i = 0; i < image_cache_count; i++)
        if (strcmp(image_cache[i].name, name) == 0)
            return image_cache[i].handle;
    int handle = resource_open_system(name);
    if (image_cache_count < IMAGE_CACHE_MAX) {
        strlcpy(image_cache[image_cache_count].name, name, sizeof(image_cache[0].name));
        image_cache[image_cache_count].handle = handle;
        image_cache_count++;
    }
    return handle;
}

// The icon for a file of type t: its own image, else its fallback
static int type_icon(const filetype_info_t* t) {
    int handle = explorer_image(t->icon);
    if (handle < 0 && t->fallback[0] != '\0')
        handle = explorer_image(t->fallback);
    return handle;
}

int explorer_back_icon(void)  { return back_icon; }

//
// Clipboard: the dwm's (os/wm.h), shared with every program. The explorer
// puts files there (WM_CLIP_FILES) and pastes files or text.
//

bool explorer_clip_set(const char* const* paths, size_t count, bool cut) {
    return wm_clip_set_files(paths, count, cut) == 0;
}

bool explorer_clip_can_paste(void) {
    wm_clip_info_t info;
    if (wm_clip_info(&info) < 0)
        return false;
    return info.type == WM_CLIP_FILES || info.type == WM_CLIP_TEXT;
}

// Rewrite the clipboard's file list with `edit` applied to every path. The
// item is replaced only when something changed; an empty list clears it.
typedef bool (*clip_edit_fn)(char* path, size_t size, const char* a, const char* b);

static void clip_edit(clip_edit_fn edit, const char* a, const char* b) {
    wm_clip_info_t info;
    char* list = wm_clip_get(&info);
    if (list == NULL)
        return;
    if (info.type != WM_CLIP_FILES) {
        free(list);
        return;
    }

    static const char* kept[64];            // Static: 32 KB is too much for the stack
    static char        paths[64][PATH_MAX];
    size_t      n = 0;
    bool        changed = false;
    char*       save = NULL;
    char*       line = strtok_r(list, "\n", &save);
    for (; line != NULL && n < 64; line = strtok_r(NULL, "\n", &save)) {
        strlcpy(paths[n], line, sizeof(paths[n]));
        if (edit(paths[n], sizeof(paths[n]), a, b)) {
            changed = true;
            if (paths[n][0] == '\0')
                continue;           // Dropped
        }
        kept[n] = paths[n];
        n++;
    }
    free(list);

    // More than fits here: leave a long list alone rather than cut it short
    if (!changed || line != NULL)
        return;
    if (n == 0)
        wm_clip_clear(info.serial);
    else
        wm_clip_set_files(kept, n, (info.flags & WM_CLIP_CUT) != 0);
}

// a renamed to b: paths at or below a move with it
static bool edit_renamed(char* path, size_t size, const char* a, const char* b) {
    if (!fs_is_inside(path, a))
        return false;
    char moved[PATH_MAX];
    size_t keep = strlen(a);
    if (strlen(b) + strlen(path + keep) >= sizeof(moved))
        return false;
    strlcpy(moved, b, sizeof(moved));
    strlcat(moved, path + keep, sizeof(moved));
    strlcpy(path, moved, size);
    return true;
}

// Paths that no longer exist go
static bool edit_gone(char* path, size_t size, const char* a, const char* b) {
    (void)size; (void)a; (void)b;
    if (fs_kind(path, NULL) >= 0)
        return false;
    path[0] = '\0';
    return true;
}

void explorer_clip_renamed(const char* old_path, const char* new_path) {
    clip_edit(edit_renamed, old_path, new_path);
}

void explorer_clip_prune(void) {
    clip_edit(edit_gone, NULL, NULL);
}

void explorer_notify(explorer_t* s, uint32_t ms, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->notice, sizeof(s->notice), fmt, ap);
    va_end(ap);
    s->notice_until = os_time_ms() + ms;
    s->dirty = true;
}

void explorer_load_theme(void) {
    wm_theme_t t;
    if (wm_get_theme(&t) == 0)
        explorer_theme = t;
    else
        fprintf(stderr, "explorer: theme: %s (using the default colors)\n", strerror(errno));
}

static void load_resources(void) {
    static const char* const names[ICON_COUNT] = {
        "icon_folder", "icon_file", "icon_document", "icon_system", "icon_storage",
    };
    for (int i = 0; i < ICON_COUNT; i++) {
        icons[i] = resource_open_system(names[i]);
        if (icons[i] < 0)
            fprintf(stderr, "explorer: %s: %s\n", names[i], strerror(errno));
    }
    back_icon = resource_open_system("ui_back");
    if (back_icon < 0)
        fprintf(stderr, "explorer: ui_back: %s\n", strerror(errno));
}

//
// Selection
//

void explorer_select_none(explorer_t* s) {
    memset(s->picked, 0, sizeof(s->picked));
    s->selected = -1;
    s->dirty = true;
}

void explorer_select_only(explorer_t* s, int index) {
    memset(s->picked, 0, sizeof(s->picked));
    if (index < 0 || index >= s->total_items)
        index = -1;
    else
        s->picked[index] = true;
    s->selected = index;
    s->dirty = true;
}

void explorer_select_all(explorer_t* s) {
    for (int i = 0; i < s->total_items; i++)
        s->picked[i] = true;
    if (s->selected < 0 && s->total_items > 0)
        s->selected = 0;
    s->dirty = true;
}

void explorer_toggle(explorer_t* s, int index) {
    if (index < 0 || index >= s->total_items)
        return;
    s->picked[index] = !s->picked[index];
    s->selected = index;
    s->dirty = true;
}

int explorer_picked_count(const explorer_t* s) {
    int n = 0;
    for (int i = 0; i < s->total_items; i++)
        n += s->picked[i];
    return n;
}

size_t explorer_selection_paths(const explorer_t* s, int index, bool files_only,
                                const char** paths, size_t max, char** block) {
    *block = NULL;
    bool group = index < 0 || (index < s->total_items && s->picked[index]);
    size_t count = 0, bytes = 0;
    for (int i = 0; i < s->total_items; i++) {
        if (group ? !s->picked[i] : i != index)
            continue;
        if (files_only && s->items[i].icon_index == ICON_STORAGE)
            continue;
        count++;
        bytes += strlen(s->full_path) + 1 + strlen(s->items[i].name) + 1;
    }
    if (count == 0)
        return 0;
    if (count > max)
        count = max;

    char* p = (char*)malloc(bytes);
    if (p == NULL)
        return 0;
    *block = p;
    size_t n = 0;
    for (int i = 0; i < s->total_items && n < count; i++) {
        if (group ? !s->picked[i] : i != index)
            continue;
        if (files_only && s->items[i].icon_index == ICON_STORAGE)
            continue;
        size_t room = strlen(s->full_path) + 1 + strlen(s->items[i].name) + 1;
        if (fs_join(s->full_path, s->items[i].name, p, room) < 0)
            continue;
        paths[n++] = p;
        p += strlen(p) + 1;
    }
    return n;
}

//
// Folder contents
//

bool explorer_item_path(const explorer_t* s, int index, char* out, size_t size) {
    if (index < 0 || index >= s->total_items)
        return false;
    return fs_join(s->full_path, s->items[index].name, out, size) == 0;
}

int explorer_find_item(const explorer_t* s, const char* name) {
    if (name == NULL || name[0] == '\0')
        return -1;
    for (int i = 0; i < s->total_items; i++)
        if (strcmp(s->items[i].name, name) == 0)
            return i;
    return -1;
}

//
// Settings (EXPLORER_KEY)
//

sort_key_t explorer_sort_key        = SORT_NAME;
bool       explorer_sort_descending = false;

static const char* const sort_names[] = { "name", "type", "size", "date" };
#define SORT_ORDER_ASCENDING    "ascending"
#define SORT_ORDER_DESCENDING   "descending"

void explorer_load_settings(void) {
    char text[16];
    if (os_reg_get_string(EXPLORER_KEY, "sort", text, sizeof(text)) == 0) {
        for (size_t i = 0; i < sizeof(sort_names) / sizeof(sort_names[0]); i++)
            if (strcasecmp(text, sort_names[i]) == 0)
                explorer_sort_key = (sort_key_t)i;
    }
    if (os_reg_get_string(EXPLORER_KEY, "order", text, sizeof(text)) == 0)
        explorer_sort_descending = strcasecmp(text, SORT_ORDER_DESCENDING) == 0;
}

// A whole number in a string value, false if missing or not one
static bool read_number(const char* name, int32_t* out) {
    char text[16];
    if (os_reg_get_string(EXPLORER_KEY, name, text, sizeof(text)) < 0)
        return false;
    char* end;
    long v = strtol(text, &end, 10);
    if (end == text || *end != '\0' || v <= 0 || v > 16384)
        return false;
    *out = (int32_t)v;
    return true;
}

static void write_number(const char* name, int32_t value) {
    char text[16];
    snprintf(text, sizeof(text), "%d", (int)value);
    os_reg_set_string(EXPLORER_KEY, name, text);
}

// The size new windows open at: the last one closed, kept on the screen
static void window_size(int32_t* w, int32_t* h) {
    *w = WINDOW_WIDTH;
    *h = WINDOW_HEIGHT;
    int32_t rw, rh;
    if (read_number("width", &rw) && read_number("height", &rh)) {
        *w = rw;
        *h = rh;
    }
    int32_t sw = 0, sh = 0;
    wm_screen_size(&sw, &sh);
    if (sw > 0 && *w > sw) *w = sw;
    if (sh > 0 && *h > sh) *h = sh;
    if (*w < WINDOW_MIN_WIDTH)  *w = WINDOW_MIN_WIDTH;
    if (*h < WINDOW_MIN_HEIGHT) *h = WINDOW_MIN_HEIGHT;
}

void explorer_save_size(const explorer_t* s) {
    if (s->win_width < WINDOW_MIN_WIDTH || s->win_height < WINDOW_MIN_HEIGHT)
        return;
    int32_t w, h;
    if (read_number("width", &w) && read_number("height", &h) &&
        w == s->win_width && h == s->win_height)
        return;                 // Unchanged: no need to write the hive again
    write_number("width", s->win_width);
    write_number("height", s->win_height);
}

void explorer_set_sort(explorer_t* s, sort_key_t key, bool descending) {
    if (key == explorer_sort_key && descending == explorer_sort_descending)
        return;
    explorer_sort_key = key;
    explorer_sort_descending = descending;
    explorer_refresh_all();     // Re-read and re-sort, selection kept by name

    if (os_reg_set_string(EXPLORER_KEY, "sort", sort_names[key]) < 0 ||
        os_reg_set_string(EXPLORER_KEY, "order",
                          descending ? SORT_ORDER_DESCENDING : SORT_ORDER_ASCENDING) < 0)
        explorer_notify(s, NOTICE_LONG_MS, "Sorted, but the choice could not be saved: %s",
                        strerror(errno));
}

static int compare_names(const struct Item* x, const struct Item* y) {
    int c = strcasecmp(x->name, y->name);
    return c != 0 ? c : strcmp(x->name, y->name);
}

static int compare_u32(uint32_t a, uint32_t b) {
    return (a > b) - (a < b);
}

// Folders (and devices) first, then files, each by the sort key (ties by
// name ignoring case); descending turns the key's order around
static int compare_items(const void* a, const void* b) {
    const struct Item* x = (const struct Item*)a;
    const struct Item* y = (const struct Item*)b;
    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;

    int c = 0;
    switch (explorer_sort_key) {
        case SORT_NAME: c = compare_names(x, y);                    break;
        case SORT_TYPE: c = strcasecmp(x->type, y->type);           break;  // "" for folders
        case SORT_SIZE: c = compare_u32(x->size, y->size);          break;  // 0 for folders
        case SORT_DATE: c = compare_u32(x->modified, y->modified);  break;
    }
    if (c != 0)
        return explorer_sort_descending ? -c : c;
    return compare_names(x, y);
}

// Where the path text turns into the mount color: just before the first
// segment that is a device root ("/mnt" + "/ata0p1/docs")
static void find_mount_split(explorer_t* s) {
    const char* path = s->full_path;
    size_t len = strlen(path);
    s->in_mount = false;
    s->mount_split = len;

    char prefix[PATH_MAX];
    for (size_t i = 2; i <= len; i++) {
        if (path[i] != '/' && path[i] != '\0')
            continue;
        memcpy(prefix, path, i);
        prefix[i] = '\0';
        if (fs_kind(prefix, NULL) == SYS_TYPE_MOUNT) {
            const char* slash = strrchr(prefix, '/');
            s->mount_split = (size_t)(slash - prefix);
            s->in_mount = true;
            return;
        }
    }
}

static void populate(explorer_t* s) {
    s->total_items = 0;
    memset(s->picked, 0, sizeof(s->picked));
    s->banding = false;
    s->truncated = false;
    find_mount_split(s);

    DIR* d = opendir(s->full_path);
    if (d == NULL) {
        explorer_notify(s, NOTICE_LONG_MS, "Can't read this folder: %s", strerror(errno));
        return;
    }

    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '\0' || strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (s->total_items == MAX_ITEMS) {
            s->truncated = true;
            break;
        }
        struct Item* item = &s->items[s->total_items];
        strlcpy(item->name, e->d_name, sizeof(item->name));

        char path[PATH_MAX];
        bool joined = fs_join(s->full_path, item->name, path, sizeof(path)) == 0;
        SysStat st = { 0 };
        int kind = joined ? fs_kind(path, &st) : -1;
        if (kind < 0)
            kind = (e->d_type == DT_DIR) ? SYS_TYPE_DIRECTORY : SYS_TYPE_FILE;

        item->is_dir = fs_kind_is_dir(kind);
        item->icon_index = (kind == SYS_TYPE_MOUNT) ? ICON_STORAGE
                         : item->is_dir             ? ICON_FOLDER
                         :                            ICON_FILE;
        item->icon = -1;
        item->type[0] = '\0';
        item->size = item->is_dir ? 0 : st.size;
        item->modified = st.modified;
        if (!item->is_dir && joined) {
            filetype_info_t t;
            filetype_lookup(path, &t);
            strlcpy(item->type, t.type, sizeof(item->type));
            item->icon = type_icon(&t);
        }
        s->total_items++;
    }
    closedir(d);

    qsort(s->items, (size_t)s->total_items, sizeof(s->items[0]), compare_items);
    if (s->truncated)
        explorer_notify(s, NOTICE_LONG_MS, "Showing the first %d items", MAX_ITEMS);
}

void explorer_drop_hint(explorer_t* s, const char* dir) {
    if (strcmp(s->drop_hint, dir) == 0)
        return;
    strlcpy(s->drop_hint, dir, sizeof(s->drop_hint));
    wm_set_drop_folder(s->handle, dir);
}

static void set_title(explorer_t* s) {
    if (strcmp(s->full_path, "/") == 0)
        strlcpy(s->window_title, "/", sizeof(s->window_title));
    else
        strlcpy(s->window_title, fs_basename(s->full_path), sizeof(s->window_title));
    if (s->handle != 0) {
        wm_window_set_title(s->handle, s->window_title);
        explorer_drop_hint(s, s->full_path);    // The drag plate says Copy from other drives
    }
}

bool explorer_navigate(explorer_t* s, const char* path, const char* select_name) {
    char abs[PATH_MAX];
    if (os_abspath(path, abs, sizeof(abs)) == NULL) {
        explorer_notify(s, NOTICE_LONG_MS, "Can't open %s: %s", path, strerror(errno));
        return false;
    }
    int kind = fs_kind(abs, NULL);
    if (kind < 0 || !fs_kind_is_dir(kind)) {
        explorer_notify(s, NOTICE_LONG_MS, "Can't open %s: %s", fs_basename(abs),
                        kind < 0 ? strerror(errno) : "not a folder");
        return false;
    }

    strlcpy(s->full_path, abs, sizeof(s->full_path));
    s->menu = MENU_NONE;
    s->renaming = false;
    s->scroll_y = 0;
    populate(s);
    set_title(s);
    explorer_select_only(s, explorer_find_item(s, select_name));
    explorer_ensure_visible(s, s->selected);
    s->dirty = true;
    return true;
}

void explorer_refresh(explorer_t* s) {
    char selected[NAME_MAX + 1] = "";
    if (s->selected >= 0 && s->selected < s->total_items)
        strlcpy(selected, s->items[s->selected].name, sizeof(selected));

    // The folder itself may be gone (deleted or moved from another window):
    // fall back to the nearest folder that still exists
    if (!fs_kind_is_dir(fs_kind(s->full_path, NULL))) {
        char up[PATH_MAX];
        strlcpy(up, s->full_path, sizeof(up));
        while (strcmp(up, "/") != 0 && !fs_kind_is_dir(fs_kind(up, NULL))) {
            char parent[PATH_MAX];
            fs_parent(up, parent, sizeof(parent));
            strlcpy(up, parent, sizeof(up));
        }
        explorer_navigate(s, up, NULL);
        return;
    }

    // The selection is kept by name across the re-read
    char (*kept)[NAME_MAX + 1] = NULL;
    int kept_count = explorer_picked_count(s);
    if (kept_count > 0)
        kept = malloc((size_t)kept_count * sizeof(*kept));
    int k = 0;
    for (int i = 0; kept != NULL && i < s->total_items; i++)
        if (s->picked[i])
            strlcpy(kept[k++], s->items[i].name, sizeof(kept[0]));

    populate(s);       // An open menu still applies: its item is found by name
    s->selected = explorer_find_item(s, selected);
    for (int i = 0; i < k; i++) {
        int at = explorer_find_item(s, kept[i]);
        if (at >= 0)
            s->picked[at] = true;
    }
    free(kept);
    if (s->renaming) {
        s->rename_index = explorer_find_item(s, s->rename_name);
        if (s->rename_index < 0)
            s->renaming = false;
    }
    explorer_clamp_scroll(s);
    s->dirty = true;
}

void explorer_refresh_all(void) {
    for (int i = 0; i < window_count; i++)
        explorer_refresh(windows[i]);
}

// What a program started from here may hold: an ordinary application's
// clearance, and never more than the explorer itself has (spawn_ex would
// refuse a bit we lack)
#define LAUNCH_MASK     (SEC_FILE | SEC_GFX | SEC_IPC | SEC_EXEC | SEC_ELEVATE)    // The "user" policy

// Start <program> from <home>/sys or <home>/bin with `count` arguments after
// argv[0], in `cwd`.
// Says why not.
bool explorer_run_system(explorer_t* s, const char* program, const char* const* extra, size_t count,
                         const char* cwd) {
    char run[PATH_MAX];
    if (os_programpath(program, run, sizeof(run)) == NULL) {
        explorer_notify(s, NOTICE_LONG_MS, "No %s: %s", program, strerror(errno));
        return false;
    }
    char* args[SPAWN_ARG_MAX + 1];
    if (count > SPAWN_ARG_MAX - 1) {
        explorer_notify(s, NOTICE_LONG_MS, "Can't start %s: too many arguments", program);
        return false;
    }
    args[0] = run;
    for (size_t i = 0; i < count; i++)
        args[1 + i] = (char*)extra[i];
    args[1 + count] = NULL;
    if (spawn_ex(run, args, cwd, sec_get() & LAUNCH_MASK) < 0) {
        int err = errno;
        explorer_notify(s, NOTICE_LONG_MS, "Can't start %s: %s", program,
                        err == EPERM ? "not allowed to start programs" : strerror(err));
        return false;
    }
    return true;
}

static bool launch_system(explorer_t* s, const char* program, const char* arg, const char* cwd) {
    const char* args[1] = { arg };
    return explorer_run_system(s, program, args, 1, cwd);
}

void explorer_open_file(explorer_t* s, const char* path) {
    SysStat st;
    bool file = fs_kind(path, &st) == SYS_TYPE_FILE;
    bool program = file && (st.permissions & SYS_PERM_EXECUTE);

    char dir[PATH_MAX];
    fs_parent(path, dir, sizeof(dir));

    // Its type names a program that opens it. One that runs the file (a
    // script: the console) is used only for a file marked executable, a
    // viewer (an image: paint) for any; anything else opens in notepad
    // below.
    filetype_info_t type;
    if (file && filetype_lookup(path, &type) == 0 && type.open[0] != '\0' &&
        (program || type.viewer)) {
        if (filetype_open(&type, path, dir, sec_get() & LAUNCH_MASK) < 0) {
            int err = errno;
            if (err == ECANCELED)
                return;
            explorer_notify(s, NOTICE_LONG_MS, "Can't open %s: %s", fs_basename(path),
                            err == EPERM ? "not allowed to start programs" : strerror(err));
            return;
        }
        explorer_notify(s, NOTICE_MS, "Opened %s", fs_basename(path));
        return;
    }

    if (!program) {
        if (launch_system(s, "notepad", path, dir))
            explorer_notify(s, NOTICE_MS, "Opened %s", fs_basename(path));
        return;
    }

    // A program whose file needs admin is started elevated: the
    // administrator prompt comes up first, naming it (os/process.h). We
    // wait here until the user answers.
    sec_mask_t mask = sec_get() & LAUNCH_MASK;
    bool elevate = program_needs_elevation(path, mask) == 1;
    if (elevate)
        mask |= SPAWN_ELEVATE;

    // A console program gets a console window (os/process.h)
    char* args[] = { (char*)path, NULL };
    if (spawn_program(path, args, dir, mask) < 0) {
        int err = errno;
        if (err == ECANCELED)
            return;         // The user said no at the prompt: nothing to tell them
        const char* why = strerror(err);
        if (err == EPERM)
            why = "not allowed to start programs";
        else if (elevate && err == EBUSY)
            why = "the administrator prompt is busy, try again in a moment";
        else if (elevate && err == ETIMEDOUT)
            why = "nobody answered the administrator prompt";
        else if (elevate && err == EAGAIN)
            why = "no desktop to ask for administrator permission";
        explorer_notify(s, NOTICE_LONG_MS, "Can't run %s: %s", fs_basename(path), why);
        return;
    }
    explorer_notify(s, NOTICE_MS, "Started %s", fs_basename(path));
}

// Edit: the file in notepad, whatever its type (a program or an image too)
void explorer_edit_file(explorer_t* s, const char* path) {
    char dir[PATH_MAX];
    fs_parent(path, dir, sizeof(dir));
    if (launch_system(s, "notepad", path, dir))
        explorer_notify(s, NOTICE_MS, "Editing %s", fs_basename(path));
}

void explorer_show_properties(explorer_t* s, const char* path) {
    char dir[PATH_MAX];
    fs_parent(path, dir, sizeof(dir));
    launch_system(s, "prop", path, dir);
}

// A drive: format asks for admin itself and refuses the system drive
void explorer_format(explorer_t* s, const char* path) {
    launch_system(s, "format", path, "/");
}

//
// Windows
//

static explorer_t* find_window(wm_window_t handle) {
    for (int i = 0; i < window_count; i++)
        if (windows[i]->handle == handle)
            return windows[i];
    return NULL;
}

explorer_t* explorer_open_window(const char* path) {
    if (window_count == MAX_WINDOWS) {
        errno = EMFILE;
        return NULL;
    }
    explorer_t* s = (explorer_t*)calloc(1, sizeof(explorer_t));
    if (s == NULL)
        return NULL;
    s->selected = -1;
    window_size(&s->win_width, &s->win_height);

    if (!explorer_navigate(s, path, NULL) && !explorer_navigate(s, "/", NULL)) {
        free(s);
        errno = ENOENT;
        return NULL;
    }

    wm_window_desc_t d = {
        .title = s->window_title,
        .x = WM_POS_DEFAULT, .y = WM_POS_DEFAULT,
        .width = s->win_width, .height = s->win_height,
        .min_width = WINDOW_MIN_WIDTH, .min_height = WINDOW_MIN_HEIGHT,
        .style = WM_STYLE_RESIZABLE | WM_STYLE_CONTEXT_MENU,   // Right clicks come here
    };
    s->handle = wm_window_create(&d);
    if (s->handle == 0) {
        int err = errno;
        free(s);
        errno = err;
        return NULL;
    }
    wm_window_get_size(s->handle, &s->win_width, &s->win_height);
    explorer_drop_hint(s, s->full_path);
    windows[window_count++] = s;

    explorer_draw(s);           // The first commit shows the window
    wm_window_focus(s->handle);
    return s;
}

void explorer_forget_window(explorer_t* s) {
    free(s->drop_list);
    for (int i = 0; i < window_count; i++) {
        if (windows[i] == s) {
            windows[i] = windows[--window_count];
            break;
        }
    }
    free(s);
}

void explorer_close_window(explorer_t* s) {
    explorer_save_size(s);      // The next window opens at this size
    wm_window_destroy(s->handle);
    explorer_forget_window(s);
}

//
// Main loop
//

// Milliseconds until the next status message runs out, -1 for none
static int32_t next_timeout(void) {
    uint64_t now = os_time_ms();
    int64_t best = -1;
    for (int i = 0; i < window_count; i++) {
        uint64_t until = windows[i]->notice_until;
        if (until == 0)
            continue;
        int64_t left = (until > now) ? (int64_t)(until - now) : 0;
        if (best < 0 || left < best)
            best = left;
    }
    return (int32_t)best;
}

static void expire_notices(void) {
    uint64_t now = os_time_ms();
    for (int i = 0; i < window_count; i++) {
        explorer_t* s = windows[i];
        if (s->notice_until != 0 && now >= s->notice_until) {
            s->notice_until = 0;
            s->notice[0] = '\0';
            s->dirty = true;
        }
    }
}

static void dispatch(const wm_event_t* ev) {
    if (ev->type == WM_EVENT_FS_CHANGED) {
        // fileop (or another program) changed files: show what is there now,
        // and drop clipboard paths that went away
        explorer_refresh_all();
        explorer_clip_prune();
        return;
    }
    if (ev->type == WM_EVENT_THEME_CHANGED) {
        explorer_load_theme();
        for (int i = 0; i < window_count; i++)
            windows[i]->dirty = true;
        return;
    }
    explorer_t* s = find_window(ev->window);
    if (s != NULL)
        explorer_handle_event(s, ev);
}

int main(int argc, char** argv) {
    const char* start = (argc > 1 && argv[1] != NULL && argv[1][0] != '\0') ? argv[1] : "/";

    load_resources();
    explorer_load_theme();
    explorer_load_settings();

    if (explorer_open_window(start) == NULL) {
        fprintf(stderr, "explorer: can't open a window: %s\n", strerror(errno));
        wm_disconnect();
        return 1;
    }

    while (window_count > 0) {
        wm_event_t ev;
        int r = wm_wait_event(&ev, next_timeout());
        if (r < 0)
            break;          // The dwm went away
        if (r > 0) {
            dispatch(&ev);
            // Take everything already queued before painting once
            while (window_count > 0 && (r = wm_wait_event(&ev, 0)) > 0)
                dispatch(&ev);
            if (r < 0)
                break;
        }
        expire_notices();
        for (int i = 0; i < window_count; i++)
            if (windows[i]->dirty)
                explorer_draw(windows[i]);
    }

    wm_disconnect();
    return 0;
}

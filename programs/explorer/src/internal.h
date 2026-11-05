#ifndef EXPLORER_INTERNAL_H
#define EXPLORER_INTERNAL_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os/filetype.h>
#include <os/gfx.h>
#include <os/syscall.h>
#include <os/ui.h>
#include <os/wm.h>

#include "explorer.h"

#define NOTICE_LEN          128

typedef enum { MENU_NONE, MENU_WINDOW, MENU_ITEM, MENU_DROP } menu_kind_t;

typedef enum {
    // Item menu
    ACT_OPEN,
    ACT_OPEN_NEW_WINDOW,
    ACT_CUT,
    ACT_COPY,
    ACT_RENAME,
    ACT_DELETE,
    ACT_FORMAT,
    ACT_ITEM_PROPERTIES,
    // Window menu
    ACT_REFRESH,
    ACT_NEW_FOLDER,
    ACT_NEW_FILE,
    ACT_PASTE,
    ACT_OPEN_CONSOLE,
    ACT_FOLDER_PROPERTIES,
    // After a right-button drop
    ACT_DROP_MOVE,
    ACT_DROP_COPY,
    ACT_DROP_CANCEL,
} action_t;

#define ACT_LAST    ACT_DROP_CANCEL

struct Item {
    char        name[NAME_MAX + 1];
    uint16_t    icon_index;         // ICON_*: what kind of entry it is
    bool        is_dir;             // Folder or device root
    int         icon;               // Files: resource handle from the file's type, -1: use icon_index
    char        type[FT_NAME_MAX];  // Files: the type's name (os/filetype.h), "" for folders
    uint32_t    size;               // Files: bytes, 0 for folders
};

struct ExplorerWindowState {
    wm_window_t handle;
    int32_t     win_width, win_height;
    bool        dirty;              // Needs a redraw

    char        full_path[PATH_MAX];            // Absolute, normalized
    char        window_title[MAX_TITLE_LEN];
    size_t      mount_split;        // Path text from here on is inside a device
    bool        in_mount;

    struct Item items[MAX_ITEMS];
    int         total_items;
    bool        truncated;          // The folder holds more than MAX_ITEMS

    int         selected;           // The focused item (keys, rename, open), -1: none
    bool        picked[MAX_ITEMS];  // Every selected item (Ctrl+click, the rectangle)

    // Selection rectangle: drag on the empty grid. Content coordinates
    // (y counts from the top of the grid, scrolled or not).
    bool        banding;
    bool        band_additive;      // Ctrl: added to what was selected
    int32_t     band_x0, band_y0, band_x1, band_y1;
    bool        band_base[MAX_ITEMS];
    int32_t     scroll_y;
    bool        back_hover;

    // Context menu (the dwm's): what it was opened on
    menu_kind_t menu;               // MENU_NONE once used
    char        context_name[NAME_MAX + 1];     // Item right-clicked, "" for the folder

    // In-place rename (os/ui.h edit box, the same one the desktop uses)
    bool        renaming;
    int         rename_index;
    char        rename_name[NAME_MAX + 1];      // The name being changed
    ui_edit_t   edit;
    bool        edit_dragging;      // Left button went down in the box: moving selects

    // Drag and drop: a left or right press on a selected item becomes a
    // drag of the selection once the pointer moves DRAG_THRESHOLD pixels
    // with the button held
    bool        press_armed;
    uint8_t     press_button;
    int         press_item;
    int32_t     press_x, press_y;
    int         reselect;           // Left click on a selected item: it alone, if no drag follows

    // A right-button drop, waiting for its Move / Copy menu
    char*       drop_list;          // malloc'd paths ('\n' between), NULL: none
    char        drop_dir[PATH_MAX];

    // Status bar message (stands in for dwm_notify)
    char        notice[NOTICE_LEN];
    uint64_t    notice_until;       // os_time_ms() deadline, 0: none
};

typedef struct ExplorerWindowState explorer_t;

//
// explorer.c: windows, folders, shared state
//

explorer_t* explorer_open_window(const char* path);
void        explorer_close_window(explorer_t* s);
void        explorer_forget_window(explorer_t* s);      // The dwm already destroyed it

bool explorer_navigate(explorer_t* s, const char* path, const char* select_name);
void explorer_refresh(explorer_t* s);
void explorer_refresh_all(void);
bool explorer_item_path(const explorer_t* s, int index, char* out, size_t size);
int  explorer_find_item(const explorer_t* s, const char* name);
void explorer_open_file(explorer_t* s, const char* path);
void explorer_show_properties(explorer_t* s, const char* path);     // <home>/sys/prop
void explorer_format(explorer_t* s, const char* path);              // <home>/sys/format
// <program> from <home>/sys or <home>/bin with arguments (argv[0] is filled in), started in `cwd`
bool explorer_run_system(explorer_t* s, const char* program, const char* const* args, size_t count,
                         const char* cwd);

void explorer_load_theme(void);
bool explorer_open_menu(explorer_t* s, int item, int32_t x, int32_t y);

void explorer_notify(explorer_t* s, uint32_t ms, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

int  explorer_icon(int icon_index);     // Resource handle, -1 if not loaded
int  explorer_image(const char* name);  // Any system image by name (cached), -1 if missing
int  explorer_back_icon(void);

// Selection (the picked items; `selected` is the focused one)
void explorer_select_none(explorer_t* s);
void explorer_select_only(explorer_t* s, int index);    // -1: none
void explorer_select_all(explorer_t* s);
void explorer_toggle(explorer_t* s, int index);         // Ctrl+click
int  explorer_picked_count(const explorer_t* s);
// The paths of the selection, or of `index` alone when it isn't selected:
// pointers into one malloc'd block in *block (free it). How many (0 with
// *block NULL when there are none or no memory). Devices left out when
// `files_only`.
size_t explorer_selection_paths(const explorer_t* s, int index, bool files_only,
                                const char** paths, size_t max, char** block);

// The system clipboard (the dwm's, os/wm.h), as the explorer uses it
bool explorer_clip_set(const char* const* paths, size_t count, bool cut);  // False with errno
bool explorer_clip_can_paste(void);                     // Files or text on it
void explorer_clip_renamed(const char* old_path, const char* new_path);   // Keep paths current
void explorer_clip_prune(void);                         // Drop paths that no longer exist

//
// procedure.c: input
//

void explorer_handle_event(explorer_t* s, const wm_event_t* ev);

//
// draw.c: painting, and the geometry the input code hit-tests against
//

void explorer_draw(explorer_t* s);

int     explorer_columns(const explorer_t* s);
void    explorer_item_rect(const explorer_t* s, int index, gfx_rect_t* out);
int     explorer_item_at(const explorer_t* s, int32_t x, int32_t y);
int32_t explorer_grid_bottom(const explorer_t* s);     // Where the status bar starts
int32_t explorer_max_scroll(const explorer_t* s);
void    explorer_clamp_scroll(explorer_t* s);
void    explorer_ensure_visible(explorer_t* s, int index);
int     explorer_rows_per_page(const explorer_t* s);
bool    explorer_back_hit(int32_t x, int32_t y);
void    explorer_place_rename(explorer_t* s);      // The rename box over its tile (s->edit.rect)

const char* explorer_action_label(action_t action);


//
// fsops.c / kind.c: file system helpers on the libc
//

int  fs_kind(const char* path, SysStat* out);   // SYS_TYPE_*, or -1 with errno
bool fs_kind_is_dir(int kind);
const char* fs_basename(const char* path);
int  fs_join(const char* dir, const char* name, char* out, size_t size);
void fs_parent(const char* path, char* out, size_t size);
bool fs_is_inside(const char* path, const char* dir);      // path is dir or below it
void fs_volume(const char* path, char* out, size_t size);  // The device root holding path, "/" if none
bool fs_same_volume(const char* a, const char* b);         // Both on the same drive / partition
int  fs_unique_path(const char* dir, const char* name, char* out, size_t size);

#endif

/*
 * Input: mouse, keyboard and window events for one explorer window, and
 * the actions behind the context menus.
 *
 * Mouse                               Keys
 *   click          select               arrows, Home/End, PgUp/PgDn  move
 *   Ctrl+click     add / take out       Enter       open
 *   drag the grid  selection rectangle  Backspace   parent folder
 *                  (Ctrl: adds to it)   Delete      delete the selection (asks first)
 *   double-click   open                 Ctrl+A      select everything
 *   right-click    the dwm's menu       Ctrl+C / Ctrl+X  the selection
 *   back button    parent folder
 *   drag an item   drag the selection (dwm drag and drop); with the right
 *                  button the drop asks Move here / Copy here / Cancel
 *   drop on it     move here / into the folder under the pointer (Ctrl, or
 *                  from another drive: copy)
 *   wheel          scroll the grid a row a notch (Ctrl: a page)
 *                                       Ctrl+C / Ctrl+X / Ctrl+V, Ctrl+R refresh
 *                                       Escape      close the dialog
 * While renaming (the shared edit box, os/ui.h): the name starts selected,
 * so typing replaces it. Left / Right (Ctrl: by word), Home / End and
 * clicks move the cursor; Shift or a drag selects; a double click selects
 * the whole name. Enter keeps, Escape cancels, clicking elsewhere keeps (as
 * the kernel version did).
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <os/input.h>
#include <os/os.h>
#include <os/process.h>

#include "internal.h"

//
// Navigation
//

static void go_up(explorer_t* s) {
    if (strcmp(s->full_path, "/") == 0)
        return;
    char parent[PATH_MAX], came_from[NAME_MAX + 1];
    strlcpy(came_from, fs_basename(s->full_path), sizeof(came_from));
    fs_parent(s->full_path, parent, sizeof(parent));
    explorer_navigate(s, parent, came_from);
}

static void open_item(explorer_t* s, int index) {
    char path[PATH_MAX];
    if (!explorer_item_path(s, index, path, sizeof(path)))
        return;
    if (s->items[index].is_dir)
        explorer_navigate(s, path, NULL);
    else
        explorer_open_file(s, path);
}

static void select_item(explorer_t* s, int index) {
    if (index >= s->total_items)
        index = s->total_items - 1;
    if (index < 0 && s->total_items > 0)
        index = 0;
    explorer_select_only(s, index);
    explorer_ensure_visible(s, index);
}

// What the keys act on: the focused item, else the first selected one
static int focus_or_first(const explorer_t* s) {
    if (s->selected >= 0)
        return s->selected;
    for (int i = 0; i < s->total_items; i++)
        if (s->picked[i])
            return i;
    return -1;
}

//
// Selection rectangle
//

static void band_apply(explorer_t* s) {
    int32_t x0 = s->band_x0 < s->band_x1 ? s->band_x0 : s->band_x1;
    int32_t x1 = s->band_x0 < s->band_x1 ? s->band_x1 : s->band_x0;
    int32_t y0 = s->band_y0 < s->band_y1 ? s->band_y0 : s->band_y1;
    int32_t y1 = s->band_y0 < s->band_y1 ? s->band_y1 : s->band_y0;
    for (int i = 0; i < s->total_items; i++) {
        gfx_rect_t t;
        explorer_item_rect(s, i, &t);
        int32_t ty = t.y + s->scroll_y;              // Content coordinates
        bool inside = x1 > x0 && y1 > y0 && t.x < x1 && t.x + t.w > x0 && ty < y1 && ty + t.h > y0;
        s->picked[i] = (s->band_additive && s->band_base[i]) || inside;
    }
    s->dirty = true;
}

static void band_start(explorer_t* s, int32_t x, int32_t y, bool additive) {
    s->banding = true;
    s->band_additive = additive;
    memcpy(s->band_base, s->picked, sizeof(s->band_base));
    s->band_x0 = s->band_x1 = x;
    s->band_y0 = s->band_y1 = y + s->scroll_y;
}

static void band_to(explorer_t* s, int32_t x, int32_t y) {
    // Past the top or bottom of the grid it scrolls, so the rectangle can
    // take in what isn't showing
    int32_t step = 0;
    if (y < GRID_TOP)
        step = y - GRID_TOP;
    else if (y > explorer_grid_bottom(s))
        step = y - explorer_grid_bottom(s);
    if (step != 0) {
        s->scroll_y += step / 2 != 0 ? step / 2 : step;
        explorer_clamp_scroll(s);
    }
    s->band_x1 = x;
    s->band_y1 = y + s->scroll_y;
    band_apply(s);
}

static void band_end(explorer_t* s) {
    s->banding = false;
    if (s->selected >= 0 && !s->picked[s->selected])
        s->selected = -1;
    for (int i = 0; i < s->total_items && s->selected < 0; i++)
        if (s->picked[i])
            s->selected = i;
    s->dirty = true;
}

//
// Rename
//

static void start_rename(explorer_t* s, int index) {
    if (index < 0 || index >= s->total_items)
        return;
    s->renaming = true;
    s->rename_index = index;
    explorer_select_only(s, index);
    strlcpy(s->rename_name, s->items[index].name, sizeof(s->rename_name));
    ui_edit_init(&s->edit, s->items[index].name, NAME_MAX, UI_EDIT_NAME);   // All selected
    s->edit_dragging = false;
    explorer_ensure_visible(s, index);
    s->dirty = true;
}

static void finish_rename(explorer_t* s, bool keep) {
    if (!s->renaming)
        return;
    s->renaming = false;
    s->edit_dragging = false;
    s->dirty = true;
    if (!keep || s->edit.len == 0 || strcmp(s->edit.text, s->rename_name) == 0)
        return;

    char path[PATH_MAX];
    if (fs_join(s->full_path, s->rename_name, path, sizeof(path)) < 0 ||
        os_rename(path, s->edit.text) < 0) {
        explorer_notify(s, NOTICE_LONG_MS, "Rename: name is invalid, too long or taken");
        return;
    }

    // Anything on the clipboard inside the renamed item moves with it
    char renamed[PATH_MAX];
    if (fs_join(s->full_path, s->edit.text, renamed, sizeof(renamed)) == 0)
        explorer_clip_renamed(path, renamed);

    char new_name[NAME_MAX + 1];
    strlcpy(new_name, s->edit.text, sizeof(new_name));
    wm_fs_changed(s->full_path);    // The desktop and other programs' windows
    explorer_refresh_all();
    select_item(s, explorer_find_item(s, new_name));
}

static void rename_key(explorer_t* s, const wm_event_t* ev) {
    switch (ui_edit_key(&s->edit, ev->key, ev->mods)) {
        case UI_EDIT_ACCEPT:    finish_rename(s, true);  break;
        case UI_EDIT_CANCEL:    finish_rename(s, false); break;
        case UI_EDIT_HANDLED:   s->dirty = true;         break;
        case UI_EDIT_IGNORED:                            break;     // Still the box's
    }
}

//
// Deleting
//

// fileop asks first (-i), then deletes with a progress window and Cancel;
// every window re-reads its folder when it says it is done (WM_EVENT_FS_CHANGED).
// The selection when `index` is in it, else that item; devices stay.
static void ask_delete(explorer_t* s, int index) {
    const char* args[SPAWN_ARG_MAX - 1];
    char* block;
    args[0] = "delete";
    args[1] = "-i";
    size_t max = sizeof(args) / sizeof(args[0]) - 2;
    size_t n = explorer_selection_paths(s, index, true, args + 2, max, &block);
    if (n == 0)
        return;
    if (n == max)
        explorer_notify(s, NOTICE_LONG_MS, "Deleting the first %u only", (unsigned)n);
    explorer_run_system(s, "fileop", args, 2 + n, s->full_path);
    free(block);
}

//
// Drag and drop: the dwm carries the paths (os/wm.h), fileop moves them
//

// The press on a selected item went far enough: hand the selection to the
// dwm, which drops it wherever the button comes up (here too, on a folder)
static void start_drag(explorer_t* s) {
    s->press_armed = false;
    s->reselect = -1;
    const char* paths[MAX_ITEMS];
    char* block;
    size_t n = explorer_selection_paths(s, s->press_item, false, paths, MAX_ITEMS, &block);
    if (n == 0)
        return;
    if (wm_drag_files(paths, n) < 0 && errno != EPERM)      // EPERM: the press is over already
        explorer_notify(s, NOTICE_LONG_MS, "Can't drag: %s", strerror(errno));
    free(block);
}

// Source / destination pairs for one fileop: its argv also holds the
// program, the verb and -r
#define DROP_MAX_PAIRS  ((SPAWN_ARG_MAX - 3) / 2)

// Move (copy) the '\n'-separated paths in `list` (cut up here) into `dir`
// with fileop, which renames taken names; files already there and folders
// dropped into themselves stay
static void transfer(explorer_t* s, char* list, const char* dir, bool copy) {
    static char targets[DROP_MAX_PAIRS][PATH_MAX];         // Static: too big for the stack
    const char* args[2 + 2 * DROP_MAX_PAIRS];
    size_t n = 2, pairs = 0, skipped = 0;
    args[0] = copy ? "copy" : "move";
    args[1] = "-r";

    char* save = NULL;
    for (char* src = strtok_r(list, "\n", &save); src != NULL; src = strtok_r(NULL, "\n", &save)) {
        char parent[PATH_MAX];
        fs_parent(src, parent, sizeof(parent));
        if (!copy && strcmp(parent, dir) == 0)
            continue;                               // Already here
        if (fs_is_inside(dir, src) || (!copy && fs_kind(src, NULL) == SYS_TYPE_MOUNT) ||
            pairs == DROP_MAX_PAIRS || fs_join(dir, fs_basename(src), targets[pairs], PATH_MAX) < 0) {
            skipped++;
            continue;
        }
        args[n++] = src;
        args[n++] = targets[pairs++];
    }

    if (pairs > 0 && explorer_run_system(s, "fileop", args, n, dir)) {
        explorer_notify(s, NOTICE_MS, "%s %u item%s to %s%s", copy ? "Copying" : "Moving", (unsigned)pairs,
                        pairs == 1 ? "" : "s", fs_basename(dir), skipped ? " (some left out)" : "");
    } else if (pairs == 0 && skipped > 0) {
        explorer_notify(s, NOTICE_LONG_MS, "Can't %s that there", copy ? "copy" : "move");
    }
}

// The dwm draws the menus: CONTEXT_MENU opens one, MENU_COMMAND brings the
// choice back. Ids are action_t + 1 (0 stays unused).
#define MENU_ID(a)  ((uint32_t)(a) + 1)

// A right-button drop: keep it and ask what to do (MENU_COMMAND finishes it)
static void ask_drop(explorer_t* s, char* list, const char* dir, int32_t x, int32_t y) {
    free(s->drop_list);
    s->drop_list = list;
    strlcpy(s->drop_dir, dir, sizeof(s->drop_dir));
    wm_menu_item_t items[] = {
        { explorer_action_label(ACT_DROP_MOVE),   MENU_ID(ACT_DROP_MOVE),   0 },
        { explorer_action_label(ACT_DROP_COPY),   MENU_ID(ACT_DROP_COPY),   0 },
        { NULL,                                   0,                        WM_MENU_SEPARATOR },
        { explorer_action_label(ACT_DROP_CANCEL), MENU_ID(ACT_DROP_CANCEL), 0 },
    };
    s->menu = MENU_DROP;
    if (wm_menu_show(s->handle, x, y, items, sizeof(items) / sizeof(items[0])) < 0) {
        s->menu = MENU_NONE;
        free(s->drop_list);
        s->drop_list = NULL;
    }
}

static void finish_drop(explorer_t* s, action_t a) {
    if (s->drop_list != NULL && a != ACT_DROP_CANCEL)
        transfer(s, s->drop_list, s->drop_dir, a == ACT_DROP_COPY);
    free(s->drop_list);
    s->drop_list = NULL;
}

// Any of the '\n'-separated paths in `list` on another drive than `dir`
static bool from_other_drive(const char* list, const char* dir) {
    char path[PATH_MAX];
    for (const char* at = list; *at != '\0'; ) {
        const char* end = strchr(at, '\n');
        size_t len = (end != NULL) ? (size_t)(end - at) : strlen(at);
        if (len > 0 && len < sizeof(path)) {
            memcpy(path, at, len);
            path[len] = '\0';
            if (!fs_same_volume(path, dir))
                return true;
        }
        if (end == NULL)
            break;
        at = end + 1;
    }
    return false;
}

// Files let go over the window: into the folder under the pointer, else
// the one shown. Moved within a drive, copied from another one (or with
// Ctrl held); a right drag asks first.
static void drop_files(explorer_t* s, const wm_event_t* ev) {
    char* list = wm_drop_get(ev, NULL);
    if (list == NULL) {
        if (errno != ENOENT)
            explorer_notify(s, NOTICE_LONG_MS, "Drop: %s", strerror(errno));
        return;
    }
    if (s->renaming)
        finish_rename(s, true);

    char dir[PATH_MAX];
    int item = explorer_item_at(s, ev->x, ev->y);
    if (item < 0 || !s->items[item].is_dir || !explorer_item_path(s, item, dir, sizeof(dir)))
        strlcpy(dir, s->full_path, sizeof(dir));

    if (ev->button == INPUT_BTN_RIGHT) {
        ask_drop(s, list, dir, ev->x, ev->y);       // Keeps the list
        return;
    }
    transfer(s, list, dir, (ev->mods & INPUT_MOD_CTRL) != 0 || from_other_drive(list, dir));
    free(list);
}

//
// Context menus
//

static void add_item(wm_menu_item_t* items, size_t* n, action_t a, bool enabled) {
    items[*n].label = explorer_action_label(a);
    items[*n].id    = MENU_ID(a);
    items[*n].flags = enabled ? 0 : WM_MENU_DISABLED;
    (*n)++;
}

static void add_separator(wm_menu_item_t* items, size_t* n) {
    items[*n].label = NULL;
    items[*n].id    = 0;
    items[*n].flags = WM_MENU_SEPARATOR;
    (*n)++;
}

bool explorer_open_menu(explorer_t* s, int item, int32_t x, int32_t y) {
    wm_menu_item_t items[WM_MENU_MAX_ITEMS];
    size_t n = 0;

    if (item >= 0) {
        bool device = (s->items[item].icon_index == ICON_STORAGE);
        bool many = s->picked[item] && explorer_picked_count(s) > 1;
        add_item(items, &n, ACT_OPEN, true);
        if (s->items[item].is_dir)
            add_item(items, &n, ACT_OPEN_NEW_WINDOW, true);
        add_separator(items, &n);
        add_item(items, &n, ACT_CUT, !device || many);
        add_item(items, &n, ACT_COPY, true);
        add_separator(items, &n);
        // A drive can't be renamed: Format takes Rename's place (the dwm
        // shows at most WM_MENU_MAX_ITEMS)
        if (device)
            add_item(items, &n, ACT_FORMAT, !many);
        else
            add_item(items, &n, ACT_RENAME, !many);
        add_item(items, &n, ACT_DELETE, !device || many);
        add_separator(items, &n);
        add_item(items, &n, ACT_ITEM_PROPERTIES, true);
        s->menu = MENU_ITEM;
        strlcpy(s->context_name, s->items[item].name, sizeof(s->context_name));
    } else {
        add_item(items, &n, ACT_REFRESH, true);
        add_separator(items, &n);
        add_item(items, &n, ACT_NEW_FOLDER, true);
        add_item(items, &n, ACT_NEW_FILE, true);
        add_separator(items, &n);
        add_item(items, &n, ACT_PASTE, explorer_clip_can_paste());
        add_separator(items, &n);
        add_item(items, &n, ACT_OPEN_CONSOLE, true);
        add_item(items, &n, ACT_FOLDER_PROPERTIES, true);
        s->menu = MENU_WINDOW;
        s->context_name[0] = '\0';
    }

    if (wm_menu_show(s->handle, x, y, items, n) < 0) {
        s->menu = MENU_NONE;
        return false;
    }
    return true;
}

static void create_new(explorer_t* s, bool folder) {
    char path[PATH_MAX];
    const char* base = folder ? "new_folder" : "new_file";
    if (fs_unique_path(s->full_path, base, path, sizeof(path)) < 0) {
        explorer_notify(s, NOTICE_LONG_MS, "Can't create %s: %s", base, strerror(errno));
        return;
    }
    int r;
    if (folder) {
        r = mkdir(path, 0777);
    } else {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        r = (fd < 0) ? -1 : close(fd);
    }
    if (r < 0) {
        explorer_notify(s, NOTICE_LONG_MS, "Can't create %s: %s", fs_basename(path), strerror(errno));
        return;
    }
    wm_fs_changed(path);            // The desktop and other programs' windows
    explorer_refresh_all();
    select_item(s, explorer_find_item(s, fs_basename(path)));
}

// WM_CLIP_FILES: fileop copies them here (or moves them, after a cut, and
// clears the clipboard once they all arrived), with its progress window
static void paste_files(explorer_t* s) {
    const char* args[2] = { "paste", s->full_path };
    explorer_run_system(s, "fileop", args, 2, s->full_path);
}

// WM_CLIP_TEXT: a new text file holding it
static void paste_text(explorer_t* s, const char* text, uint32_t size) {
    char path[PATH_MAX];
    if (fs_unique_path(s->full_path, "pasted.txt", path, sizeof(path)) < 0) {
        explorer_notify(s, NOTICE_LONG_MS, "Can't paste text: %s", strerror(errno));
        return;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    ssize_t wrote = (fd < 0) ? -1 : write(fd, text, size);
    int err = errno;
    if (fd >= 0)
        close(fd);
    if (wrote < 0 || (uint32_t)wrote != size) {
        explorer_notify(s, NOTICE_LONG_MS, "Can't paste text: %s", strerror(err));
        return;
    }
    explorer_notify(s, NOTICE_MS, "Text pasted as %s", fs_basename(path));
    explorer_refresh_all();
    select_item(s, explorer_find_item(s, fs_basename(path)));
}

static void paste(explorer_t* s) {
    wm_clip_info_t info;
    char* data = wm_clip_get(&info);
    if (data == NULL) {
        if (errno != ENOENT)
            explorer_notify(s, NOTICE_LONG_MS, "Clipboard: %s", strerror(errno));
        return;
    }
    if (info.type == WM_CLIP_FILES)
        paste_files(s);
    else if (info.type == WM_CLIP_TEXT)
        paste_text(s, data, info.size);
    else
        explorer_notify(s, NOTICE_MS, "Can't paste %s into a folder", wm_clip_type_name(info.type));
    free(data);
}

// `item`: what it applies to (item actions), -1 for the folder
static void run_action(explorer_t* s, action_t a, int item) {
    char path[PATH_MAX];
    bool have_path = explorer_item_path(s, item, path, sizeof(path));
    s->dirty = true;

    switch (a) {
        case ACT_OPEN:
            if (have_path) open_item(s, item);
            break;
        case ACT_OPEN_NEW_WINDOW:
            if (have_path && explorer_open_window(path) == NULL)
                explorer_notify(s, NOTICE_LONG_MS, "Can't open a new window: %s", strerror(errno));
            break;
        case ACT_CUT:
        case ACT_COPY: {
            // The selection when the item is in it (devices can't be cut)
            const char* paths[MAX_ITEMS];
            char* block;
            size_t n = have_path ? explorer_selection_paths(s, item, a == ACT_CUT, paths, MAX_ITEMS, &block) : 0;
            if (n == 0)
                break;
            const char* verb = (a == ACT_CUT) ? "Cut" : "Copied";
            if (!explorer_clip_set(paths, n, a == ACT_CUT))
                explorer_notify(s, NOTICE_LONG_MS, "Clipboard: %s", strerror(errno));
            else if (n == 1)
                explorer_notify(s, NOTICE_MS, "%s %s", verb, fs_basename(paths[0]));
            else
                explorer_notify(s, NOTICE_MS, "%s %u items", verb, (unsigned)n);
            free(block);
            break;
        }
        case ACT_RENAME:
            start_rename(s, item);
            break;
        case ACT_DELETE:
            ask_delete(s, item);
            break;
        case ACT_FORMAT:
            if (have_path) explorer_format(s, path);
            break;
        case ACT_ITEM_PROPERTIES:
            if (have_path) explorer_show_properties(s, path);
            break;
        case ACT_REFRESH:
            explorer_refresh(s);
            break;
        case ACT_NEW_FOLDER:
            create_new(s, true);
            break;
        case ACT_NEW_FILE:
            create_new(s, false);
            break;
        case ACT_PASTE:
            paste(s);
            break;
        case ACT_OPEN_CONSOLE: {
            // The console takes its starting folder as its argument
            const char* args[1] = { s->full_path };
            explorer_run_system(s, "console", args, 1, s->full_path);
            break;
        }
        case ACT_FOLDER_PROPERTIES:
            explorer_show_properties(s, s->full_path);
            break;
        case ACT_DROP_MOVE:
        case ACT_DROP_COPY:
        case ACT_DROP_CANCEL:
            finish_drop(s, a);
            break;
    }
}

static void menu_command(explorer_t* s, uint32_t id) {
    menu_kind_t kind = s->menu;
    s->menu = MENU_NONE;
    if (kind == MENU_NONE || id == 0 || id > MENU_ID(ACT_LAST))
        return;                     // Stale (the folder changed since it opened)
    action_t a = (action_t)(id - 1);
    bool drop_act = (a == ACT_DROP_MOVE || a == ACT_DROP_COPY || a == ACT_DROP_CANCEL);
    if ((kind == MENU_DROP) != drop_act)
        return;

    int item = -1;
    if (kind == MENU_ITEM) {
        item = explorer_find_item(s, s->context_name);   // The list may have been refreshed
        if (item < 0)
            return;
    }
    run_action(s, a, item);
}

static void context_menu(explorer_t* s, const wm_event_t* ev) {
    if (ev->y < GRID_TOP)
        return;                     // No menu over the toolbar
    if (s->renaming)
        finish_rename(s, true);
    int item = explorer_item_at(s, ev->x, ev->y);
    if (item >= 0 && !s->picked[item])
        explorer_select_only(s, item);
    explorer_open_menu(s, item, ev->x, ev->y);
}

//
// Events
//

static void mouse_down(explorer_t* s, const wm_event_t* ev) {
    bool right = (ev->button == INPUT_BTN_RIGHT);
    bool left = (ev->button == INPUT_BTN_LEFT);

    if (s->renaming) {
        explorer_place_rename(s);           // Where it was drawn
        if (ui_edit_click(&s->edit, ev->x, ev->y, ev->mods, ev->clicks)) {
            s->edit_dragging = left;        // Click into the box: cursor there
            s->dirty = true;
            return;
        }
        gfx_rect_t tile;
        explorer_item_rect(s, s->rename_index, &tile);
        bool inside = ev->x >= tile.x && ev->x < tile.x + tile.w && ev->y >= tile.y && ev->y < tile.y + tile.h;
        if (inside)
            return;                         // The rest of its tile: keep editing
        finish_rename(s, true);
        if (!right)
            return;
    }

    if (explorer_back_hit(ev->x, ev->y)) {
        if (left)
            go_up(s);
        return;
    }

    // A right click selects what it is on (a selected item keeps the rest
    // with it); its menu comes with CONTEXT_MENU. Moving with it held drags.
    int item = explorer_item_at(s, ev->x, ev->y);
    bool ctrl = (ev->mods & INPUT_MOD_CTRL) != 0;
    s->reselect = -1;
    if (right) {
        if (ev->y < GRID_TOP)
            return;
        if (item < 0)
            explorer_select_none(s);
        else if (!s->picked[item])
            explorer_select_only(s, item);
        else
            s->selected = item;
    } else if (left) {
        if (item >= 0 && ev->clicks >= 2 && !ctrl) {
            explorer_select_only(s, item);
            open_item(s, item);
            return;
        }
        if (item >= 0) {
            if (ctrl)
                explorer_toggle(s, item);
            else if (!s->picked[item])
                explorer_select_only(s, item);
            else {
                s->selected = item;
                s->reselect = item;         // Keep the group for a drag; alone on a plain click
                s->dirty = true;
            }
        } else if (ev->y >= GRID_TOP) {
            if (!ctrl)
                explorer_select_none(s);
            band_start(s, ev->x, ev->y, ctrl);
            return;
        }
    } else {
        return;
    }

    // Moving off with the button held drags the selection (a device alone
    // stays where it is)
    s->press_armed  = item >= 0 && s->picked[item] && s->items[item].icon_index != ICON_STORAGE;
    s->press_button = ev->button;
    s->press_item   = item;
    s->press_x      = ev->x;
    s->press_y      = ev->y;
}

static void mouse_up(explorer_t* s, const wm_event_t* ev) {
    if (ev->button == INPUT_BTN_LEFT) {
        s->edit_dragging = false;
        if (s->banding)
            band_end(s);
        if (s->reselect >= 0)
            explorer_select_only(s, s->reselect);   // A plain click on a selected item
    }
    if (ev->button == s->press_button) {
        s->press_armed = false;
        s->reselect = -1;
    }
}

static void mouse_move(explorer_t* s, const wm_event_t* ev) {
    if (s->banding && (ev->buttons & INPUT_BTN_LEFT)) {
        band_to(s, ev->x, ev->y);
        return;
    }
    if (s->press_armed && (ev->buttons & s->press_button)) {
        int32_t dx = ev->x - s->press_x, dy = ev->y - s->press_y;
        if (dx > DRAG_THRESHOLD || -dx > DRAG_THRESHOLD || dy > DRAG_THRESHOLD || -dy > DRAG_THRESHOLD) {
            start_drag(s);
            return;
        }
    }
    if (s->renaming && s->edit_dragging && (ev->buttons & INPUT_BTN_LEFT)) {
        explorer_place_rename(s);
        ui_edit_drag(&s->edit, ev->x);
        s->dirty = true;
    }
    bool back = explorer_back_hit(ev->x, ev->y);
    if (back != s->back_hover) {
        s->back_hover = back;
        s->dirty = true;
    }
}

static void key(explorer_t* s, const wm_event_t* ev) {
    if (s->renaming) {
        rename_key(s, ev);
        return;
    }

    if (ev->mods & INPUT_MOD_CTRL) {
        int c = tolower(ev->key < 0x80 ? ev->key : 0);
        if (c == 0x03) c = 'c';             // In case Ctrl arrives as a control code
        if (c == 0x18) c = 'x';
        if (c == 0x16) c = 'v';
        if (c == 0x12) c = 'r';
        if (c == 0x01) c = 'a';
        int sel = focus_or_first(s);
        switch (c) {
            case 'a': explorer_select_all(s); break;
            case 'c': if (sel >= 0) run_action(s, ACT_COPY, sel); break;
            case 'x': if (sel >= 0) run_action(s, ACT_CUT, sel); break;
            case 'v': run_action(s, ACT_PASTE, -1); break;
            case 'r': run_action(s, ACT_REFRESH, -1); break;
        }
        return;
    }

    int cols = explorer_columns(s);
    int page = explorer_rows_per_page(s) * cols;
    int sel = s->selected;
    switch (ev->key) {
        case KEY_LEFT:      select_item(s, sel < 0 ? 0 : sel - 1);    break;
        case KEY_RIGHT:     select_item(s, sel < 0 ? 0 : sel + 1);    break;
        case KEY_UP:        select_item(s, sel < 0 ? 0 : (sel - cols >= 0 ? sel - cols : sel)); break;
        case KEY_DOWN:      select_item(s, sel < 0 ? 0 : (sel + cols < s->total_items ? sel + cols : sel)); break;
        case KEY_HOME:      select_item(s, 0);                        break;
        case KEY_END:       select_item(s, s->total_items - 1);       break;
        case KEY_PAGE_UP:   select_item(s, sel < 0 ? 0 : (sel - page > 0 ? sel - page : 0)); break;
        case KEY_PAGE_DOWN: select_item(s, sel < 0 ? 0 : sel + page); break;
        case KEY_ENTER:     if (sel >= 0) open_item(s, sel);          break;
        case KEY_BACKSPACE: go_up(s);                                 break;
        case KEY_DELETE:
            if (focus_or_first(s) >= 0)
                ask_delete(s, focus_or_first(s));
            break;
        case KEY_ESCAPE:
            explorer_select_none(s);
            break;
    }
}

// Wheel: the grid scrolls a row a notch (Ctrl: a page); a selection
// rectangle being drawn follows the items that moved under the pointer
static void wheel(explorer_t* s, const wm_event_t* ev) {
    if (ev->wheel == 0)
        return;
    int32_t step = ITEM_HEIGHT;
    if (ev->mods & INPUT_MOD_CTRL)
        step *= explorer_rows_per_page(s);
    int32_t before = s->scroll_y;
    s->scroll_y += (int32_t)ev->wheel * step;
    explorer_clamp_scroll(s);
    if (s->scroll_y == before)
        return;
    if (s->banding)
        band_to(s, ev->x, ev->y);
    s->dirty = true;
}

void explorer_handle_event(explorer_t* s, const wm_event_t* ev) {
    switch (ev->type) {
        case WM_EVENT_CLOSE:
            explorer_close_window(s);       // s is gone after this
            return;
        case WM_EVENT_DESTROYED:
            explorer_forget_window(s);
            return;
        case WM_EVENT_RESIZED:
            s->win_width = ev->rect_w;
            s->win_height = ev->rect_h;
            explorer_clamp_scroll(s);
            s->dirty = true;
            break;
        case WM_EVENT_MOUSE_DOWN:
            mouse_down(s, ev);
            break;
        case WM_EVENT_MOUSE_MOVE:
            mouse_move(s, ev);
            break;
        case WM_EVENT_MOUSE_UP:
            mouse_up(s, ev);
            break;
        case WM_EVENT_MOUSE_WHEEL:
            wheel(s, ev);
            break;
        case WM_EVENT_DROP:
            drop_files(s, ev);
            break;
        case WM_EVENT_MOUSE_LEAVE:
            if (s->back_hover) {
                s->back_hover = false;
                s->dirty = true;
            }
            break;
        case WM_EVENT_KEY:
            key(s, ev);
            break;
        case WM_EVENT_CONTEXT_MENU:
            context_menu(s, ev);
            break;
        case WM_EVENT_MENU_COMMAND:
            menu_command(s, ev->param);
            break;
        case WM_EVENT_SHOW:
            s->dirty = true;
            break;
    }
}



/*
 * regedit: the Registry Editor. Browse the system registry and change it.
 *
 *     regedit                  the window, at hkey_user
 *     regedit <key path>       the window, at that key
 *
 * Installed as <home>/sys/settings/regedit, in the settings folder the
 * start menu's "Settings" opens.
 *
 * The window:
 *
 *   Address bar   the key picked, as a path ("hkey_user/control/desktop").
 *                 Type a path and press Enter to go there.
 *   Left pane     the keys, as a tree under the two hives (hkey_root: the
 *                 system's, hkey_user: this user's).
 *   Right pane    the key's values: name, type and data.
 *   Status bar    what happened last, or a hint.
 *
 * A value is just bytes to the registry. The editor shows a value as a
 * String when it is printable text ending in '\0' (what os_reg_set_string
 * stores) and as Binary (hex bytes) otherwise. Either can be made or
 * changed here.
 *
 * Changes (os/registry.h; each is saved to the hive's file at once):
 *
 *   New key, New string value, New binary value
 *   Modify a value (its data, its type or its name)
 *   Rename a key or a value
 *   Delete a key (with everything below it) or a value, after asking
 *   Copy the key's path
 *
 * Mouse: click to pick, click the arrow or double click a key to open or
 * close it, double click a value to modify it, right click for the menu,
 * drag the scroll bars or the line between the panes. The mouse wheel
 * scrolls the pane under the pointer WHEEL_ROWS rows a notch (Ctrl: a page).
 *
 * Keys: Tab moves between the address bar, the keys and the values. Up /
 * Down / Home / End / Page Up / Page Down move, Left / Right close / open a
 * key, Enter opens a key or modifies a value, F2 renames, Delete deletes,
 * Ctrl+N new key, Ctrl+S new string value, Ctrl+B new binary value, Ctrl+L
 * the address bar, F5 reads everything again (as does coming back to the
 * window), Escape or Ctrl+Q closes.
 *
 * hkey_user is the user's own and needs only file write clearance (the
 * desktop's launch mask has it). Changing hkey_root takes admin: the first
 * time, the editor asks for it with sec_elevate() and then goes on.
 *
 * Limits: names are at most 15 characters (OS_REG_NAME_MAX - 1), a value
 * at most 4096 bytes (OS_REG_VALUE_MAX). The editing box holds 255
 * characters, so a string up to 254 characters or a binary value up to 85
 * bytes can be changed here; longer ones show, and can be renamed or
 * deleted, but not edited.
 *
 * Needs SEC_FILE (the registry), SEC_GFX, SEC_IPC and, for hkey_root,
 * SEC_ELEVATE.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/os.h>
#include <os/registry.h>
#include <os/security.h>
#include <os/ui.h>
#include <os/wm.h>
#include <os/wm_app.h>     // Not on the desktop: says so and exits before main()

#ifndef APP_NAME
#define APP_NAME        "regedit"
#endif

#define HIVE_ROOT       "hkey_root"
#define HIVE_USER       "hkey_user"

#define PATH_LEN        256         // A key's path
#define NAME_LEN        OS_REG_NAME_MAX
#define MAX_ROWS        1024        // Keys shown in the tree
#define MAX_OPEN        256         // Keys open in the tree
#define MAX_VALUES      256         // Values of one key
#define PREVIEW_LEN     96          // Bytes of a value kept for the list
#define VALUE_MAX       OS_REG_VALUE_MAX

// Layout
#define WIN_W           700
#define WIN_H           440
#define MIN_W           360
#define MIN_H           220
#define PAD             8
#define ICON            16
#define INDENT          16
#define EXPANDER_W      14
#define SCROLL_W        12
#define WHEEL_ROWS      3           // Rows a wheel notch scrolls (Ctrl: a page)
#define THUMB_MIN       16
#define SPLIT_GRAB      4           // Pixels either side of the divider that grab it
#define MIN_PANE        120

// Dialog layout
#define DLG_W           380
#define DLG_ROW         30
#define DLG_LABEL_W     64
#define BUTTON_W        72
#define BUTTON_H        24

// Menu command ids
enum {
    CMD_NEW_KEY = 1,
    CMD_NEW_STRING,
    CMD_NEW_BINARY,
    CMD_MODIFY,
    CMD_RENAME,
    CMD_DELETE,
    CMD_COPY_PATH,
    CMD_COPY_DATA,
    CMD_EXPAND,
    CMD_COLLAPSE,
    CMD_REFRESH,
    CMD_TYPE_STRING,
    CMD_TYPE_BINARY,
};

typedef enum { FOCUS_ADDRESS, FOCUS_TREE, FOCUS_VALUES } focus_t;
typedef enum { TYPE_STRING, TYPE_BINARY } vtype_t;

//
// The tree: the keys shown, in order. Rebuilt from the registry whenever
// it is read again; which keys are open is kept by path.
//

typedef struct {
    char path[PATH_LEN];
    char name[NAME_LEN];
    int  depth;
    bool has_children;
    bool open;
    bool denied;                    // EACCES listing it
} row_t;

static row_t* rows;                 // MAX_ROWS of them
static int    row_count;
static bool   rows_cut;             // More keys than MAX_ROWS: the rest left out
static char   open_paths[MAX_OPEN][PATH_LEN];
static int    open_count;

static char   sel_path[PATH_LEN];   // The key picked (kept by path)
static int    sel_row = -1;

//
// The values of the key picked, sorted by name
//

typedef struct {
    char     name[NAME_LEN];
    vtype_t  type;
    int      error;                 // errno reading it (EACCES), else 0
    size_t   length;
    uint8_t  preview[PREVIEW_LEN];  // Its first bytes
} value_t;

static value_t* values;             // MAX_VALUES of them
static int      value_count;
static int      key_error;          // errno listing the key's values, else 0
static int      sel_value = -1;
static char     sel_value_name[NAME_LEN];

//
// Dialogs, drawn over the window. While one is up it has the input.
//

typedef enum {
    DLG_NONE,
    DLG_VALUE,                      // New or modify a value: name, type, data
    DLG_NAME,                       // New key, or rename a key or value: one name
    DLG_CONFIRM,                    // Delete?
} dlg_kind_t;

typedef enum {
    NAME_NEW_KEY,
    NAME_RENAME_KEY,
    NAME_RENAME_VALUE,
} name_purpose_t;

typedef struct {
    dlg_kind_t     kind;
    char           title[64];
    char           key[PATH_LEN];   // The key it is about
    char           old_name[NAME_LEN];  // The value modified / renamed ("" for new)
    bool           is_new;
    name_purpose_t purpose;
    vtype_t        type;
    ui_edit_t      name_box;
    ui_edit_t      data_box;
    int            focus;           // 0 name box, 1 data box
    bool           dragging;
    char           message[160];    // Confirm: the question; others: why it can't be done
    bool           delete_key;      // Confirm: a key (else the value old_name)
} dialog_t;

static dialog_t dlg;

// Window
static wm_window_t win;
static wm_theme_t  theme;
static bool        running = true;
static bool        dirty = true;
static int32_t     win_w = WIN_W, win_h = WIN_H;
static int32_t     lh = 16, row_h = 20, cw = 8;
static int32_t     split_x = 0;     // Where the panes meet (0: not placed yet)
static bool        split_drag;
static focus_t     focus = FOCUS_TREE;

static ui_edit_t   address_box;
static bool        address_drag;

static int         tree_top;        // First tree row shown
static int         value_top;       // First value shown
static int         thumb_drag;      // 0 none, 1 tree, 2 values
static int32_t     thumb_grab;

static char        status[192];
static bool        status_error;

//
// Helpers
//

static void set_status(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(bool error, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
    status_error = error;
    dirty = true;
}

static bool in_rect(gfx_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

// The hive a path is in: its first segment
static bool path_in_root_hive(const char* path) {
    size_t n = strlen(HIVE_ROOT);
    return strncmp(path, HIVE_ROOT, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static bool path_is_hive(const char* path) {
    return strchr(path, '/') == NULL;
}

// "a/b/c" -> "a/b"; a hive stays itself
static void parent_path(const char* path, char* out, size_t size) {
    strlcpy(out, path, size);
    char* slash = strrchr(out, '/');
    if (slash != NULL)
        *slash = '\0';
}

static const char* last_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

// path + "/" + name into out. False if it doesn't fit.
static bool join_path(char* out, size_t size, const char* path, const char* name) {
    int n = snprintf(out, size, "%s/%s", path, name);
    return n >= 0 && (size_t)n < size;
}

// Is `path` the same as `ancestor` or below it?
static bool path_within(const char* path, const char* ancestor) {
    size_t n = strlen(ancestor);
    return strncmp(path, ancestor, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

// A path typed by hand: no '/' at the ends or doubled
static void tidy_path(char* path) {
    char* out = path;
    const char* in = path;
    while (*in == '/' || *in == ' ') in++;
    while (*in != '\0') {
        if (*in == '/' && (out == path || out[-1] == '/')) {
            in++;
            continue;
        }
        *out++ = *in++;
    }
    while (out > path && (out[-1] == '/' || out[-1] == ' '))
        out--;
    *out = '\0';
}

static const char* reg_error(int error) {
    switch (error) {
        case EPERM:        return "not allowed (changing hkey_root needs admin)";
        case EACCES:       return "the registry marks it read-only";
        case ENOENT:       return "it is not there any more";
        case EEXIST:       return "that name is taken";
        case EINVAL:       return "not a name the registry takes";
        case ENAMETOOLONG: return "a name is longer than 15 characters";
        case EIO:          return "changed, but the hive could not be saved to disk";
        case ENOSYS:       return "this kernel can't do that (update it)";
        case ENOMEM:       return "out of memory";
        default:           return strerror(error);
    }
}

//
// Reading values
//

static bool printable_text(const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\t')
            continue;
        if (data[i] < 0x20 || data[i] >= 0x7F)
            return false;
    }
    return true;
}

// String when it is printable text ending in its '\0'
static vtype_t guess_type(const uint8_t* data, size_t length) {
    if (length >= 1 && data[length - 1] == '\0' && printable_text(data, length - 1))
        return TYPE_STRING;
    return TYPE_BINARY;
}

// Read a whole value into buf (VALUE_MAX bytes). Its length, or -1 with errno.
static ssize_t read_value(const char* key, const char* name, uint8_t* buf) {
    return os_reg_get(key, name, buf, VALUE_MAX);
}

// Bytes as "01 a0 ff", cut with ".." if it would pass `size`
static void format_hex(const uint8_t* data, size_t n, size_t total, char* out, size_t size) {
    size_t at = 0;
    out[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (at + 4 >= size)
            break;
        at += (size_t)snprintf(out + at, size - at, i == 0 ? "%02x" : " %02x", data[i]);
    }
    if ((n < total || at + 4 >= size) && at + 3 < size)
        strlcpy(out + at, " ..", size - at);
}

// The data column's text
static void format_value(const value_t* v, char* out, size_t size) {
    if (v->error != 0) {
        strlcpy(out, "Access denied", size);
        return;
    }
    size_t shown = v->length < PREVIEW_LEN ? v->length : PREVIEW_LEN;
    if (v->type == TYPE_STRING) {
        size_t n = shown;
        if (n > 0 && v->preview[n - 1] == '\0') n--;
        if (n >= size) n = size - 1;
        memcpy(out, v->preview, n);
        out[n] = '\0';
        if (v->length > PREVIEW_LEN && n + 3 < size)
            strlcat(out, "..", size);
    } else if (v->length == 0) {
        strlcpy(out, "(zero-length binary value)", size);
    } else {
        format_hex(v->preview, shown, v->length, out, size);
    }
}

//
// Reading the registry
//

static int compare_names(const void* a, const void* b) {
    return strcasecmp((const char*)a, (const char*)b);
}

static int compare_values(const void* a, const void* b) {
    return strcasecmp(((const value_t*)a)->name, ((const value_t*)b)->name);
}

static bool is_open(const char* path) {
    for (int i = 0; i < open_count; i++)
        if (strcmp(open_paths[i], path) == 0)
            return true;
    return false;
}

static void set_open(const char* path, bool open) {
    for (int i = 0; i < open_count; i++) {
        if (strcmp(open_paths[i], path) == 0) {
            if (!open)
                memmove(open_paths[i], open_paths[i + 1], (size_t)(open_count - i - 1) * PATH_LEN), open_count--;
            return;
        }
    }
    if (open && open_count < MAX_OPEN)
        strlcpy(open_paths[open_count++], path, PATH_LEN);
}

// Forget open keys at or below `path` (it was deleted or renamed)
static void forget_open_below(const char* path) {
    for (int i = 0; i < open_count;) {
        if (path_within(open_paths[i], path)) {
            memmove(open_paths[i], open_paths[i + 1], (size_t)(open_count - i - 1) * PATH_LEN);
            open_count--;
        } else {
            i++;
        }
    }
}

// The subkey names of `path`, sorted, into a malloc'd array of NAME_LEN
// names. Their count, or -1 with errno.
static int list_subkeys(const char* path, char (**out)[NAME_LEN]) {
    int cap = 16, n = 0;
    char (*names)[NAME_LEN] = malloc((size_t)cap * NAME_LEN);
    if (names == NULL)
        return -1;
    for (unsigned i = 0;; i++) {
        char name[NAME_LEN];
        int r = os_reg_key(path, i, name, sizeof(name));
        if (r < 0) {
            int e = errno;
            free(names);
            errno = e;
            return -1;
        }
        if (r == 0)
            break;
        if (n == cap) {
            cap *= 2;
            char (*more)[NAME_LEN] = realloc(names, (size_t)cap * NAME_LEN);
            if (more == NULL)
                break;
            names = more;
        }
        strlcpy(names[n++], name, NAME_LEN);
        if (n >= MAX_ROWS)
            break;
    }
    qsort(names, (size_t)n, NAME_LEN, compare_names);
    *out = names;
    return n;
}

static void add_rows(const char* path, const char* name, int depth) {
    if (row_count >= MAX_ROWS) {
        rows_cut = true;
        return;
    }
    row_t* r = &rows[row_count++];
    strlcpy(r->path, path, sizeof(r->path));
    strlcpy(r->name, name, sizeof(r->name));
    r->depth = depth;
    r->open = is_open(path);
    r->denied = false;

    char (*names)[NAME_LEN] = NULL;
    int n = list_subkeys(path, &names);
    if (n < 0) {
        r->denied = (errno == EACCES);
        r->has_children = false;
        r->open = false;
        return;
    }
    r->has_children = n > 0;
    if (!r->has_children)
        r->open = false;
    if (r->open) {
        for (int i = 0; i < n; i++) {
            char child[PATH_LEN];
            if (join_path(child, sizeof(child), path, names[i]))
                add_rows(child, names[i], depth + 1);
        }
    }
    free(names);
}

static void read_values(void);

// Read the tree again, keeping what is open and what is picked
static void read_tree(void) {
    row_count = 0;
    rows_cut = false;
    add_rows(HIVE_ROOT, HIVE_ROOT, 0);
    add_rows(HIVE_USER, HIVE_USER, 0);

    // The key picked, else the nearest key above it that is still there
    sel_row = -1;
    char want[PATH_LEN];
    strlcpy(want, sel_path, sizeof(want));
    while (sel_row < 0) {
        for (int i = 0; i < row_count; i++) {
            if (strcmp(rows[i].path, want) == 0) {
                sel_row = i;
                break;
            }
        }
        if (sel_row >= 0 || path_is_hive(want))
            break;
        parent_path(want, want, sizeof(want));
    }
    if (sel_row < 0 && row_count > 0)
        sel_row = row_count > 1 ? 1 : 0;   // hkey_user
    if (sel_row >= 0)
        strlcpy(sel_path, rows[sel_row].path, sizeof(sel_path));
    read_values();
    dirty = true;
}

// Read the values of the key picked
static void read_values(void) {
    value_count = 0;
    key_error = 0;
    if (sel_path[0] == '\0')
        return;

    uint8_t* buf = malloc(VALUE_MAX);
    if (buf == NULL) {
        key_error = ENOMEM;
        return;
    }
    for (unsigned i = 0; value_count < MAX_VALUES; i++) {
        char name[NAME_LEN];
        int r = os_reg_value(sel_path, i, name, sizeof(name));
        if (r < 0) {
            key_error = errno;
            break;
        }
        if (r == 0)
            break;
        value_t* v = &values[value_count++];
        memset(v, 0, sizeof(*v));
        strlcpy(v->name, name, sizeof(v->name));
        ssize_t n = read_value(sel_path, name, buf);
        if (n < 0) {
            v->error = errno;
            v->type = TYPE_BINARY;
            continue;
        }
        v->length = (size_t)n;
        v->type = guess_type(buf, v->length);
        memcpy(v->preview, buf, v->length < PREVIEW_LEN ? v->length : PREVIEW_LEN);
    }
    free(buf);
    qsort(values, (size_t)value_count, sizeof(values[0]), compare_values);

    // The value picked, by name
    sel_value = -1;
    for (int i = 0; i < value_count; i++) {
        if (strcmp(values[i].name, sel_value_name) == 0) {
            sel_value = i;
            break;
        }
    }
    if (sel_value < 0)
        sel_value_name[0] = '\0';
    dirty = true;
}

//
// Layout
//

static int32_t address_h(void) { return lh + 12; }
static int32_t status_h(void)  { return lh + 8; }
static int32_t pane_top(void)  { return address_h(); }
static int32_t pane_h(void)    { int32_t h = win_h - address_h() - status_h(); return h > 0 ? h : 0; }

static void clamp_split(void) {
    if (split_x == 0)
        split_x = win_w * 2 / 5;
    if (split_x > win_w - MIN_PANE) split_x = win_w - MIN_PANE;
    if (split_x < MIN_PANE) split_x = MIN_PANE;
}

static gfx_rect_t tree_rect(void)   { return gfx_rect(0, pane_top(), split_x, pane_h()); }
static gfx_rect_t values_rect(void) { return gfx_rect(split_x + 1, pane_top(), win_w - split_x - 1, pane_h()); }

// The value list's header and rows
static int32_t header_h(void) { return row_h; }

static int tree_visible(void) {
    int n = (int)(pane_h() / row_h);
    return n > 0 ? n : 1;
}

static int values_visible(void) {
    int n = (int)((pane_h() - header_h()) / row_h);
    return n > 0 ? n : 1;
}

static void clamp_scrolls(void) {
    int max = row_count - tree_visible();
    if (tree_top > max) tree_top = max;
    if (tree_top < 0) tree_top = 0;
    max = value_count - values_visible();
    if (value_top > max) value_top = max;
    if (value_top < 0) value_top = 0;
}

static void scroll_to_selection(void) {
    if (sel_row >= 0) {
        if (sel_row < tree_top) tree_top = sel_row;
        else if (sel_row >= tree_top + tree_visible()) tree_top = sel_row - tree_visible() + 1;
    }
    if (sel_value >= 0) {
        if (sel_value < value_top) value_top = sel_value;
        else if (sel_value >= value_top + values_visible()) value_top = sel_value - values_visible() + 1;
    }
    clamp_scrolls();
}

// A scroll bar's thumb: top and height inside a track of `track` pixels
static void thumb_rect(int count, int visible, int top, int32_t track, int32_t* ty, int32_t* th) {
    int32_t h = track * visible / (count > 0 ? count : 1);
    if (h < THUMB_MIN) h = THUMB_MIN;
    if (h > track) h = track;
    int max = count - visible;
    *ty = max > 0 ? (track - h) * top / max : 0;
    *th = h;
}

// Columns of the value list (x offsets inside it)
static int32_t col_type_x(void) {
    int32_t w = values_rect().w;
    int32_t x = 16 * cw + ICON + 10;
    if (x > w / 2) x = w / 2;
    return x;
}

static int32_t col_data_x(void) {
    return col_type_x() + 8 * cw;
}

//
// Drawing
//

static void default_theme(void) {
    memset(&theme, 0, sizeof(theme));
    theme.client.background = 0xFF141420;  theme.client.text = 0xFFFFFFFF;
    theme.client.text_soft = 0xFFD0D0DF;   theme.client.text_muted = 0xFF8888AA;
    theme.client.accent = 0xFF04C004;      theme.client.divider_soft = 0xFF086008;
    theme.client.text_value = 0xFF3FFF3F;  theme.client.text_danger = 0xFFC00404;
    theme.edit.selection = 0xFF2E5C2E;     theme.edit.background = 0xFF202020;
    theme.edit.border = 0xFF404040;        theme.edit.text = 0xFFFFFFFF;
    theme.edit.cursor = 0xFFF0F0F0;
    theme.button.fill = 0xFF1C1C2A;        theme.button.border = 0xFF444466;
    theme.button.border_active = 0xFF04C004; theme.button.text = 0xFF3FFF3F;
    theme.frame.border = 0xFF444466;
}

static void load_theme(void) {
    if (wm_get_theme(&theme) < 0)
        default_theme();
}

// Text cut with ".." to fit in `width` pixels
static void draw_text_fit(gfx_surface_t* s, int32_t x, int32_t y, const char* text, int32_t width, gfx_color_t color) {
    if (width <= 0)
        return;
    if (gfx_text_width(s, text) <= width) {
        gfx_draw_text(s, x, y, text, color);
        return;
    }
    char buf[PATH_LEN + 4];
    size_t n = strlen(text);
    if (n > PATH_LEN) n = PATH_LEN;
    while (n > 0) {
        memcpy(buf, text, n);
        memcpy(buf + n, "..", 3);
        if (gfx_text_width(s, buf) <= width)
            break;
        n--;
    }
    if (n > 0)
        gfx_draw_text(s, x, y, buf, color);
}

// A folder: closed, or open (lighter, with its flap down)
static void draw_folder(gfx_surface_t* s, int32_t x, int32_t y, bool open, bool hive) {
    gfx_color_t body = hive ? GFX_RGB(110, 150, 220) : GFX_RGB(226, 186, 82);
    gfx_color_t dark = hive ? GFX_RGB(60, 90, 150) : GFX_RGB(160, 120, 40);
    gfx_color_t light = hive ? GFX_RGB(170, 200, 250) : GFX_RGB(250, 220, 130);
    gfx_fill_rect(s, x + 1, y + 3, 6, 2, dark);
    gfx_fill_rect(s, x + 1, y + 4, 14, 10, body);
    gfx_draw_rect(s, x + 1, y + 4, 14, 10, dark);
    if (open) {
        for (int i = 0; i < 6; i++)
            gfx_draw_hline(s, x + 3 + i / 2, y + 8 + i, 12 - i / 2, light);
        gfx_draw_line(s, x + 3, y + 7, x + 15, y + 7, dark);
    }
}

// A value: "ab" for a string, "01" for binary data
static void draw_value_icon(gfx_surface_t* s, int32_t x, int32_t y, vtype_t type) {
    gfx_color_t back = type == TYPE_STRING ? GFX_RGB(180, 60, 60) : GFX_RGB(50, 110, 190);
    gfx_fill_rect(s, x + 1, y + 2, 14, 12, back);
    gfx_draw_rect(s, x + 1, y + 2, 14, 12, GFX_RGB(20, 20, 26));
    const gfx_color_t ink = GFX_RGB(240, 240, 245);
    if (type == TYPE_STRING) {
        // "ab"
        gfx_draw_rect(s, x + 3, y + 7, 4, 5, ink);
        gfx_draw_hline(s, x + 3, y + 6, 3, ink);
        gfx_draw_vline(s, x + 9, y + 4, 8, ink);
        gfx_draw_rect(s, x + 9, y + 7, 4, 5, ink);
    } else {
        // "01"
        gfx_draw_rect(s, x + 3, y + 5, 4, 7, ink);
        gfx_draw_vline(s, x + 11, y + 5, 7, ink);
        gfx_set_pixel(s, x + 10, y + 6, ink);
    }
}

// Open: pointing down; closed: pointing right
static void draw_expander(gfx_surface_t* s, int32_t x, int32_t y, bool open, gfx_color_t color) {
    for (int i = 0; i < 4; i++) {
        if (open)
            gfx_draw_hline(s, x + 1 + i, y + 6 + i, 7 - 2 * i, color);
        else
            gfx_draw_vline(s, x + 3 + i, y + 4 + i, 7 - 2 * i, color);
    }
}

static void draw_scrollbar(gfx_surface_t* s, gfx_rect_t track, int count, int visible, int top, bool active) {
    int32_t ty, th;
    thumb_rect(count, visible, top, track.h, &ty, &th);
    gfx_fill_rect(s, track.x, track.y, SCROLL_W, track.h, theme.edit.background);
    gfx_fill_rect(s, track.x + 2, track.y + ty + 1, SCROLL_W - 4, th - 2,
                  active ? theme.client.accent : theme.button.border);
}

static uint32_t selection_color(bool focused) {
    return focused ? theme.edit.selection : theme.button.fill;
}

static void draw_tree(gfx_surface_t* s) {
    gfx_rect_t r = tree_rect();
    bool bar = row_count > tree_visible();
    int32_t list_w = bar ? r.w - SCROLL_W : r.w;
    gfx_rect_t clip = gfx_rect(r.x, r.y, list_w, r.h);
    gfx_set_clip(s, &clip);

    int visible = tree_visible();
    for (int i = 0; i <= visible && tree_top + i < row_count; i++) {
        int n = tree_top + i;
        const row_t* row = &rows[n];
        int32_t y = r.y + i * row_h;
        bool picked = n == sel_row;
        if (picked)
            gfx_fill_rect(s, r.x, y, list_w, row_h, selection_color(focus == FOCUS_TREE));
        int32_t x = r.x + 4 + row->depth * INDENT;
        int32_t iy = y + (row_h - ICON) / 2;
        if (row->has_children)
            draw_expander(s, x, iy, row->open, theme.client.text_soft);
        x += EXPANDER_W;
        draw_folder(s, x, iy, row->open || picked, row->depth == 0);
        x += ICON + 5;
        gfx_color_t color = row->denied ? theme.client.text_muted
                          : (picked || row->depth == 0) ? theme.client.text : theme.client.text_soft;
        draw_text_fit(s, x, gfx_text_y(s, y, row_h), row->name, r.x + list_w - x - 4, color);
    }
    gfx_set_clip(s, NULL);

    if (bar)
        draw_scrollbar(s, gfx_rect(r.x + list_w, r.y, SCROLL_W, r.h), row_count, visible, tree_top, thumb_drag == 1);
}

static void draw_values(gfx_surface_t* s) {
    gfx_rect_t r = values_rect();
    if (r.w <= 0)
        return;
    bool bar = value_count > values_visible();
    int32_t list_w = bar ? r.w - SCROLL_W : r.w;
    int32_t tx = r.x + col_type_x(), dx = r.x + col_data_x();

    // Header
    gfx_fill_rect(s, r.x, r.y, r.w, header_h(), theme.button.fill);
    int32_t hy = gfx_text_y(s, r.y, header_h());
    gfx_draw_text(s, r.x + 6, hy, "Name", theme.client.text_muted);
    gfx_draw_text(s, tx, hy, "Type", theme.client.text_muted);
    gfx_draw_text(s, dx, hy, "Data", theme.client.text_muted);
    gfx_draw_vline(s, tx - 6, r.y + 3, header_h() - 6, theme.button.border);
    gfx_draw_vline(s, dx - 6, r.y + 3, header_h() - 6, theme.button.border);
    gfx_draw_hline(s, r.x, r.y + header_h() - 1, r.w, theme.client.divider_soft);

    int32_t top = r.y + header_h();
    gfx_rect_t clip = gfx_rect(r.x, top, list_w, r.h - header_h());
    gfx_set_clip(s, &clip);

    if (key_error != 0 && value_count == 0) {
        gfx_draw_text(s, r.x + 6, gfx_text_y(s, top, row_h), "Access denied", theme.client.text_danger);
    } else if (value_count == 0) {
        gfx_draw_text(s, r.x + 6, gfx_text_y(s, top, row_h), "(no values)", theme.client.text_muted);
    }

    int visible = values_visible();
    for (int i = 0; i <= visible && value_top + i < value_count; i++) {
        int n = value_top + i;
        const value_t* v = &values[n];
        int32_t y = top + i * row_h;
        bool picked = n == sel_value;
        if (picked)
            gfx_fill_rect(s, r.x, y, list_w, row_h, selection_color(focus == FOCUS_VALUES));
        int32_t ty = gfx_text_y(s, y, row_h);
        draw_value_icon(s, r.x + 4, y + (row_h - ICON) / 2, v->type);
        int32_t nx = r.x + 4 + ICON + 5;
        draw_text_fit(s, nx, ty, v->name, tx - 8 - nx, picked ? theme.client.text : theme.client.text_soft);
        draw_text_fit(s, tx, ty, v->error ? "" : v->type == TYPE_STRING ? "String" : "Binary",
                      dx - 8 - tx, theme.client.text_muted);
        char data[PREVIEW_LEN * 3 + 8];
        format_value(v, data, sizeof(data));
        draw_text_fit(s, dx, ty, data, r.x + list_w - dx - 4,
                      v->error ? theme.client.text_muted : v->type == TYPE_STRING ? theme.client.text_value
                                                                                  : theme.client.text_soft);
    }
    gfx_set_clip(s, NULL);

    if (bar)
        draw_scrollbar(s, gfx_rect(r.x + list_w, top, SCROLL_W, r.h - header_h()), value_count, visible, value_top,
                       thumb_drag == 2);
}

static void draw_box(gfx_surface_t* s, const ui_edit_t* e, bool focused) {
    ui_edit_style_t st = {
        .background = theme.edit.background,
        .border     = focused ? theme.button.border_active : theme.edit.border,
        .text       = theme.edit.text,
        .cursor     = focused ? theme.edit.cursor : theme.edit.background,   // Hidden unfocused
        .selection  = focused ? theme.edit.selection : theme.edit.background,
    };
    ui_edit_paint(e, s, &st);
}

// Place a box on its rectangle (it sizes itself to the font)
static void place_box(ui_edit_t* e, gfx_rect_t r, gfx_surface_t* s, gfx_rect_t bounds) {
    ui_edit_place(e, r.x + r.w / 2, gfx_text_y(s, r.y, r.h), r.w, r.w, bounds);
}

static gfx_rect_t address_rect(void) {
    int32_t label = 8 * cw + PAD;
    return gfx_rect(label, 4, win_w - label - PAD, address_h() - 8);
}

static void draw_address(gfx_surface_t* s) {
    gfx_fill_rect(s, 0, 0, win_w, address_h(), theme.button.fill);
    gfx_draw_text(s, PAD, gfx_text_y(s, 0, address_h()), "Key", theme.client.text_muted);
    place_box(&address_box, address_rect(), s, gfx_rect(0, 0, win_w, address_h()));
    draw_box(s, &address_box, focus == FOCUS_ADDRESS && dlg.kind == DLG_NONE);
    gfx_draw_hline(s, 0, address_h() - 1, win_w, theme.client.divider_soft);
}

static void draw_status(gfx_surface_t* s) {
    int32_t top = win_h - status_h();
    gfx_fill_rect(s, 0, top, win_w, status_h(), theme.button.fill);
    gfx_draw_hline(s, 0, top, win_w, theme.client.divider_soft);
    int32_t ty = gfx_text_y(s, top, status_h());
    if (status[0] != '\0') {
        draw_text_fit(s, PAD, ty, status, win_w - 2 * PAD, status_error ? theme.client.text_danger : theme.client.text_soft);
        return;
    }
    char text[96];
    if (rows_cut)
        snprintf(text, sizeof(text), "Only the first %d keys are shown", MAX_ROWS);
    else
        snprintf(text, sizeof(text), "%d value%s", value_count, value_count == 1 ? "" : "s");
    gfx_draw_text(s, PAD, ty, text, theme.client.text_soft);
    const char* hint = path_in_root_hive(sel_path) && !sec_is_admin() ? "hkey_root: changes ask for admin"
                                                                     : "Right click: menu  F2 rename  Del delete";
    int32_t hw = gfx_text_width(s, hint);
    if (gfx_text_width(s, text) + hw + 3 * PAD < win_w)
        gfx_draw_text(s, win_w - PAD - hw, ty, hint, theme.client.text_muted);
}

//
// Dialog layout and drawing
//

static int32_t dlg_rows(void) {
    switch (dlg.kind) {
        case DLG_VALUE:   return 3;
        case DLG_NAME:    return 1;
        case DLG_CONFIRM: return 0;
        default:          return 0;
    }
}

static gfx_rect_t dlg_rect(void) {
    int32_t w = DLG_W < win_w - 16 ? DLG_W : win_w - 16;
    int32_t h = lh + 16                           // Title
              + dlg_rows() * DLG_ROW
              + (lh + 4) * (dlg.kind == DLG_VALUE ? 1 : 2) + 6  // Message lines
              + BUTTON_H + 2 * PAD;
    return gfx_rect((win_w - w) / 2, (win_h - h) / 2, w, h);
}

static int32_t dlg_row_y(int row) {
    return dlg_rect().y + lh + 16 + row * DLG_ROW;
}

static gfx_rect_t dlg_field(int row) {
    gfx_rect_t d = dlg_rect();
    return gfx_rect(d.x + PAD + DLG_LABEL_W, dlg_row_y(row), d.w - 2 * PAD - DLG_LABEL_W, BUTTON_H);
}

// The type drop-down (DLG_VALUE row 1)
static gfx_rect_t dlg_type_rect(void) {
    gfx_rect_t f = dlg_field(1);
    f.w = 110;
    return f;
}

static gfx_rect_t dlg_cancel_rect(void) {
    gfx_rect_t d = dlg_rect();
    return gfx_rect(d.x + d.w - PAD - BUTTON_W, d.y + d.h - PAD - BUTTON_H, BUTTON_W, BUTTON_H);
}

static gfx_rect_t dlg_ok_rect(void) {
    gfx_rect_t r = dlg_cancel_rect();
    r.x -= BUTTON_W + 8;
    return r;
}

static void draw_button(gfx_surface_t* s, gfx_rect_t b, const char* label, bool primary) {
    gfx_fill_rect(s, b.x, b.y, b.w, b.h, theme.button.fill);
    gfx_draw_rect(s, b.x, b.y, b.w, b.h, primary ? theme.button.border_active : theme.button.border);
    gfx_draw_text(s, b.x + (b.w - gfx_text_width(s, label)) / 2, gfx_text_y(s, b.y, b.h), label, theme.button.text);
}

static void draw_dropdown(gfx_surface_t* s, gfx_rect_t f, const char* value) {
    gfx_fill_rect(s, f.x, f.y, f.w, f.h, theme.edit.background);
    gfx_draw_rect(s, f.x, f.y, f.w, f.h, theme.edit.border);
    gfx_draw_text(s, f.x + 6, gfx_text_y(s, f.y, f.h), value, theme.edit.text);
    gfx_draw_text(s, f.x + f.w - cw - 6, gfx_text_y(s, f.y, f.h), "v", theme.client.text_muted);
}

static void draw_dialog(gfx_surface_t* s) {
    if (dlg.kind == DLG_NONE)
        return;
    gfx_rect_t d = dlg_rect();

    // Shade the window behind it
    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_fill_rect(s, 0, 0, win_w, win_h, 0x80000000);
    gfx_set_blend(s, old);

    gfx_fill_rect(s, d.x, d.y, d.w, d.h, theme.client.background);
    gfx_draw_rect(s, d.x, d.y, d.w, d.h, theme.button.border_active);
    gfx_fill_rect(s, d.x + 1, d.y + 1, d.w - 2, lh + 8, theme.button.fill);
    gfx_draw_text(s, d.x + PAD, d.y + 7, dlg.title, theme.client.text);
    gfx_draw_text(s, d.x + PAD + 1, d.y + 7, dlg.title, theme.client.text);     // Bold

    int32_t text_w = d.w - 2 * PAD;
    int32_t msg_y = dlg_row_y(dlg_rows()) + 2;
    gfx_rect_t inner = gfx_rect(d.x, d.y, d.w, d.h);

    if (dlg.kind == DLG_VALUE) {
        int32_t ly = gfx_text_y(s, dlg_row_y(0), BUTTON_H);
        gfx_draw_text(s, d.x + PAD, ly, "Name", theme.client.text_soft);
        place_box(&dlg.name_box, dlg_field(0), s, inner);
        draw_box(s, &dlg.name_box, dlg.focus == 0);

        ly = gfx_text_y(s, dlg_row_y(1), BUTTON_H);
        gfx_draw_text(s, d.x + PAD, ly, "Type", theme.client.text_soft);
        draw_dropdown(s, dlg_type_rect(), dlg.type == TYPE_STRING ? "String" : "Binary");

        ly = gfx_text_y(s, dlg_row_y(2), BUTTON_H);
        gfx_draw_text(s, d.x + PAD, ly, "Data", theme.client.text_soft);
        place_box(&dlg.data_box, dlg_field(2), s, inner);
        draw_box(s, &dlg.data_box, dlg.focus == 1);

    } else if (dlg.kind == DLG_NAME) {
        int32_t ly = gfx_text_y(s, dlg_row_y(0), BUTTON_H);
        gfx_draw_text(s, d.x + PAD, ly, "Name", theme.client.text_soft);
        place_box(&dlg.name_box, dlg_field(0), s, inner);
        draw_box(s, &dlg.name_box, true);
        char where[PATH_LEN + 8];
        snprintf(where, sizeof(where), "In %s", dlg.key);
        draw_text_fit(s, d.x + PAD, msg_y, where, text_w, theme.client.text_muted);
    } else if (dlg.kind == DLG_CONFIRM) {
        // The question, wrapped over two lines at most
        size_t fit = (size_t)(text_w / (cw > 0 ? cw : 8));
        ui_line_t lines[2];
        bool more = false;
        size_t n = ui_wrap_text(dlg.message, fit, lines, 2, &more);
        for (size_t i = 0; i < n; i++) {
            char line[192];
            ui_line_copy(&lines[i], false, fit, line, sizeof(line));
            gfx_draw_text(s, d.x + PAD, dlg_row_y(0) + 4 + (int32_t)i * (lh + 4), line, theme.client.text);
        }
    }

    // A problem with what was typed, over the message line
    if (dlg.kind != DLG_CONFIRM && dlg.message[0] != '\0')
        draw_text_fit(s, d.x + PAD, dlg.kind == DLG_VALUE ? msg_y : msg_y + lh + 4, dlg.message, text_w, theme.client.text_danger);

    draw_button(s, dlg_ok_rect(), dlg.kind == DLG_CONFIRM ? "Delete" : "OK", true);
    draw_button(s, dlg_cancel_rect(), "Cancel", false);
}

static void draw(void) {
    dirty = false;
    gfx_surface_t* s = wm_window_surface(win);
    if (s == NULL)
        return;
    lh = gfx_line_height(s);
    cw = gfx_char_width(s);
    row_h = lh + 6 > ICON + 4 ? lh + 6 : ICON + 4;
    clamp_split();
    clamp_scrolls();

    gfx_clear(s, theme.client.background);
    draw_address(s);
    draw_tree(s);
    gfx_draw_vline(s, split_x, pane_top(), pane_h(), split_drag ? theme.client.accent : theme.client.divider_soft);
    draw_values(s);
    draw_status(s);
    draw_dialog(s);
    wm_window_commit(win, NULL);
}

//
// Changing the registry
//

// Run a change; on EPERM in hkey_root, ask for admin once and run it again.
// 0, or -1 with errno.
typedef int (*change_fn)(void* arg);

static int with_admin(const char* path, change_fn fn, void* arg) {
    int r = fn(arg);
    if (r < 0 && errno == EPERM && path_in_root_hive(path) && !sec_is_admin()) {
        draw();
        if (sec_elevate("Change the system registry (hkey_root)") < 0) {
            int e = errno;
            // Cancel at the prompt: ECANCELED, which the callers don't report
            errno = (e == EACCES) ? ECANCELED : e;
            return -1;
        }
        r = fn(arg);
    }
    return r;
}

// "<what>: <why>" in the open dialog; nothing after a Cancel at the
// administrator prompt (ECANCELED, with_admin), which the user just chose
static void failed_message(const char* what, int err) {
    if (err == ECANCELED)
        dlg.message[0] = '\0';
    else
        snprintf(dlg.message, sizeof(dlg.message), "%s: %s", what, reg_error(err));
}


typedef struct {
    const char* path;
    const char* name;
    const char* new_name;
    const void* data;
    size_t      size;
} change_t;

static int do_set(void* arg)    { change_t* c = arg; return os_reg_set(c->path, c->name, c->data, c->size); }
static int do_delete(void* arg) { change_t* c = arg; return os_reg_delete(c->path, c->name); }
static int do_mkkey(void* arg)  { change_t* c = arg; return os_reg_mkkey(c->path); }
static int do_rename(void* arg) { change_t* c = arg; return os_reg_rename(c->path, c->name, c->new_name); }

static void pick_key(const char* path) {
    if (strcmp(sel_path, path) != 0) {
        sel_value_name[0] = '\0';
        value_top = 0;
    }
    strlcpy(sel_path, path, sizeof(sel_path));
    ui_edit_init(&address_box, sel_path, PATH_LEN - 1, 0);
    address_box.anchor = address_box.cursor;    // Not selected
}

// Open every key above `path` so it shows
static void reveal(const char* path) {
    char p[PATH_LEN];
    parent_path(path, p, sizeof(p));
    while (!path_is_hive(path) && strcmp(p, path) != 0) {
        set_open(p, true);
        if (path_is_hive(p))
            break;
        parent_path(p, p, sizeof(p));
    }
}

//
// Starting dialogs
//

static void close_dialog(void) {
    dlg.kind = DLG_NONE;
    dirty = true;
}

static bool can_change_key(void) {
    return sel_row >= 0 && !rows[sel_row].denied;
}

static const value_t* picked_value(void) {
    return (sel_value >= 0 && sel_value < value_count) ? &values[sel_value] : NULL;
}

static void start_new_key(void) {
    if (!can_change_key())
        return;
    memset(&dlg, 0, sizeof(dlg));
    dlg.kind = DLG_NAME;
    dlg.purpose = NAME_NEW_KEY;
    strlcpy(dlg.title, "New key", sizeof(dlg.title));
    strlcpy(dlg.key, sel_path, sizeof(dlg.key));
    ui_edit_init(&dlg.name_box, "new key", NAME_LEN - 1, UI_EDIT_NAME);
    dirty = true;
}

// The value dialog's title, for its type
static void set_dialog_title(void) {
    if (dlg.is_new)
        strlcpy(dlg.title, dlg.type == TYPE_STRING ? "New string value" : "New binary value", sizeof(dlg.title));
    else
        strlcpy(dlg.title, dlg.type == TYPE_STRING ? "Edit string value" : "Edit binary value", sizeof(dlg.title));
}

static void start_new_value(vtype_t type) {
    if (!can_change_key())
        return;
    memset(&dlg, 0, sizeof(dlg));
    dlg.kind = DLG_VALUE;
    dlg.is_new = true;
    dlg.type = type;
    set_dialog_title();
    strlcpy(dlg.key, sel_path, sizeof(dlg.key));
    ui_edit_init(&dlg.name_box, "new value", NAME_LEN - 1, UI_EDIT_NAME);
    ui_edit_init(&dlg.data_box, "", UI_EDIT_MAX - 1, 0);
    dlg.focus = 0;
    dirty = true;
}

static void start_modify(void) {
    const value_t* v = picked_value();
    if (v == NULL)
        return;
    if (v->error != 0) {
        set_status(true, "%s: Access denied", v->name);
        return;
    }
    uint8_t* buf = malloc(VALUE_MAX);
    if (buf == NULL)
        return;
    ssize_t n = read_value(sel_path, v->name, buf);
    if (n < 0) {
        set_status(true, "%s: Access denied", v->name);
        free(buf);
        read_values();
        return;
    }
    vtype_t type = guess_type(buf, (size_t)n);
    char text[UI_EDIT_MAX];
    if (type == TYPE_STRING) {
        if ((size_t)n > UI_EDIT_MAX) {
            set_status(true, "%s is %d characters, too long to edit here (254 at most)", v->name, (int)n - 1);
            free(buf);
            return;
        }
        memcpy(text, buf, (size_t)n);       // Ends in its '\0'
    } else {
        if ((size_t)n * 3 > UI_EDIT_MAX) {
            set_status(true, "%s is %d bytes, too long to edit here (85 at most)", v->name, (int)n);
            free(buf);
            return;
        }
        format_hex(buf, (size_t)n, (size_t)n, text, sizeof(text));
    }
    free(buf);

    memset(&dlg, 0, sizeof(dlg));
    dlg.kind = DLG_VALUE;
    dlg.type = type;
    set_dialog_title();
    strlcpy(dlg.key, sel_path, sizeof(dlg.key));
    strlcpy(dlg.old_name, v->name, sizeof(dlg.old_name));
    ui_edit_init(&dlg.name_box, v->name, NAME_LEN - 1, UI_EDIT_NAME);
    ui_edit_init(&dlg.data_box, text, UI_EDIT_MAX - 1, 0);
    dlg.focus = 1;
    dirty = true;
}

static void start_rename(void) {
    memset(&dlg, 0, sizeof(dlg));
    if (focus == FOCUS_VALUES && picked_value() != NULL) {
        dlg.kind = DLG_NAME;
        dlg.purpose = NAME_RENAME_VALUE;
        strlcpy(dlg.title, "Rename value", sizeof(dlg.title));
        strlcpy(dlg.key, sel_path, sizeof(dlg.key));
        strlcpy(dlg.old_name, picked_value()->name, sizeof(dlg.old_name));
        ui_edit_init(&dlg.name_box, dlg.old_name, NAME_LEN - 1, UI_EDIT_NAME);
    } else if (sel_row >= 0 && !path_is_hive(sel_path)) {
        dlg.kind = DLG_NAME;
        dlg.purpose = NAME_RENAME_KEY;
        strlcpy(dlg.title, "Rename key", sizeof(dlg.title));
        parent_path(sel_path, dlg.key, sizeof(dlg.key));
        strlcpy(dlg.old_name, last_name(sel_path), sizeof(dlg.old_name));
        ui_edit_init(&dlg.name_box, dlg.old_name, NAME_LEN - 1, UI_EDIT_NAME);
    } else if (sel_row >= 0) {
        set_status(true, "A hive can't be renamed");
    }
    dirty = true;
}

static void start_delete(void) {
    memset(&dlg, 0, sizeof(dlg));
    if (focus == FOCUS_VALUES && picked_value() != NULL) {
        dlg.kind = DLG_CONFIRM;
        dlg.delete_key = false;
        strlcpy(dlg.title, "Delete value", sizeof(dlg.title));
        strlcpy(dlg.key, sel_path, sizeof(dlg.key));
        strlcpy(dlg.old_name, picked_value()->name, sizeof(dlg.old_name));
        snprintf(dlg.message, sizeof(dlg.message), "Delete the value \"%s\"? This can't be undone.", dlg.old_name);
    } else if (sel_row >= 0 && !path_is_hive(sel_path)) {
        dlg.kind = DLG_CONFIRM;
        dlg.delete_key = true;
        strlcpy(dlg.title, "Delete key", sizeof(dlg.title));
        strlcpy(dlg.key, sel_path, sizeof(dlg.key));
        snprintf(dlg.message, sizeof(dlg.message),
                 "Delete the key \"%s\" and everything below it? This can't be undone.", last_name(sel_path));
    } else if (sel_row >= 0) {
        set_status(true, "A hive can't be deleted");
    }
    dirty = true;
}

//
// Finishing dialogs
//

static bool valid_name(const char* name, char* why, size_t size) {
    if (name[0] == '\0') {
        strlcpy(why, "Type a name", size);
        return false;
    }
    if (strchr(name, '/') != NULL) {
        strlcpy(why, "A name can't have '/' in it", size);
        return false;
    }
    return true;
}

// "01 a0ff" -> bytes. Their count, or -1 (why says what is wrong).
static int parse_hex(const char* text, uint8_t* out, size_t size, char* why, size_t why_size) {
    size_t n = 0;
    int high = -1;
    for (const char* p = text; *p != '\0'; p++) {
        if (*p == ' ' || *p == ',' || *p == '\t' || *p == ':')
            continue;
        if (!isxdigit((unsigned char)*p)) {
            snprintf(why, why_size, "'%c' is not a hex digit", *p);
            return -1;
        }
        int digit = isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10);
        if (high < 0) {
            high = digit;
        } else {
            if (n >= size) {
                strlcpy(why, "Too many bytes", why_size);
                return -1;
            }
            out[n++] = (uint8_t)(high << 4 | digit);
            high = -1;
        }
    }
    if (high >= 0) {
        strlcpy(why, "An odd number of hex digits: each byte takes two", why_size);
        return -1;
    }
    return (int)n;
}

static void finish_value(void) {
    char name[NAME_LEN];
    strlcpy(name, dlg.name_box.text, sizeof(name));
    if (!valid_name(name, dlg.message, sizeof(dlg.message)))
        return;

    uint8_t data[UI_EDIT_MAX];
    size_t size;
    if (dlg.type == TYPE_STRING) {
        size = strlen(dlg.data_box.text) + 1;
        memcpy(data, dlg.data_box.text, size);
    } else {
        int n = parse_hex(dlg.data_box.text, data, sizeof(data), dlg.message, sizeof(dlg.message));
        if (n < 0)
            return;
        size = (size_t)n;
    }

    // A new name must not be taken: setting would replace that value
    bool renaming = !dlg.is_new && strcmp(name, dlg.old_name) != 0;
    if (dlg.is_new || renaming) {
        uint8_t probe[1];
        if (os_reg_get(dlg.key, name, probe, 0) >= 0 || errno != ENOENT) {
            snprintf(dlg.message, sizeof(dlg.message), "There is a value called \"%s\" already", name);
            return;
        }
    }

    change_t c = { .path = dlg.key, .name = renaming ? dlg.old_name : name, .new_name = name };
    if (renaming && with_admin(dlg.key, do_rename, &c) < 0) {
        failed_message("Not renamed", errno);
        return;
    }
    c.name = name;
    c.data = data;
    c.size = size;
    if (with_admin(dlg.key, do_set, &c) < 0) {
        int e = errno;
        if (renaming) {
            // Renamed, but the data didn't change: say so where it shows
            set_status(true, "Renamed to %s, but the data was not changed: %s", name, reg_error(e));
            strlcpy(sel_value_name, name, sizeof(sel_value_name));
            close_dialog();
            read_values();
            return;
        }
        failed_message("Not saved", e);
        return;
    }
    set_status(false, "%s %s", dlg.is_new ? "Made" : "Saved", name);
    strlcpy(sel_value_name, name, sizeof(sel_value_name));
    focus = FOCUS_VALUES;
    close_dialog();
    read_values();
    scroll_to_selection();
}

static void finish_name(void) {
    char name[NAME_LEN];
    strlcpy(name, dlg.name_box.text, sizeof(name));
    if (!valid_name(name, dlg.message, sizeof(dlg.message)))
        return;

    if (dlg.purpose == NAME_NEW_KEY) {
        char path[PATH_LEN];
        if (!join_path(path, sizeof(path), dlg.key, name)) {
            strlcpy(dlg.message, "The path would be too long", sizeof(dlg.message));
            return;
        }
        change_t c = { .path = path };
        if (with_admin(path, do_mkkey, &c) < 0) {
            failed_message("Not made", errno);
            return;
        }
        set_open(dlg.key, true);
        pick_key(path);
        focus = FOCUS_TREE;
        set_status(false, "Made the key %s", name);
    } else if (dlg.purpose == NAME_RENAME_KEY) {
        char old_path[PATH_LEN], new_path[PATH_LEN];
        if (!join_path(old_path, sizeof(old_path), dlg.key, dlg.old_name) ||
            !join_path(new_path, sizeof(new_path), dlg.key, name)) {
            strlcpy(dlg.message, "The path would be too long", sizeof(dlg.message));
            return;
        }
        change_t c = { .path = old_path, .name = NULL, .new_name = name };
        if (with_admin(old_path, do_rename, &c) < 0) {
            failed_message("Not renamed", errno);
            return;
        }
        // Keys that were open below it are open under the new name
        for (int i = 0; i < open_count; i++) {
            if (path_within(open_paths[i], old_path)) {
                char moved[PATH_LEN];
                snprintf(moved, sizeof(moved), "%s%s", new_path, open_paths[i] + strlen(old_path));
                strlcpy(open_paths[i], moved, PATH_LEN);
            }
        }
        pick_key(new_path);
        set_status(false, "Renamed %s to %s", dlg.old_name, name);
    } else {
        change_t c = { .path = dlg.key, .name = dlg.old_name, .new_name = name };
        if (with_admin(dlg.key, do_rename, &c) < 0) {
            failed_message("Not renamed", errno);
            return;
        }
        strlcpy(sel_value_name, name, sizeof(sel_value_name));
        set_status(false, "Renamed %s to %s", dlg.old_name, name);
    }
    close_dialog();
    read_tree();
    scroll_to_selection();
}

static void finish_delete(void) {
    change_t c = { .path = dlg.key, .name = dlg.delete_key ? NULL : dlg.old_name };
    if (with_admin(dlg.key, do_delete, &c) < 0) {
        if (errno != ECANCELED)         // A Cancel at the prompt needs no word
            set_status(true, "Not deleted: %s", reg_error(errno));
        close_dialog();
        read_tree();
        return;
    }
    if (dlg.delete_key) {
        set_status(false, "Deleted the key %s", last_name(dlg.key));
        forget_open_below(dlg.key);
        char parent[PATH_LEN];
        parent_path(dlg.key, parent, sizeof(parent));
        pick_key(parent);
    } else {
        set_status(false, "Deleted %s", dlg.old_name);
        // Pick the value that took its place
        int next = sel_value + 1 < value_count ? sel_value + 1 : sel_value - 1;
        sel_value_name[0] = '\0';
        if (next >= 0 && next < value_count)
            strlcpy(sel_value_name, values[next].name, sizeof(sel_value_name));
    }
    close_dialog();
    read_tree();
    scroll_to_selection();
}

static void dialog_accept(void) {
    dlg.message[0] = dlg.kind == DLG_CONFIRM ? dlg.message[0] : '\0';
    switch (dlg.kind) {
        case DLG_VALUE:   finish_value(); break;
        case DLG_NAME:    finish_name(); break;
        case DLG_CONFIRM: finish_delete(); break;
        default:          break;
    }
    dirty = true;
}

//
// Dialog input
//

static ui_edit_t* dialog_box(void) {
    if (dlg.kind == DLG_NAME)
        return &dlg.name_box;
    if (dlg.kind == DLG_VALUE)
        return dlg.focus == 0 ? &dlg.name_box : &dlg.data_box;
    return NULL;
}

static void show_type_menu(void) {
    gfx_rect_t f = dlg_type_rect();
    wm_menu_item_t items[] = {
        { "String", CMD_TYPE_STRING, dlg.type == TYPE_STRING ? WM_MENU_CHECKED : 0 },
        { "Binary", CMD_TYPE_BINARY, dlg.type == TYPE_BINARY ? WM_MENU_CHECKED : 0 },
    };
    wm_menu_show(win, f.x, f.y + f.h, items, 2);
}

// Switch the value dialog's type, turning what is typed into the other form
static void set_dialog_type(vtype_t type) {
    if (dlg.kind != DLG_VALUE || type == dlg.type)
        return;
    char text[UI_EDIT_MAX];
    if (type == TYPE_BINARY) {
        // The text's bytes, with its '\0' as a string would store it
        size_t n = strlen(dlg.data_box.text) + 1;
        if (n * 3 > UI_EDIT_MAX) {
            strlcpy(dlg.message, "Too long to show as bytes here", sizeof(dlg.message));
            return;
        }
        format_hex((const uint8_t*)dlg.data_box.text, n, n, text, sizeof(text));
    } else {
        uint8_t bytes[UI_EDIT_MAX];
        char why[64];
        int n = parse_hex(dlg.data_box.text, bytes, sizeof(bytes) - 1, why, sizeof(why));
        if (n < 0) {
            text[0] = '\0';
        } else {
            if (n > 0 && bytes[n - 1] == '\0') n--;
            for (int i = 0; i < n; i++)
                if (bytes[i] < 0x20 || bytes[i] >= 0x7F) bytes[i] = '?';
            memcpy(text, bytes, (size_t)n);
            text[n] = '\0';
        }
    }
    dlg.type = type;
    set_dialog_title();
    ui_edit_init(&dlg.data_box, text, UI_EDIT_MAX - 1, 0);
    dlg.message[0] = '\0';
    dirty = true;
}

static void dialog_key(const wm_event_t* ev) {
    if (ev->key == KEY_TAB && dlg.kind == DLG_VALUE) {
        dlg.focus = dlg.focus == 0 ? 1 : 0;
        ui_edit_select_all(dialog_box());
        dirty = true;
        return;
    }
    ui_edit_t* box = dialog_box();
    if (box == NULL) {
        if (ev->key == KEY_ESCAPE)
            close_dialog();
        else if (ev->key == KEY_ENTER || ev->key == '\r' || ev->key == '\n')
            dialog_accept();
        return;
    }
    switch (ui_edit_key(box, ev->key, ev->mods)) {
        case UI_EDIT_ACCEPT:
            dialog_accept();
            break;
        case UI_EDIT_CANCEL:
            close_dialog();
            break;
        default:
            break;
    }
    dirty = true;
}

static void dialog_mouse_down(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return;
    int32_t x = ev->x, y = ev->y;
    dlg.dragging = false;
    if (in_rect(dlg_ok_rect(), x, y)) {
        dialog_accept();
    } else if (in_rect(dlg_cancel_rect(), x, y)) {
        close_dialog();
    } else if ((dlg.kind == DLG_NAME || dlg.kind == DLG_VALUE) &&
               ui_edit_click(&dlg.name_box, x, y, ev->mods, ev->clicks)) {
        dlg.focus = 0;
        dlg.dragging = true;
    } else if (dlg.kind == DLG_VALUE && ui_edit_click(&dlg.data_box, x, y, ev->mods, ev->clicks)) {
        dlg.focus = 1;
        dlg.dragging = true;
    } else if (dlg.kind == DLG_VALUE && in_rect(dlg_type_rect(), x, y)) {
        show_type_menu();
    }
    dirty = true;
}

//
// Main window input
//

static void select_row(int r) {
    if (row_count == 0)
        return;
    if (r < 0) r = 0;
    if (r >= row_count) r = row_count - 1;
    sel_row = r;
    pick_key(rows[r].path);
    read_values();
    status[0] = '\0';
    dirty = true;
}

static void select_value(int v) {
    if (value_count == 0) {
        sel_value = -1;
        return;
    }
    if (v < 0) v = 0;
    if (v >= value_count) v = value_count - 1;
    sel_value = v;
    strlcpy(sel_value_name, values[v].name, sizeof(sel_value_name));
    dirty = true;
}

static void toggle_row(int r) {
    if (r < 0 || r >= row_count || !rows[r].has_children)
        return;
    set_open(rows[r].path, !rows[r].open);
    // Closing a key with one below it picked picks it
    if (rows[r].open && path_within(sel_path, rows[r].path))
        pick_key(rows[r].path);
    read_tree();
}

static void go_to_address(void) {
    char path[PATH_LEN];
    strlcpy(path, address_box.text, sizeof(path));
    tidy_path(path);
    char name[NAME_LEN];
    bool found = path[0] != '\0' && os_reg_key(path, 0, name, sizeof(name)) >= 0;
    if (!found) {
        int e = errno;
        set_status(true, "No key %s%s", path, e == EACCES ? " that can be read" : "");
        return;
    }
    reveal(path);
    pick_key(path);
    read_tree();
    focus = FOCUS_TREE;
    scroll_to_selection();
    set_status(false, "%s", sel_path);
}

static void copy_path(void) {
    if (sel_path[0] == '\0')
        return;
    wm_clip_set_text(sel_path);
    set_status(false, "Copied %s", sel_path);
}

static void copy_data(void) {
    const value_t* v = picked_value();
    if (v == NULL || v->error != 0)
        return;
    uint8_t* buf = malloc(VALUE_MAX + 1);
    char* text = malloc(VALUE_MAX * 3 + 4);
    ssize_t n = (buf != NULL && text != NULL) ? read_value(sel_path, v->name, buf) : -1;
    if (n >= 0) {
        if (guess_type(buf, (size_t)n) == TYPE_STRING) {
            buf[n] = '\0';
            wm_clip_set_text((const char*)buf);
        } else {
            format_hex(buf, (size_t)n, (size_t)n, text, VALUE_MAX * 3 + 4);
            wm_clip_set_text(text);
        }
        set_status(false, "Copied the data of %s", v->name);
    }
    free(buf);
    free(text);
}

static void refresh(void) {
    read_tree();
    scroll_to_selection();
}

static void run_command(uint32_t id) {
    switch (id) {
        case CMD_NEW_KEY:     start_new_key(); break;
        case CMD_NEW_STRING:  start_new_value(TYPE_STRING); break;
        case CMD_NEW_BINARY:  start_new_value(TYPE_BINARY); break;
        case CMD_MODIFY:      start_modify(); break;
        case CMD_RENAME:      start_rename(); break;
        case CMD_DELETE:      start_delete(); break;
        case CMD_COPY_PATH:   copy_path(); break;
        case CMD_COPY_DATA:   copy_data(); break;
        case CMD_EXPAND:
            if (sel_row >= 0 && !rows[sel_row].open) toggle_row(sel_row);
            break;
        case CMD_COLLAPSE:
            if (sel_row >= 0 && rows[sel_row].open) toggle_row(sel_row);
            break;
        case CMD_REFRESH:     refresh(); set_status(false, "Read the registry again"); break;
        case CMD_TYPE_STRING: set_dialog_type(TYPE_STRING); break;
        case CMD_TYPE_BINARY: set_dialog_type(TYPE_BINARY); break;
    }
    dirty = true;
}

static void show_menu(int32_t x, int32_t y) {
    bool can_key = can_change_key();
    bool not_hive = sel_row >= 0 && !path_is_hive(sel_path);
    if (focus == FOCUS_VALUES) {
        bool v = picked_value() != NULL;
        // WM_MENU_MAX_ITEMS (10) at most, separators included
        wm_menu_item_t items[] = {
            { "Modify...",           CMD_MODIFY,     v ? 0 : WM_MENU_DISABLED },
            { "Rename",              CMD_RENAME,     v ? 0 : WM_MENU_DISABLED },
            { "Delete",              CMD_DELETE,     v ? 0 : WM_MENU_DISABLED },
            { "Copy data",           CMD_COPY_DATA,  v ? 0 : WM_MENU_DISABLED },
            { NULL,                  0,              WM_MENU_SEPARATOR },
            { "New string value",    CMD_NEW_STRING, can_key ? 0 : WM_MENU_DISABLED },
            { "New binary value",    CMD_NEW_BINARY, can_key ? 0 : WM_MENU_DISABLED },
            { NULL,                  0,              WM_MENU_SEPARATOR },
            { "Refresh",             CMD_REFRESH,    0 },
        };
        wm_menu_show(win, x, y, items, sizeof(items) / sizeof(items[0]));
    } else {
        bool kids = sel_row >= 0 && rows[sel_row].has_children;
        bool open = kids && rows[sel_row].open;
        // WM_MENU_MAX_ITEMS (10) at most, separators included
        wm_menu_item_t items[] = {
            { open ? "Collapse" : "Expand", open ? CMD_COLLAPSE : CMD_EXPAND, kids ? 0 : WM_MENU_DISABLED },
            { "New key",             CMD_NEW_KEY,    can_key ? 0 : WM_MENU_DISABLED },
            { "New string value",    CMD_NEW_STRING, can_key ? 0 : WM_MENU_DISABLED },
            { "New binary value",    CMD_NEW_BINARY, can_key ? 0 : WM_MENU_DISABLED },
            { NULL,                  0,              WM_MENU_SEPARATOR },
            { "Rename",              CMD_RENAME,     not_hive ? 0 : WM_MENU_DISABLED },
            { "Delete",              CMD_DELETE,     not_hive ? 0 : WM_MENU_DISABLED },
            { "Copy key path",       CMD_COPY_PATH,  sel_row >= 0 ? 0 : WM_MENU_DISABLED },
            { NULL,                  0,              WM_MENU_SEPARATOR },
            { "Refresh",             CMD_REFRESH,    0 },
        };
        wm_menu_show(win, x, y, items, sizeof(items) / sizeof(items[0]));
    }
}

static void tree_key(const wm_event_t* ev) {
    int page = tree_visible() - 1 > 1 ? tree_visible() - 1 : 1;
    switch (ev->key) {
        case KEY_UP:        select_row(sel_row - 1); break;
        case KEY_DOWN:      select_row(sel_row + 1); break;
        case KEY_HOME:      select_row(0); break;
        case KEY_END:       select_row(row_count - 1); break;
        case KEY_PAGE_UP:   select_row(sel_row - page); break;
        case KEY_PAGE_DOWN: select_row(sel_row + page); break;
        case KEY_RIGHT:
            if (sel_row >= 0 && rows[sel_row].has_children) {
                if (!rows[sel_row].open) toggle_row(sel_row);
                else select_row(sel_row + 1);
            }
            break;
        case KEY_LEFT:
            if (sel_row >= 0) {
                if (rows[sel_row].open) {
                    toggle_row(sel_row);
                } else if (!path_is_hive(sel_path)) {
                    char parent[PATH_LEN];
                    parent_path(sel_path, parent, sizeof(parent));
                    pick_key(parent);
                    read_tree();
                }
            }
            break;
        case KEY_ENTER: case '\r': case '\n': case ' ':
            toggle_row(sel_row);
            break;
        default:
            return;
    }
    scroll_to_selection();
}

static void values_key(const wm_event_t* ev) {
    int page = values_visible() - 1 > 1 ? values_visible() - 1 : 1;
    switch (ev->key) {
        case KEY_UP:        select_value(sel_value - 1); break;
        case KEY_DOWN:      select_value(sel_value + 1); break;
        case KEY_HOME:      select_value(0); break;
        case KEY_END:       select_value(value_count - 1); break;
        case KEY_PAGE_UP:   select_value(sel_value - page); break;
        case KEY_PAGE_DOWN: select_value(sel_value + page); break;
        case KEY_LEFT:      focus = FOCUS_TREE; dirty = true; break;
        case KEY_ENTER: case '\r': case '\n':
            start_modify();
            break;
        default:
            return;
    }
    scroll_to_selection();
}

static void main_key(const wm_event_t* ev) {
    if (ev->mods & INPUT_MOD_CTRL) {
        switch (ev->key) {
            case 'n': case 'N': start_new_key(); return;
            case 's': case 'S': start_new_value(TYPE_STRING); return;
            case 'b': case 'B': start_new_value(TYPE_BINARY); return;
            case 'c': case 'C':
                if (focus == FOCUS_VALUES) copy_data(); else if (focus == FOCUS_TREE) copy_path();
                if (focus != FOCUS_ADDRESS) return;
                break;
            case 'l': case 'L':
                focus = FOCUS_ADDRESS;
                ui_edit_select_all(&address_box);
                dirty = true;
                return;
            case 'q': case 'Q': running = false; return;
        }
    }

    if (ev->key == KEY_TAB) {
        bool back = (ev->mods & INPUT_MOD_SHIFT) != 0;
        focus = (focus_t)((focus + (back ? 2 : 1)) % 3);
        if (focus == FOCUS_VALUES && sel_value < 0 && value_count > 0)
            select_value(0);
        if (focus == FOCUS_ADDRESS)
            ui_edit_select_all(&address_box);
        dirty = true;
        return;
    }
    if (ev->key == KEY_F5) {
        run_command(CMD_REFRESH);
        return;
    }

    if (focus == FOCUS_ADDRESS) {
        switch (ui_edit_key(&address_box, ev->key, ev->mods)) {
            case UI_EDIT_ACCEPT: go_to_address(); break;
            case UI_EDIT_CANCEL: pick_key(sel_path); focus = FOCUS_TREE; break;
            default: break;
        }
        dirty = true;
        return;
    }

    if (ev->key == KEY_ESCAPE) {
        running = false;
        return;
    }
    if (ev->key == KEY_F2) {
        start_rename();
        return;
    }
    if (ev->key == KEY_DELETE) {
        start_delete();
        return;
    }
    if (focus == FOCUS_TREE)
        tree_key(ev);
    else
        values_key(ev);
}

static void scroll_thumb_to(int which, int32_t pointer_y) {
    gfx_rect_t r = which == 1 ? tree_rect() : values_rect();
    int32_t track_y = which == 1 ? r.y : r.y + header_h();
    int32_t track = which == 1 ? r.h : r.h - header_h();
    int count = which == 1 ? row_count : value_count;
    int visible = which == 1 ? tree_visible() : values_visible();
    int* top = which == 1 ? &tree_top : &value_top;
    int32_t ty, th;
    thumb_rect(count, visible, *top, track, &ty, &th);
    int32_t span = track - th;
    int max = count - visible;
    if (span <= 0 || max <= 0)
        return;
    int32_t at = pointer_y - track_y - thumb_grab;
    *top = (int)((at * max + span / 2) / span);
    clamp_scrolls();
    dirty = true;
}

// A click on a scroll bar (which: 1 tree, 2 values). True if it was one.
static bool scrollbar_click(int which, const wm_event_t* ev) {
    gfx_rect_t r = which == 1 ? tree_rect() : values_rect();
    int count = which == 1 ? row_count : value_count;
    int visible = which == 1 ? tree_visible() : values_visible();
    if (count <= visible || ev->x < r.x + r.w - SCROLL_W)
        return false;
    int32_t track_y = which == 1 ? r.y : r.y + header_h();
    int32_t track = which == 1 ? r.h : r.h - header_h();
    int* top = which == 1 ? &tree_top : &value_top;
    int32_t ty, th;
    thumb_rect(count, visible, *top, track, &ty, &th);
    int32_t y = ev->y - track_y;
    if (y < ty) {
        *top -= visible;
    } else if (y >= ty + th) {
        *top += visible;
    } else {
        thumb_drag = which;
        thumb_grab = y - ty;
    }
    clamp_scrolls();
    dirty = true;
    return true;
}

static void main_mouse_down(const wm_event_t* ev) {
    int32_t x = ev->x, y = ev->y;
    bool left = ev->button == INPUT_BTN_LEFT;
    address_drag = false;

    // Address bar
    if (y < address_h()) {
        if (left && ui_edit_click(&address_box, x, y, ev->mods, ev->clicks)) {
            focus = FOCUS_ADDRESS;
            address_drag = true;
        }
        dirty = true;
        return;
    }
    if (y >= pane_top() + pane_h())
        return;

    // The divider
    if (left && x >= split_x - SPLIT_GRAB && x <= split_x + SPLIT_GRAB) {
        split_drag = true;
        dirty = true;
        return;
    }

    if (in_rect(tree_rect(), x, y)) {
        focus = FOCUS_TREE;
        if (left && scrollbar_click(1, ev))
            return;
        int r = tree_top + (y - pane_top()) / row_h;
        if (r < row_count) {
            select_row(r);
            int32_t ex = 4 + rows[r].depth * INDENT;
            if (left && (ev->clicks >= 2 || (x >= ex && x < ex + EXPANDER_W)))
                toggle_row(r);
        }
        scroll_to_selection();
        dirty = true;
        return;
    }

    if (in_rect(values_rect(), x, y)) {
        focus = FOCUS_VALUES;
        if (y < pane_top() + header_h())
            return;
        if (left && scrollbar_click(2, ev))
            return;
        int v = value_top + (y - pane_top() - header_h()) / row_h;
        if (v < value_count) {
            select_value(v);
            if (left && ev->clicks >= 2)
                start_modify();
        } else {
            sel_value = -1;
            sel_value_name[0] = '\0';
        }
        dirty = true;
    }
}

// Wheel: scrolls the pane under the pointer (the selection stays put)
static void main_wheel(const wm_event_t* ev) {
    if (ev->wheel == 0 || thumb_drag || split_drag)
        return;
    int which;
    if (in_rect(tree_rect(), ev->x, ev->y))
        which = 1;
    else if (in_rect(values_rect(), ev->x, ev->y))
        which = 2;
    else
        return;
    int visible = which == 1 ? tree_visible() : values_visible();
    int* top = which == 1 ? &tree_top : &value_top;
    int step = (ev->mods & INPUT_MOD_CTRL) ? visible : WHEEL_ROWS;
    int before = *top;
    *top += ev->wheel * step;
    clamp_scrolls();
    if (*top != before)
        dirty = true;
}

static void main_event(const wm_event_t* ev) {
    if (dlg.kind != DLG_NONE) {
        switch (ev->type) {
            case WM_EVENT_KEY:        dialog_key(ev); return;
            case WM_EVENT_MOUSE_DOWN: dialog_mouse_down(ev); return;
            case WM_EVENT_MOUSE_MOVE:
                if (dlg.dragging && (ev->buttons & INPUT_BTN_LEFT) && dialog_box() != NULL) {
                    ui_edit_drag(dialog_box(), ev->x);
                    dirty = true;
                }
                return;
            case WM_EVENT_MOUSE_UP:   dlg.dragging = false; return;
            case WM_EVENT_CONTEXT_MENU: return;
            default: break;
        }
    }

    switch (ev->type) {
        case WM_EVENT_KEY:
            main_key(ev);
            break;

        case WM_EVENT_MOUSE_DOWN:
            main_mouse_down(ev);
            break;

        case WM_EVENT_MOUSE_MOVE:
            if (split_drag) {
                if (ev->buttons & INPUT_BTN_LEFT) {
                    split_x = ev->x;
                    clamp_split();
                } else {
                    split_drag = false;
                }
                dirty = true;
            } else if (thumb_drag) {
                if (ev->buttons & INPUT_BTN_LEFT)
                    scroll_thumb_to(thumb_drag, ev->y);
                else
                    thumb_drag = 0;
            } else if (address_drag && (ev->buttons & INPUT_BTN_LEFT)) {
                ui_edit_drag(&address_box, ev->x);
                dirty = true;
            }
            break;

        case WM_EVENT_MOUSE_UP:
            if (thumb_drag || split_drag) {
                thumb_drag = 0;
                split_drag = false;
                dirty = true;
            }
            address_drag = false;
            break;

        case WM_EVENT_MOUSE_WHEEL:
            main_wheel(ev);
            break;

        case WM_EVENT_CONTEXT_MENU: {
            // Right click picks what is under the pointer first
            wm_event_t pick = *ev;
            pick.type = WM_EVENT_MOUSE_DOWN;
            pick.button = INPUT_BTN_RIGHT;
            pick.clicks = 1;
            main_mouse_down(&pick);
            if (ev->y >= pane_top() && ev->y < pane_top() + pane_h())
                show_menu(ev->x, ev->y);
            break;
        }

        case WM_EVENT_MENU_COMMAND:
            run_command(ev->param);
            scroll_to_selection();
            break;

        case WM_EVENT_FOCUS_GAINED:
            // Another program may have changed the registry meanwhile
            if (dlg.kind == DLG_NONE)
                refresh();
            break;

        case WM_EVENT_RESIZED:
            win_w = ev->rect_w;
            win_h = ev->rect_h;
            clamp_split();
            dirty = true;
            break;

        case WM_EVENT_CLOSE:
            running = false;
            break;

        case WM_EVENT_DESTROYED:
            win = 0;
            running = false;
            break;
    }
}

//
// Main
//

int main(int argc, char** argv) {
    if (argc > 2) {
        fputs("usage: " APP_NAME " [key path]\n", stderr);
        return 2;
    }
    rows = calloc(MAX_ROWS, sizeof(row_t));
    values = calloc(MAX_VALUES, sizeof(value_t));
    if (rows == NULL || values == NULL) {
        fputs(APP_NAME ": out of memory\n", stderr);
        return 1;
    }
    load_theme();

    // Start at the key named, else hkey_user, with the hives open
    set_open(HIVE_ROOT, true);
    set_open(HIVE_USER, true);
    strlcpy(sel_path, HIVE_USER, sizeof(sel_path));
    if (argc == 2) {
        char path[PATH_LEN];
        strlcpy(path, argv[1], sizeof(path));
        tidy_path(path);
        reveal(path);
        strlcpy(sel_path, path, sizeof(sel_path));
    }
    read_tree();
    pick_key(sel_path);
    if (argc == 2 && strcmp(sel_path, argv[1]) != 0)
        set_status(true, "No key %s: showing %s", argv[1], sel_path);

    int32_t sw = 0, sh = 0;
    wm_screen_size(&sw, &sh);
    if (sw > 0 && win_w + 32 > sw) win_w = sw - 32;
    if (sh > 0 && win_h + 64 > sh) win_h = sh - 64;

    wm_window_desc_t d;
    memset(&d, 0, sizeof(d));
    d.title      = "Registry Editor";
    d.x          = WM_POS_DEFAULT;
    d.y          = WM_POS_DEFAULT;
    d.width      = win_w;
    d.height     = win_h;
    d.min_width  = MIN_W;
    d.min_height = MIN_H;
    d.style      = WM_STYLE_RESIZABLE | WM_STYLE_CONTEXT_MENU;

    win = wm_window_create(&d);
    if (win == 0) {
        fprintf(stderr, APP_NAME ": can't create a window: %s\n", strerror(errno));
        wm_disconnect();
        return 1;
    }
    wm_window_get_size(win, &win_w, &win_h);
    draw();
    scroll_to_selection();
    wm_window_focus(win);

    wm_event_t ev;
    while (running) {
        int r = wm_wait_event(&ev, dirty ? 0 : -1);
        if (r < 0)
            break;                          // The dwm went away
        if (r == 0) {
            if (dirty) draw();
            continue;
        }
        if (ev.type == WM_EVENT_THEME_CHANGED) {
            load_theme();
            dirty = true;
        } else if (ev.window == win) {
            main_event(&ev);
        }
    }

    if (win != 0)
        wm_window_destroy(win);
    wm_disconnect();
    return 0;
}

/*
 * The standard Open / Save as dialog (os/filedlg.h).
 *
 * One window: the folder's path and an Up button along the top, the list of
 * its folders and files (folders first, each group by name), the name field
 * (an os/ui.h edit box) and the buttons along the bottom, with a status line
 * left of the buttons for "no such file" and the replace question.
 *
 * Everything is placed by layout() from the window's size and the font, at
 * the start of every paint and every mouse event, so what is drawn and what
 * is hit-tested always agree, also after a resize or a font change.
 *
 * The mouse wheel scrolls the list WHEEL_ROWS rows a notch (Ctrl: a page),
 * leaving the selection where it is.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <os/os.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/ui.h>
#include <os/wm.h>
#include <os/filedlg.h>

#define DLG_W           440
#define DLG_H           320
#define DLG_MIN_W       300
#define DLG_MIN_H       220

#define MARGIN          10
#define GAP             8
#define BTN_W           72
#define BTN_UP_W        40
#define SB_W            12          // Scroll bar
#define SB_MIN_THUMB    12
#define WHEEL_ROWS      3           // Rows a wheel notch scrolls (Ctrl: a page)
#define ICON_W          12          // Folder / file glyph
#define ROW_PAD         4           // Above and below a row's text

#define FOLDER_COLOR    GFX_RGB(226, 184, 82)   // As explorer's fallback folder icon

enum { BTN_NONE = -1, BTN_OK, BTN_CANCEL, BTN_UP, BTN_COUNT };

typedef struct {
    char* name;
    bool  dir;
} entry_t;

typedef struct {
    gfx_rect_t path;                // The folder's path text
    gfx_rect_t button[BTN_COUNT];
    gfx_rect_t list;                // Frame of the list
    gfx_rect_t rows;                // Inside it, less the scroll bar
    gfx_rect_t bar;                 // Scroll bar (w 0: none)
    gfx_rect_t thumb;
    gfx_rect_t status;
    int32_t    name_label_x, name_y, name_h;
    int32_t    row_h;
    int32_t    visible;             // Whole rows that fit
} layout_t;

struct filedlg {
    filedlg_mode_t mode;
    wm_window_t    win;
    wm_window_t    owner;
    wm_theme_t     theme;
    bool           dirty;

    char           dir[PATH_MAX];   // The folder shown, absolute
    char           result[PATH_MAX];
    char           confirm[PATH_MAX];   // Save: the existing file the user was asked about
    char           suggest[NAME_MAX + 1];   // Save: the name offered

    entry_t*       items;
    size_t         count, cap;
    int32_t        sel;             // Selected row, -1: none
    int32_t        top;             // First row shown

    ui_edit_t      name;
    bool           list_active;     // The list has the keyboard (else the name field)
    bool           name_drag;       // Selecting in the name field with the button held
    int            pressed;         // BTN_* held down, BTN_NONE
    bool           thumb_held;
    int32_t        thumb_grab;      // Where on the thumb it was taken

    char           status[128];
    bool           status_error;
    layout_t       lay;
};

//
// Helpers
//

static bool in_rect(const gfx_rect_t* r, int32_t x, int32_t y) {
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static void set_status(filedlg_t* d, bool error, const char* text) {
    strlcpy(d->status, text ? text : "", sizeof(d->status));
    d->status_error = error;
    d->dirty = true;
}

static void set_name(filedlg_t* d, const char* text) {
    ui_edit_init(&d->name, text, NAME_MAX, 0);
    d->dirty = true;
}

// `dir` + "/" + `name` (or `name` alone when it is absolute), made absolute
// and folded. NULL with errno when it doesn't fit.
static char* join_path(const char* dir, const char* name, char* out, size_t size) {
    char tmp[PATH_MAX];
    if (name[0] == '/')
        strlcpy(tmp, name, sizeof(tmp));
    else if (snprintf(tmp, sizeof(tmp), "%s%s%s", dir, (strcmp(dir, "/") == 0) ? "" : "/", name)
             >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return os_abspath(tmp, out, size);
}

static void load_theme(filedlg_t* d) {
    if (wm_get_theme(&d->theme) == 0)
        return;
    wm_theme_t* t = &d->theme;
    memset(t, 0, sizeof(*t));
    t->client.background     = GFX_RGB(236, 236, 236);
    t->client.text           = GFX_RGB(20, 20, 20);
    t->client.text_soft      = GFX_RGB(80, 80, 80);
    t->client.text_danger    = GFX_RGB(190, 30, 30);
    t->button.fill           = GFX_RGB(222, 222, 222);
    t->button.border         = GFX_RGB(140, 140, 140);
    t->button.border_active  = GFX_RGB(40, 100, 200);
    t->button.text           = GFX_RGB(20, 20, 20);
    t->button.fill_pressed   = GFX_RGB(190, 190, 190);
    t->button.border_pressed = GFX_RGB(90, 90, 90);
    t->edit.background       = GFX_RGB(255, 255, 255);
    t->edit.border           = GFX_RGB(120, 120, 120);
    t->edit.text             = GFX_RGB(20, 20, 20);
    t->edit.cursor           = GFX_RGB(20, 20, 20);
    t->edit.selection        = GFX_RGB(170, 200, 240);
}

//
// The folder
//

static void free_items(filedlg_t* d) {
    for (size_t i = 0; i < d->count; i++)
        free(d->items[i].name);
    d->count = 0;
}

static int compare_entries(const void* a, const void* b) {
    const entry_t* x = (const entry_t*)a;
    const entry_t* y = (const entry_t*)b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    int c = strcasecmp(x->name, y->name);
    return c != 0 ? c : strcmp(x->name, y->name);
}

static int32_t find_item(const filedlg_t* d, const char* name) {
    for (size_t i = 0; i < d->count; i++)
        if (strcmp(d->items[i].name, name) == 0)
            return (int32_t)i;
    return -1;
}

static bool add_item(filedlg_t* d, const char* name, bool dir) {
    if (d->count == d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 64;
        entry_t* items = (entry_t*)realloc(d->items, cap * sizeof(entry_t));
        if (items == NULL)
            return false;
        d->items = items;
        d->cap = cap;
    }
    char* copy = strdup(name);
    if (copy == NULL)
        return false;
    d->items[d->count].name = copy;
    d->items[d->count].dir = dir;
    d->count++;
    return true;
}

// Show the folder at `path` (absolute), selecting `select` if it is there.
// On failure the folder shown stays and the status says why.
static bool load_dir(filedlg_t* d, const char* path, const char* select) {
    DIR* dir = opendir(path);
    if (dir == NULL) {
        char msg[sizeof(d->status)];
        snprintf(msg, sizeof(msg), "Can't open that folder: %s", strerror(errno));
        set_status(d, true, msg);
        return false;
    }

    free_items(d);
    bool full = false;
    if (strcmp(path, "/") != 0)
        add_item(d, "..", true);
    struct dirent* e;
    while ((e = readdir(dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (!add_item(d, e->d_name, e->d_type == DT_DIR)) {
            full = true;
            break;
        }
    }
    closedir(dir);

    // ".." stays first
    size_t skip = (d->count > 0 && strcmp(d->items[0].name, "..") == 0) ? 1 : 0;
    if (d->count > skip)
        qsort(d->items + skip, d->count - skip, sizeof(entry_t), compare_entries);

    if (path != d->dir)
        strlcpy(d->dir, path, sizeof(d->dir));
    d->sel = select ? find_item(d, select) : -1;
    d->top = 0;
    d->confirm[0] = '\0';
    set_status(d, full, full ? "Not enough memory to list all of it" : NULL);
    return true;
}

static void go_up(filedlg_t* d) {
    if (strcmp(d->dir, "/") == 0)
        return;
    char parent[PATH_MAX], child[NAME_MAX + 1];
    strlcpy(parent, d->dir, sizeof(parent));
    char* slash = strrchr(parent, '/');
    if (slash == NULL)
        return;
    strlcpy(child, slash + 1, sizeof(child));
    if (slash == parent)
        slash[1] = '\0';            // "/mnt" -> "/"
    else
        slash[0] = '\0';
    load_dir(d, parent, child);
}

//
// Layout
//

static void layout(filedlg_t* d, gfx_surface_t* s) {
    layout_t* L = &d->lay;
    int32_t w = 0, h = 0;
    wm_window_get_size(d->win, &w, &h);
    int32_t lh = gfx_line_height(s);
    int32_t btn_h = lh + 10;

    // Top: the path, then Up
    L->button[BTN_UP] = gfx_rect(w - MARGIN - BTN_UP_W, MARGIN, BTN_UP_W, btn_h);
    L->path = gfx_rect(MARGIN, MARGIN, L->button[BTN_UP].x - GAP - MARGIN, btn_h);

    // Bottom: the status, then OK and Cancel
    int32_t by = h - MARGIN - btn_h;
    L->button[BTN_CANCEL] = gfx_rect(w - MARGIN - BTN_W, by, BTN_W, btn_h);
    L->button[BTN_OK] = gfx_rect(L->button[BTN_CANCEL].x - GAP - BTN_W, by, BTN_W, btn_h);
    L->status = gfx_rect(MARGIN, by, L->button[BTN_OK].x - GAP - MARGIN, btn_h);

    // Above them: "Name:" and the field
    L->name_h = lh + 6;
    L->name_y = by - GAP - L->name_h;
    L->name_label_x = MARGIN;
    int32_t fx = MARGIN + gfx_text_width(s, "Name:") + GAP;
    int32_t fw = w - MARGIN - fx;
    if (fw < 2 * gfx_char_width(s)) fw = 2 * gfx_char_width(s);
    ui_edit_place(&d->name, fx + fw / 2, L->name_y + 3, fw, fw, gfx_rect(fx, 0, fw, h));

    // The rest: the list
    int32_t ly = MARGIN + btn_h + GAP;
    L->list = gfx_rect(MARGIN, ly, w - 2 * MARGIN, L->name_y - GAP - ly);
    if (L->list.h < lh + 2 * ROW_PAD + 2) L->list.h = lh + 2 * ROW_PAD + 2;
    L->row_h = lh + 2 * ROW_PAD;
    L->rows = gfx_rect(L->list.x + 1, L->list.y + 1, L->list.w - 2, L->list.h - 2);
    L->visible = L->rows.h / L->row_h;
    if (L->visible < 1) L->visible = 1;

    // Scroll bar when it doesn't all fit
    L->bar = gfx_rect(0, 0, 0, 0);
    L->thumb = L->bar;
    int32_t count = (int32_t)d->count;
    if (count > L->visible) {
        L->rows.w -= SB_W;
        L->bar = gfx_rect(L->rows.x + L->rows.w, L->rows.y, SB_W, L->rows.h);
        int32_t th = L->bar.h * L->visible / count;
        if (th < SB_MIN_THUMB) th = SB_MIN_THUMB;
        if (th > L->bar.h) th = L->bar.h;
        int32_t range = count - L->visible;
        if (d->top > range) d->top = range;
        int32_t ty = L->bar.y + (int32_t)((int64_t)(L->bar.h - th) * d->top / range);
        L->thumb = gfx_rect(L->bar.x + 1, ty, SB_W - 2, th);
    } else {
        d->top = 0;
    }
}

static void scroll_to_sel(filedlg_t* d) {
    if (d->sel < 0)
        return;
    int32_t vis = d->lay.visible > 0 ? d->lay.visible : 1;
    if (d->sel < d->top)
        d->top = d->sel;
    else if (d->sel >= d->top + vis)
        d->top = d->sel - vis + 1;
}

static void scroll_by(filedlg_t* d, int32_t rows) {
    int32_t max = (int32_t)d->count - d->lay.visible;
    if (max < 0) max = 0;
    d->top += rows;
    if (d->top > max) d->top = max;
    if (d->top < 0) d->top = 0;
    d->dirty = true;
}

//
// Painting
//

static void draw_button(filedlg_t* d, gfx_surface_t* s, int i, const char* label, bool def) {
    const gfx_rect_t* r = &d->lay.button[i];
    const wm_theme_t* t = &d->theme;
    bool pressed = (d->pressed == i);
    gfx_fill_rect(s, r->x, r->y, r->w, r->h, pressed ? t->button.fill_pressed : t->button.fill);
    gfx_color_t border = pressed ? t->button.border_pressed
                       : def ? t->button.border_active : t->button.border;
    gfx_draw_rect(s, r->x, r->y, r->w, r->h, border);
    if (def)
        gfx_draw_rect(s, r->x + 1, r->y + 1, r->w - 2, r->h - 2, border);
    int32_t tw = gfx_text_width(s, label);
    gfx_draw_text(s, r->x + (r->w - tw) / 2, gfx_text_y(s, r->y, r->h), label, t->button.text);
}

static void draw_glyph(gfx_surface_t* s, int32_t x, int32_t cy, bool dir, const wm_theme_t* t) {
    if (dir) {
        gfx_fill_rect(s, x, cy - 4, 5, 2, FOLDER_COLOR);
        gfx_fill_rect(s, x, cy - 3, ICON_W, 8, FOLDER_COLOR);
        gfx_draw_hline(s, x, cy + 4, ICON_W, t->client.text_soft);
    } else {
        gfx_fill_rect(s, x + 2, cy - 5, ICON_W - 4, 11, GFX_RGB(255, 255, 255));
        gfx_draw_rect(s, x + 2, cy - 5, ICON_W - 4, 11, t->client.text_soft);
        gfx_draw_hline(s, x + 4, cy - 2, ICON_W - 8, t->client.text_soft);
        gfx_draw_hline(s, x + 4, cy + 1, ICON_W - 8, t->client.text_soft);
    }
}

// The end of `text` that fits in `fit` cells, ".." in front when cut
static void fit_tail(const char* text, size_t fit, char* buf, size_t size) {
    size_t len = strlen(text);
    if (len <= fit || fit < 3) {
        ui_fit_text(text, fit, buf, size);
        return;
    }
    snprintf(buf, size, "..%s", text + len - (fit - 2));
}

static void paint(filedlg_t* d) {
    gfx_surface_t* s = wm_window_surface(d->win);
    if (s == NULL)
        return;
    layout(d, s);
    const layout_t* L = &d->lay;
    const wm_theme_t* t = &d->theme;
    int32_t cw = gfx_char_width(s);
    if (cw < 1) cw = 1;
    char buf[PATH_MAX];

    gfx_clear(s, t->client.background);

    // The folder
    fit_tail(d->dir, (size_t)(L->path.w / cw), buf, sizeof(buf));
    gfx_draw_text(s, L->path.x, gfx_text_y(s, L->path.y, L->path.h), buf, t->client.text);
    draw_button(d, s, BTN_UP, "Up", false);

    // The list
    gfx_fill_rect(s, L->list.x, L->list.y, L->list.w, L->list.h, t->edit.background);
    gfx_draw_rect(s, L->list.x, L->list.y, L->list.w, L->list.h,
                  d->list_active ? t->button.border_active : t->edit.border);
    gfx_save(s);
    gfx_set_clip(s, &L->rows);
    int32_t text_x = L->rows.x + 4 + ICON_W + 6;
    size_t fit = (size_t)((L->rows.x + L->rows.w - 4 - text_x) / cw);
    for (int32_t i = 0; i <= L->visible && d->top + i < (int32_t)d->count; i++) {
        int32_t index = d->top + i;
        const entry_t* e = &d->items[index];
        int32_t y = L->rows.y + i * L->row_h;
        if (index == d->sel)
            gfx_fill_rect(s, L->rows.x, y, L->rows.w, L->row_h, t->edit.selection);
        draw_glyph(s, L->rows.x + 4, y + L->row_h / 2, e->dir, t);
        ui_fit_text(e->name, fit, buf, sizeof(buf));
        gfx_draw_text(s, text_x, gfx_text_y(s, y, L->row_h), buf, t->edit.text);
    }
    if (d->count == 0)
        gfx_draw_text(s, L->rows.x + 6, gfx_text_y(s, L->rows.y, L->row_h), "(empty)", t->client.text_soft);
    gfx_restore(s);

    if (L->bar.w > 0) {
        gfx_fill_rect(s, L->bar.x, L->bar.y, L->bar.w, L->bar.h, t->client.background);
        gfx_fill_rect(s, L->thumb.x, L->thumb.y, L->thumb.w, L->thumb.h,
                      d->thumb_held ? t->button.fill_pressed : t->button.fill);
        gfx_draw_rect(s, L->thumb.x, L->thumb.y, L->thumb.w, L->thumb.h, t->button.border);
    }

    // The name
    gfx_draw_text(s, L->name_label_x, gfx_text_y(s, d->name.rect.y, d->name.rect.h), "Name:", t->client.text);
    ui_edit_style_t style = {
        .background = t->edit.background,
        .border     = d->list_active ? t->edit.border : t->button.border_active,
        .text       = t->edit.text,
        .cursor     = t->edit.cursor,
        .selection  = t->edit.selection,
    };
    ui_edit_paint(&d->name, s, &style);

    // The status and the buttons
    if (d->status[0] != '\0') {
        ui_fit_text(d->status, (size_t)(L->status.w / cw), buf, sizeof(buf));
        gfx_draw_text(s, L->status.x, gfx_text_y(s, L->status.y, L->status.h), buf,
                      d->status_error ? t->client.text_danger : t->client.text_soft);
    }
    draw_button(d, s, BTN_OK, d->mode == FILEDLG_SAVE ? "Save" : "Open", true);
    draw_button(d, s, BTN_CANCEL, "Cancel", false);

    wm_window_commit(d->win, NULL);
}

//
// Actions
//

static void select_row(filedlg_t* d, int32_t index) {
    if (d->count == 0)
        return;
    if (index < 0) index = 0;
    if (index >= (int32_t)d->count) index = (int32_t)d->count - 1;
    d->sel = index;
    d->list_active = true;
    d->confirm[0] = '\0';
    if (!d->items[index].dir)
        set_name(d, d->items[index].name);
    scroll_to_sel(d);
    d->dirty = true;
}

static void enter_dir(filedlg_t* d, const char* name) {
    if (strcmp(name, "..") == 0) {
        go_up(d);
        return;
    }
    char path[PATH_MAX];
    if (join_path(d->dir, name, path, sizeof(path)) == NULL) {
        set_status(d, true, strerror(errno));
        return;
    }
    load_dir(d, path, NULL);
}

// Enter, double click or the OK button
static filedlg_result_t accept(filedlg_t* d) {
    if (d->list_active && d->sel >= 0 && d->items[d->sel].dir) {
        enter_dir(d, d->items[d->sel].name);
        return FILEDLG_HANDLED;
    }
    if (d->name.len == 0) {
        set_status(d, true, d->mode == FILEDLG_SAVE ? "Type a name to save under" : "Pick a file");
        return FILEDLG_HANDLED;
    }

    char path[PATH_MAX];
    if (join_path(d->dir, d->name.text, path, sizeof(path)) == NULL) {
        set_status(d, true, strerror(errno));
        return FILEDLG_HANDLED;
    }

    struct stat st;
    bool exists = (stat(path, &st) == 0);
    if (exists && S_ISDIR(st.st_mode)) {
        // A folder typed in: go there
        if (load_dir(d, path, NULL)) {
            set_name(d, d->mode == FILEDLG_SAVE ? d->suggest : "");
            d->list_active = false;
        }
        return FILEDLG_HANDLED;
    }

    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char msg[sizeof(d->status)];

    if (d->mode == FILEDLG_OPEN) {
        if (!exists) {
            snprintf(msg, sizeof(msg), "Can't find %s", base);
            set_status(d, true, msg);
            return FILEDLG_HANDLED;
        }
    } else if (exists && strcmp(d->confirm, path) != 0) {
        strlcpy(d->confirm, path, sizeof(d->confirm));
        snprintf(msg, sizeof(msg), "%s is already there. Save again to replace it.", base);
        set_status(d, true, msg);
        return FILEDLG_HANDLED;
    }

    strlcpy(d->result, path, sizeof(d->result));
    set_status(d, false, NULL);
    return FILEDLG_ACCEPT;
}

static filedlg_result_t press_button(filedlg_t* d, int button) {
    switch (button) {
        case BTN_OK:     return accept(d);
        case BTN_CANCEL: return FILEDLG_CANCEL;
        case BTN_UP:     go_up(d); return FILEDLG_HANDLED;
    }
    return FILEDLG_HANDLED;
}

// Text or a copied file: its first line, typed into the name field
static void paste(filedlg_t* d) {
    wm_clip_info_t info;
    char* data = (char*)wm_clip_get(&info);
    if (data == NULL)
        return;
    if (info.type == WM_CLIP_TEXT || info.type == WM_CLIP_FILES)
        for (uint32_t i = 0; i < info.size && data[i] != '\n' && data[i] != '\r' && data[i] != '\0'; i++)
            ui_edit_key(&d->name, (uint8_t)data[i], 0);
    free(data);
    d->list_active = false;
    d->dirty = true;
}

//
// Events
//

static filedlg_result_t on_key(filedlg_t* d, const wm_event_t* ev) {
    uint16_t key = ev->key;
    bool ctrl = (ev->mods & INPUT_MOD_CTRL) != 0;
    int32_t page = d->lay.visible > 1 ? d->lay.visible - 1 : 1;

    switch (key) {
        case KEY_ESCAPE:    return FILEDLG_CANCEL;
        case KEY_ENTER:     return accept(d);
        case KEY_TAB:       d->list_active = !d->list_active; d->dirty = true; return FILEDLG_HANDLED;
        case KEY_UP:        select_row(d, d->sel < 0 ? 0 : d->sel - 1);        return FILEDLG_HANDLED;
        case KEY_DOWN:      select_row(d, d->sel + 1);                         return FILEDLG_HANDLED;
        case KEY_PAGE_UP:   select_row(d, d->sel < 0 ? 0 : d->sel - page);     return FILEDLG_HANDLED;
        case KEY_PAGE_DOWN: select_row(d, d->sel + page);                      return FILEDLG_HANDLED;
        case KEY_HOME:
        case KEY_END:
            if (d->list_active) {
                select_row(d, key == KEY_HOME ? 0 : (int32_t)d->count - 1);
                return FILEDLG_HANDLED;
            }
            break;
        case KEY_BACKSPACE:
            if (d->list_active || d->name.len == 0) {
                go_up(d);
                return FILEDLG_HANDLED;
            }
            break;
    }

    if (ctrl && (key == 'v' || key == 'V' || key == 22)) {
        paste(d);
        return FILEDLG_HANDLED;
    }
    if (ui_edit_key(&d->name, key, ev->mods) != UI_EDIT_IGNORED) {
        d->list_active = false;
        d->confirm[0] = '\0';
        d->dirty = true;
    }
    return FILEDLG_HANDLED;
}

static filedlg_result_t on_mouse_down(filedlg_t* d, const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return FILEDLG_HANDLED;
    gfx_surface_t* s = wm_window_surface(d->win);
    if (s == NULL)
        return FILEDLG_HANDLED;
    layout(d, s);
    const layout_t* L = &d->lay;

    for (int i = 0; i < BTN_COUNT; i++) {
        if (in_rect(&L->button[i], ev->x, ev->y)) {
            d->pressed = i;
            d->dirty = true;
            return FILEDLG_HANDLED;
        }
    }

    if (ui_edit_click(&d->name, ev->x, ev->y, ev->mods, ev->clicks)) {
        d->name_drag = true;
        d->list_active = false;
        d->dirty = true;
        return FILEDLG_HANDLED;
    }

    if (L->bar.w > 0 && in_rect(&L->bar, ev->x, ev->y)) {
        if (in_rect(&L->thumb, ev->x, ev->y)) {
            d->thumb_held = true;
            d->thumb_grab = ev->y - L->thumb.y;
        } else {
            scroll_by(d, (ev->y < L->thumb.y ? -1 : 1) * (L->visible > 1 ? L->visible - 1 : 1));
        }
        d->dirty = true;
        return FILEDLG_HANDLED;
    }

    if (in_rect(&L->rows, ev->x, ev->y)) {
        int32_t index = d->top + (ev->y - L->rows.y) / L->row_h;
        if (index >= (int32_t)d->count) {
            d->sel = -1;            // Empty space: nothing selected
            d->list_active = true;
            d->dirty = true;
            return FILEDLG_HANDLED;
        }
        select_row(d, index);
        if (ev->clicks >= 2) {
            if (d->items[index].dir) {
                char name[NAME_MAX + 1];
                strlcpy(name, d->items[index].name, sizeof(name));
                enter_dir(d, name);
                return FILEDLG_HANDLED;
            }
            return accept(d);
        }
    }
    return FILEDLG_HANDLED;
}

static void on_mouse_move(filedlg_t* d, const wm_event_t* ev) {
    bool held = (ev->buttons & INPUT_BTN_LEFT) != 0;
    if (!held) {
        if (d->name_drag || d->thumb_held || d->pressed != BTN_NONE)
            d->dirty = true;
        d->name_drag = d->thumb_held = false;
        d->pressed = BTN_NONE;
        return;
    }
    if (d->name_drag) {
        ui_edit_drag(&d->name, ev->x);
        d->dirty = true;
    } else if (d->thumb_held) {
        const layout_t* L = &d->lay;
        int32_t range = (int32_t)d->count - L->visible;
        int32_t travel = L->bar.h - L->thumb.h;
        if (range > 0 && travel > 0) {
            int32_t pos = ev->y - d->thumb_grab - L->bar.y;
            d->top = (int32_t)(((int64_t)pos * range + travel / 2) / travel);
            scroll_by(d, 0);        // Clamps
        }
    }
}

// Wheel anywhere in the dialog scrolls the list (nothing else in it scrolls)
static void on_wheel(filedlg_t* d, const wm_event_t* ev) {
    if (ev->wheel == 0 || d->thumb_held || d->lay.bar.w == 0)
        return;
    int32_t step = WHEEL_ROWS;
    if (ev->mods & INPUT_MOD_CTRL)
        step = d->lay.visible > 1 ? d->lay.visible - 1 : 1;
    scroll_by(d, (int32_t)ev->wheel * step);
}

static filedlg_result_t on_mouse_up(filedlg_t* d, const wm_event_t* ev) {
    d->name_drag = false;
    if (d->thumb_held) {
        d->thumb_held = false;
        d->dirty = true;
    }
    if (d->pressed == BTN_NONE)
        return FILEDLG_HANDLED;
    int button = d->pressed;
    d->pressed = BTN_NONE;
    d->dirty = true;
    if (in_rect(&d->lay.button[button], ev->x, ev->y))     // Released over the same button
        return press_button(d, button);
    return FILEDLG_HANDLED;
}

//
// Public
//

filedlg_t* filedlg_open(const filedlg_desc_t* desc) {
    filedlg_t* d = (filedlg_t*)calloc(1, sizeof(filedlg_t));
    if (d == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    d->mode = desc->mode;
    d->owner = desc->owner;
    d->sel = -1;
    d->pressed = BTN_NONE;
    load_theme(d);

    // Where to start: the folder given, a file's folder, else the cwd
    char start[PATH_MAX], name[NAME_MAX + 1] = "";
    struct stat st;
    bool ok = false;
    if (desc->path != NULL && desc->path[0] != '\0' && os_abspath(desc->path, start, sizeof(start)) != NULL) {
        if (stat(start, &st) == 0 && S_ISDIR(st.st_mode)) {
            ok = true;
        } else {
            char* slash = strrchr(start, '/');
            if (slash != NULL) {
                strlcpy(name, slash + 1, sizeof(name));
                if (slash == start) slash[1] = '\0'; else slash[0] = '\0';
                ok = (stat(start, &st) == 0 && S_ISDIR(st.st_mode));
            }
        }
    }
    if (!ok && getcwd(start, sizeof(start)) == NULL)
        strlcpy(start, "/", sizeof(start));
    if (name[0] == '\0' && desc->name != NULL)
        strlcpy(name, desc->name, sizeof(name));

    strlcpy(d->suggest, (d->mode == FILEDLG_SAVE) ? name : "", sizeof(d->suggest));
    if (!load_dir(d, start, name[0] ? name : NULL) && strcmp(start, "/") != 0)
        load_dir(d, "/", NULL);
    set_name(d, (d->mode == FILEDLG_SAVE || d->sel >= 0) ? name : "");
    d->list_active = (d->mode == FILEDLG_OPEN);

    int32_t sw = 0, sh = 0;
    wm_screen_size(&sw, &sh);
    wm_window_desc_t wd;
    memset(&wd, 0, sizeof(wd));
    wd.title      = desc->title ? desc->title : (d->mode == FILEDLG_SAVE ? "Save as" : "Open");
    wd.x          = (sw > DLG_W) ? (sw - DLG_W) / 2 : WM_POS_DEFAULT;
    wd.y          = (sh > DLG_H) ? (sh - DLG_H) / 2 - 30 : WM_POS_DEFAULT;
    wd.width      = DLG_W;
    wd.height     = DLG_H;
    wd.min_width  = DLG_MIN_W;
    wd.min_height = DLG_MIN_H;
    wd.style      = WM_STYLE_RESIZABLE | WM_STYLE_NOMINIMIZE;

    d->win = wm_window_create(&wd);
    if (d->win == 0) {
        int err = errno;
        free_items(d);
        free(d->items);
        free(d);
        errno = err;
        return NULL;
    }

    // Lay out once so the selection can be scrolled into view
    gfx_surface_t* s = wm_window_surface(d->win);
    if (s != NULL) {
        layout(d, s);
        scroll_to_sel(d);
    }
    d->dirty = true;
    paint(d);
    d->dirty = false;
    wm_window_focus(d->win);
    return d;
}

void filedlg_close(filedlg_t* d) {
    if (d == NULL)
        return;
    if (d->win != 0)
        wm_window_destroy(d->win);
    if (d->owner != 0)
        wm_window_focus(d->owner);
    free_items(d);
    free(d->items);
    free(d);
}

filedlg_result_t filedlg_event(filedlg_t* d, const wm_event_t* ev) {
    if (d == NULL)
        return FILEDLG_IGNORED;

    if (ev->window == 0) {
        if (ev->type == WM_EVENT_THEME_CHANGED) {
            load_theme(d);
            char text[UI_EDIT_MAX];
            strlcpy(text, d->name.text, sizeof(text));
            set_name(d, text);      // The font's cell may have changed
        } else if (ev->type == WM_EVENT_FS_CHANGED) {
            char keep[NAME_MAX + 1] = "";
            if (d->sel >= 0)
                strlcpy(keep, d->items[d->sel].name, sizeof(keep));
            char dir[PATH_MAX];
            strlcpy(dir, d->dir, sizeof(dir));
            if (!load_dir(d, dir, keep[0] ? keep : NULL))
                go_up(d);
            scroll_to_sel(d);
        }
        return FILEDLG_IGNORED;     // The program may want them too
    }
    if (ev->window != d->win || d->win == 0)
        return FILEDLG_IGNORED;

    switch (ev->type) {
        case WM_EVENT_KEY:          return on_key(d, ev);
        case WM_EVENT_MOUSE_DOWN:   return on_mouse_down(d, ev);
        case WM_EVENT_MOUSE_UP:     return on_mouse_up(d, ev);
        case WM_EVENT_MOUSE_MOVE:   on_mouse_move(d, ev); break;
        case WM_EVENT_MOUSE_WHEEL:  on_wheel(d, ev); break;
        case WM_EVENT_CLOSE:        return FILEDLG_CANCEL;

        case WM_EVENT_DESTROYED:
            d->win = 0;
            return FILEDLG_CANCEL;

        case WM_EVENT_RESIZED:
        case WM_EVENT_FOCUS_GAINED:
        case WM_EVENT_FOCUS_LOST:
            d->dirty = true;
            break;
    }
    return FILEDLG_HANDLED;
}

bool filedlg_dirty(const filedlg_t* d) {
    return d != NULL && d->win != 0 && d->dirty;
}

void filedlg_paint(filedlg_t* d) {
    if (!filedlg_dirty(d))
        return;
    d->dirty = false;
    paint(d);
}

const char* filedlg_path(const filedlg_t* d) {
    return d ? d->result : "";
}

filedlg_mode_t filedlg_mode(const filedlg_t* d) {
    return d ? d->mode : FILEDLG_OPEN;
}

wm_window_t filedlg_window(const filedlg_t* d) {
    return d ? d->win : 0;
}

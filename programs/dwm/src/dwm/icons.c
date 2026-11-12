/*
 * Desktop icons (icons.h): the icon list, selection, dragging and the
 * selection rectangle ("band"). input.c decides what a click or a drag on
 * the desktop means and calls in here; the compositor calls icons_paint()
 * right after the desktop background.
 *
 * Icons are kept in drawing order (the last on top). Dragging moves the
 * selected icons to the end, so they are drawn over the others.
 *
 * Labels: a name too long for the cell ends in ".."; a selected icon shows
 * all of it (up to LABEL_LINES_MAX lines) on a plate over whatever is below.
 * Renaming (dwm_icon_edit) draws an edit box (os/ui.h, shared with the
 * programs) in place of the label: arrows, Home/End and clicks move its
 * cursor, Shift or a drag selects.
 */

#include <string.h>
#include <os/input.h>
#include <os/ui.h>

#include "background.h"
#include "compositor.h"
#include "dirty.h"
#include "icons.h"
#include "internal.h"
#include "theme.h"
#include "../resources/imagelib.h"

#define FIRST_X             12
#define FIRST_Y             12
#define IMAGE_SIZE_DEFAULT  32      // Plain square when the image is missing
#define IMAGE_TOP           3       // Image's offset in the cell
#define LABEL_GAP           4       // Between image and label
#define LABEL_BOTTOM        4
#define LABEL_LINES_MAX     4       // A selected icon's full name, wrapped
#define EDIT_MIN_W          30      // Rename box width limits
#define EDIT_MAX_W          160

typedef struct {
    dwm_icon_t handle;
    char       label[DWM_ICON_LABEL_MAX];
    char       image[IMAGE_NAME_MAX];
    int32_t    x, y;
    bool       selected;
    bool       band_base;           // Selected before the band started (Ctrl)
    bool       drop_target;         // A folder or drive: drops on it go into it
    int32_t    drag_x, drag_y;      // Position when the drag started
    void*      user;
    char       path[DWM_ICON_PATH_MAX];     // dwm_icon_set_path, "": none
} icon_t;

static icon_t     icons[DWM_MAX_ICONS];
static size_t     count = 0;
static dwm_icon_t next_handle = 1;

static bool       selection_changed = false;

_Static_assert(DWM_ICON_LABEL_MAX <= UI_EDIT_MAX, "a label must fit the rename box");

// The icon being renamed (dwm_icon_edit)
static struct {
    dwm_icon_t icon;                // 0: none
    ui_edit_t  box;
} edit;
static char       edit_result[DWM_ICON_LABEL_MAX];

static dwm_icon_t drop_hover = 0;   // Drop target under a drag: drawn highlighted

static bool       band_active = false;
static int32_t    band_x0, band_y0;
static gfx_rect_t band = { 0, 0, 0, 0 };

//
// Lookup and geometry
//

static icon_t* find(dwm_icon_t handle) {
    if (handle == 0)
        return NULL;
    for (size_t i = 0; i < count; i++) {
        if (icons[i].handle == handle)
            return &icons[i];
    }
    return NULL;
}

static const image_t* icon_image(const icon_t* icon) {
    return imagelib_get(icon->image);
}

static int32_t screen_w(void) { int32_t w = compositor_width();  return w > 0 ? w : 1024; }
// Down to the taskbar: icons live in the work area
static int32_t screen_h(void) { int32_t h = dwm_work_area().h; return h > 0 ? h : 768; }

static int32_t line_h(void) {
    return dwm_text_height();
}

static int32_t image_h(const icon_t* icon) {
    const image_t* image = icon_image(icon);
    return image ? image->height : IMAGE_SIZE_DEFAULT;
}

static gfx_rect_t cell(const icon_t* icon) {
    return gfx_rect(icon->x, icon->y, DWM_ICON_WIDTH, IMAGE_TOP + image_h(icon) + LABEL_GAP + line_h() + LABEL_BOTTOM);
}

// Top of the label's first line
static int32_t label_top(const icon_t* icon) {
    return icon->y + IMAGE_TOP + image_h(icon) + LABEL_GAP;
}

// Width of a label line
static int32_t label_width(void) {
    return DWM_ICON_WIDTH - 4;
}


// A selected icon whose name is longer than one line shows all of it
static bool expanded(const icon_t* icon) {
    return icon->selected && icon->handle != edit.icon && dwm_text_width(icon->label, strlen(icon->label)) > label_width();
}

// Lines the expanded label takes
static size_t expanded_lines(const icon_t* icon) {
    dwm_text_line_t lines[LABEL_LINES_MAX];
    bool more;
    return dwm_text_wrap(icon->label, strlen(icon->label), label_width(), lines, LABEL_LINES_MAX, &more);
}

// The plate under the cell that an expanded label needs (empty if none)
static gfx_rect_t plate(const icon_t* icon) {
    if (!expanded(icon))
        return gfx_rect(0, 0, 0, 0);
    gfx_rect_t c = cell(icon);
    int32_t bottom = label_top(icon) + (int32_t)expanded_lines(icon) * line_h() + LABEL_BOTTOM;
    int32_t top = c.y + c.h - 1;
    return bottom > top ? gfx_rect(c.x, top, c.w, bottom - top) : gfx_rect(0, 0, 0, 0);
}

// The rename box, placed over the label and kept on the screen. Placed
// again on every call: the icon may have moved or changed image since.
static gfx_rect_t edit_box(const icon_t* icon) {
    gfx_rect_t c = cell(icon);
    ui_edit_place(&edit.box, c.x + c.w / 2, label_top(icon), EDIT_MIN_W, EDIT_MAX_W,
                  gfx_rect(0, 0, screen_w(), screen_h()));
    return edit.box.rect;
}

// Everything the icon draws on: its cell, plate and rename box
static gfx_rect_t extent(const icon_t* icon) {
    gfx_rect_t r = rect_union(cell(icon), plate(icon));
    if (icon->handle == edit.icon)
        r = rect_union(r, edit_box(icon));
    return r;
}

static void invalidate_icon(const icon_t* icon) {
    compositor_invalidate(extent(icon));
}

static void set_selected(icon_t* icon, bool selected) {
    if (icon->selected == selected)
        return;
    invalidate_icon(icon);          // The plate may come or go
    icon->selected = selected;
    selection_changed = true;
    invalidate_icon(icon);
}

//
// Public API
//

// First grid spot no icon overlaps, filling columns top to bottom
static void free_spot(int32_t* x, int32_t* y) {
    int32_t rows = (screen_h() - FIRST_Y) / DWM_ICON_GRID_Y;
    int32_t cols = (screen_w() - FIRST_X) / DWM_ICON_GRID_X;
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;

    icon_t probe;
    memset(&probe, 0, sizeof(probe));
    for (int32_t c = 0; c < cols; c++) {
        for (int32_t r = 0; r < rows; r++) {
            probe.x = FIRST_X + c * DWM_ICON_GRID_X;
            probe.y = FIRST_Y + r * DWM_ICON_GRID_Y;
            gfx_rect_t spot = cell(&probe);
            bool taken = false;
            for (size_t i = 0; i < count && !taken; i++)
                taken = rect_overlaps(spot, cell(&icons[i]));
            if (!taken) {
                *x = probe.x;
                *y = probe.y;
                return;
            }
        }
    }
    *x = FIRST_X;       // Full: stack on the first spot
    *y = FIRST_Y;
}

dwm_icon_t dwm_icon_add(const char* label, const char* image, int32_t x, int32_t y) {
    if (count == DWM_MAX_ICONS)
        return 0;

    icon_t* icon = &icons[count];
    memset(icon, 0, sizeof(*icon));
    if (label != NULL) strncpy(icon->label, label, sizeof(icon->label) - 1);
    if (image != NULL) strncpy(icon->image, image, sizeof(icon->image) - 1);

    if (x == DWM_POS_DEFAULT || y == DWM_POS_DEFAULT)
        free_spot(&icon->x, &icon->y);
    else {
        icon->x = x;
        icon->y = y;
    }

    icon->handle = next_handle++;
    if (next_handle == 0)
        next_handle = 1;
    count++;

    invalidate_icon(icon);
    return icon->handle;
}

void dwm_icon_remove(dwm_icon_t handle) {
    icon_t* icon = find(handle);
    if (icon == NULL)
        return;
    if (icon->handle == edit.icon)
        edit.icon = 0;              // Renaming it: dropped, nothing posted
    if (icon->handle == drop_hover)
        drop_hover = 0;
    invalidate_icon(icon);
    if (icon->selected)
        selection_changed = true;
    size_t at = (size_t)(icon - icons);
    memmove(&icons[at], &icons[at + 1], (count - at - 1) * sizeof(icon_t));
    count--;
}

bool dwm_icon_exists(dwm_icon_t handle) {
    return find(handle) != NULL;
}

void dwm_icon_set_label(dwm_icon_t handle, const char* label) {
    icon_t* icon = find(handle);
    if (icon == NULL)
        return;
    invalidate_icon(icon);          // The old plate
    memset(icon->label, 0, sizeof(icon->label));
    if (label != NULL)
        strncpy(icon->label, label, sizeof(icon->label) - 1);
    invalidate_icon(icon);
}

const char* dwm_icon_label(dwm_icon_t handle) {
    icon_t* icon = find(handle);
    return icon ? icon->label : "";
}

void dwm_icon_set_image(dwm_icon_t handle, const char* image) {
    icon_t* icon = find(handle);
    if (icon == NULL)
        return;
    invalidate_icon(icon);      // The cell's height may change
    memset(icon->image, 0, sizeof(icon->image));
    if (image != NULL)
        strncpy(icon->image, image, sizeof(icon->image) - 1);
    invalidate_icon(icon);
}

const char* dwm_icon_image(dwm_icon_t handle) {
    icon_t* icon = find(handle);
    return icon ? icon->image : "";
}

void dwm_icon_set_user(dwm_icon_t handle, void* user) {
    icon_t* icon = find(handle);
    if (icon != NULL)
        icon->user = user;
}

void* dwm_icon_user(dwm_icon_t handle) {
    icon_t* icon = find(handle);
    return icon ? icon->user : NULL;
}

void dwm_icon_set_path(dwm_icon_t handle, const char* path) {
    icon_t* icon = find(handle);
    if (icon == NULL)
        return;
    icon->path[0] = '\0';
    if (path != NULL && path[0] == '/' && strlen(path) < sizeof(icon->path))
        strlcpy(icon->path, path, sizeof(icon->path));
}

const char* dwm_icon_path(dwm_icon_t handle) {
    icon_t* icon = find(handle);
    return icon ? icon->path : "";
}

int icons_selection_other_drive(const char* folder) {
    if (folder == NULL || folder[0] != '/')
        return -1;
    bool any = false;
    for (size_t i = 0; i < count; i++) {
        if (!icons[i].selected)
            continue;
        if (icons[i].path[0] == '\0')
            return -1;
        if (!dwm_same_drive(icons[i].path, folder))
            return 1;
        any = true;
    }
    return any ? 0 : -1;
}

void dwm_icon_set_drop_target(dwm_icon_t handle, bool target) {
    icon_t* icon = find(handle);
    if (icon != NULL)
        icon->drop_target = target;
}

bool dwm_icon_drop_target(dwm_icon_t handle) {
    icon_t* icon = find(handle);
    return icon != NULL && icon->drop_target;
}

void dwm_icon_move(dwm_icon_t handle, int32_t x, int32_t y) {
    icon_t* icon = find(handle);
    if (icon == NULL || (icon->x == x && icon->y == y))
        return;
    invalidate_icon(icon);
    icon->x = x;
    icon->y = y;
    invalidate_icon(icon);
}

bool dwm_icon_get_rect(dwm_icon_t handle, gfx_rect_t* rect) {
    icon_t* icon = find(handle);
    if (icon == NULL)
        return false;
    if (rect != NULL)
        *rect = cell(icon);
    return true;
}

bool dwm_icon_selected(dwm_icon_t handle) {
    icon_t* icon = find(handle);
    return icon != NULL && icon->selected;
}

void dwm_icon_select(dwm_icon_t handle, bool selected) {
    icon_t* icon = find(handle);
    if (icon != NULL)
        set_selected(icon, selected);
}

void dwm_icon_select_all(bool selected) {
    for (size_t i = 0; i < count; i++)
        set_selected(&icons[i], selected);
}

size_t dwm_icon_selection(dwm_icon_t* out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if (!icons[i].selected)
            continue;
        if (out != NULL && n < max)
            out[n] = icons[i].handle;
        n++;
    }
    return n;
}

size_t dwm_icon_count(void) {
    return count;
}

dwm_icon_t dwm_icon_at_index(size_t index) {
    return (index < count) ? icons[index].handle : 0;
}

static bool cell_contains(const icon_t* icon, int32_t x, int32_t y) {
    gfx_rect_t c = cell(icon);
    return x >= c.x && y >= c.y && x < c.x + c.w && y < c.y + c.h;
}

dwm_icon_t dwm_icon_at(int32_t x, int32_t y) {
    for (size_t i = count; i-- > 0; ) {
        if (cell_contains(&icons[i], x, y))
            return icons[i].handle;
    }
    return 0;
}

dwm_icon_t icons_drop_target_at(int32_t x, int32_t y, bool skip_selected) {
    for (size_t i = count; i-- > 0; ) {
        if (skip_selected && icons[i].selected)
            continue;
        if (cell_contains(&icons[i], x, y))
            return icons[i].drop_target ? icons[i].handle : 0;
    }
    return 0;
}

void icons_drop_hover(dwm_icon_t handle) {
    if (handle == drop_hover)
        return;
    icon_t* old = find(drop_hover);
    if (old != NULL)
        invalidate_icon(old);
    drop_hover = handle;
    icon_t* now = find(handle);
    if (now != NULL)
        invalidate_icon(now);
}

//
// Selection, for input.c
//

void icons_select_only(dwm_icon_t handle) {
    for (size_t i = 0; i < count; i++)
        set_selected(&icons[i], icons[i].handle == handle);
}

void icons_toggle(dwm_icon_t handle) {
    icon_t* icon = find(handle);
    if (icon != NULL)
        set_selected(icon, !icon->selected);
}

void icons_open_selection(void) {
    for (size_t i = 0; i < count; i++) {
        if (icons[i].selected)
            dwm_post_simple(DWM_DESKTOP, DWM_EVENT_ICON_OPEN, icons[i].handle, 0);
    }
}

void icons_open(dwm_icon_t handle) {
    if (find(handle) != NULL)
        dwm_post_simple(DWM_DESKTOP, DWM_EVENT_ICON_OPEN, handle, 0);
}

void icons_notify(void) {
    if (!selection_changed)
        return;
    selection_changed = false;
    dwm_post_simple(DWM_DESKTOP, DWM_EVENT_SELECTION_CHANGED, (uint32_t)dwm_icon_selection(NULL, 0), 0);
}

bool icons_default_key(const dwm_event_t* ev) {
    if (ev->key == KEY_ENTER) {
        icons_open_selection();
        return true;
    }
    if ((ev->mods & INPUT_MOD_CTRL) && (ev->key == 'a' || ev->key == 'A' || ev->key == 1)) {
        dwm_icon_select_all(true);
        return true;
    }
    return false;
}

//
// Renaming
//

bool dwm_icon_edit(dwm_icon_t handle) {
    icon_t* icon = find(handle);
    if (icon == NULL)
        return false;
    dwm_icon_edit_end(true);        // One at a time
    icons_select_only(handle);
    dwm_window_focus(DWM_DESKTOP);  // The keys come here

    invalidate_icon(icon);          // Its plate goes
    edit.icon = handle;
    ui_edit_init(&edit.box, icon->label, DWM_ICON_LABEL_MAX - 1, UI_EDIT_NAME);  // All selected
    invalidate_icon(icon);
    return true;
}

dwm_icon_t dwm_icon_editing(void) {
    return find(edit.icon) != NULL ? edit.icon : 0;
}

void dwm_icon_edit_end(bool keep) {
    icon_t* icon = find(edit.icon);
    if (icon == NULL) {
        edit.icon = 0;
        return;
    }
    invalidate_icon(icon);
    edit.icon = 0;
    invalidate_icon(icon);          // The label (or plate) is back

    if (keep && edit.box.len > 0 && strcmp(edit.box.text, icon->label) != 0) {
        memcpy(edit_result, edit.box.text, edit.box.len + 1);
        dwm_post_simple(DWM_DESKTOP, DWM_EVENT_ICON_RENAMED, icon->handle, 0);
    }
}

const char* dwm_icon_edit_result(void) {
    return edit_result;
}

bool icons_edit_key(uint16_t key, uint8_t mods) {
    icon_t* icon = find(edit.icon);
    if (icon == NULL)
        return false;

    invalidate_icon(icon);          // The box may grow or shrink
    ui_edit_result_t r = ui_edit_key(&edit.box, key, mods);
    if (r == UI_EDIT_ACCEPT || r == UI_EDIT_CANCEL) {
        dwm_icon_edit_end(r == UI_EDIT_ACCEPT);
        return true;
    }
    invalidate_icon(icon);
    return true;                    // Every key is the box's while renaming
}

bool icons_edit_click(int32_t x, int32_t y, uint8_t mods, uint8_t clicks) {
    icon_t* icon = find(edit.icon);
    if (icon == NULL)
        return false;
    edit_box(icon);                 // Placed where it is drawn
    if (!ui_edit_contains(&edit.box, x, y))
        return false;
    invalidate_icon(icon);
    ui_edit_click(&edit.box, x, y, mods, clicks);
    invalidate_icon(icon);
    return true;
}

void icons_edit_drag(int32_t x) {
    icon_t* icon = find(edit.icon);
    if (icon == NULL)
        return;
    edit_box(icon);
    invalidate_icon(icon);
    ui_edit_drag(&edit.box, x);
    invalidate_icon(icon);
}

//
// Dragging the selection
//

void icons_drag_start(void) {
    // Selected icons to the end (on top), keeping the order of both groups.
    // A stable partition in place, one icon_t of scratch: each unselected
    // icon found after a selected run is lifted out and the run slid up
    // under it. O(n^2) moves at worst, nothing at DWM_MAX_ICONS.
    size_t kept = 0;                        // Unselected ones placed so far
    for (size_t i = 0; i < count; i++) {
        if (icons[i].selected)
            continue;
        if (i != kept) {
            icon_t lifted = icons[i];
            memmove(&icons[kept + 1], &icons[kept], (i - kept) * sizeof(icon_t));
            icons[kept] = lifted;
        }
        kept++;
    }

    for (size_t i = 0; i < count; i++) {
        icons[i].drag_x = icons[i].x;
        icons[i].drag_y = icons[i].y;
        if (icons[i].selected)
            invalidate_icon(&icons[i]);     // Now on top
    }
}

void icons_drag_to(int32_t dx, int32_t dy) {
    // Keep the whole group on the screen
    bool any = false;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (size_t i = 0; i < count; i++) {
        if (!icons[i].selected)
            continue;
        gfx_rect_t c = cell(&icons[i]);
        int32_t cx0 = icons[i].drag_x, cy0 = icons[i].drag_y;
        int32_t cx1 = cx0 + c.w, cy1 = cy0 + c.h;
        if (!any || cx0 < x0) x0 = cx0;
        if (!any || cy0 < y0) y0 = cy0;
        if (!any || cx1 > x1) x1 = cx1;
        if (!any || cy1 > y1) y1 = cy1;
        any = true;
    }
    if (!any)
        return;

    if (x1 - x0 <= screen_w()) {
        if (x0 + dx < 0)           dx = -x0;
        if (x1 + dx > screen_w())  dx = screen_w() - x1;
    }
    if (y1 - y0 <= screen_h()) {
        if (y0 + dy < 0)           dy = -y0;
        if (y1 + dy > screen_h())  dy = screen_h() - y1;
    }

    for (size_t i = 0; i < count; i++) {
        if (icons[i].selected)
            dwm_icon_move(icons[i].handle, icons[i].drag_x + dx, icons[i].drag_y + dy);
    }
}

void icons_drag_end(void) {
    dwm_post_simple(DWM_DESKTOP, DWM_EVENT_ICONS_MOVED, (uint32_t)dwm_icon_selection(NULL, 0), 0);
}

void icons_drag_cancel(void) {
    for (size_t i = 0; i < count; i++) {
        if (icons[i].selected)
            dwm_icon_move(icons[i].handle, icons[i].drag_x, icons[i].drag_y);
    }
}

//
// Selection rectangle
//

// The parts of a outside b, invalidated
static void invalidate_outside(gfx_rect_t a, gfx_rect_t b) {
    gfx_rect_t i = rect_intersect(a, b);
    if (rect_empty(i)) {
        compositor_invalidate(a);
        return;
    }
    if (i.y > a.y)               compositor_invalidate(gfx_rect(a.x, a.y, a.w, i.y - a.y));
    if (i.y + i.h < a.y + a.h)   compositor_invalidate(gfx_rect(a.x, i.y + i.h, a.w, a.y + a.h - (i.y + i.h)));
    if (i.x > a.x)               compositor_invalidate(gfx_rect(a.x, i.y, i.x - a.x, i.h));
    if (i.x + i.w < a.x + a.w)   compositor_invalidate(gfx_rect(i.x + i.w, i.y, a.x + a.w - (i.x + i.w), i.h));
}

static void invalidate_edges(gfx_rect_t r) {
    if (rect_empty(r))
        return;
    compositor_invalidate(gfx_rect(r.x, r.y, r.w, 1));
    compositor_invalidate(gfx_rect(r.x, r.y + r.h - 1, r.w, 1));
    compositor_invalidate(gfx_rect(r.x, r.y, 1, r.h));
    compositor_invalidate(gfx_rect(r.x + r.w - 1, r.y, 1, r.h));
}

static void band_apply(void) {
    for (size_t i = 0; i < count; i++) {
        bool inside = !rect_empty(band) && rect_overlaps(band, cell(&icons[i]));
        set_selected(&icons[i], icons[i].band_base || inside);
    }
}

void icons_band_start(int32_t x, int32_t y, bool additive) {
    band_active = true;
    band_x0 = x;
    band_y0 = y;
    band = gfx_rect(x, y, 0, 0);
    for (size_t i = 0; i < count; i++)
        icons[i].band_base = additive && icons[i].selected;
}

void icons_band_to(int32_t x, int32_t y) {
    if (!band_active)
        return;
    int32_t x0 = band_x0 < x ? band_x0 : x;
    int32_t y0 = band_y0 < y ? band_y0 : y;
    int32_t x1 = band_x0 < x ? x : band_x0;
    int32_t y1 = band_y0 < y ? y : band_y0;
    gfx_rect_t next = gfx_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);

    // Only what changes: both outlines, and the area gained or lost
    invalidate_edges(band);
    invalidate_edges(next);
    if (!rect_empty(band)) {
        invalidate_outside(band, next);
        invalidate_outside(next, band);
    } else {
        compositor_invalidate(next);
    }

    band = next;
    band_apply();
}

void icons_band_end(void) {
    if (!band_active)
        return;
    compositor_invalidate(band);
    band_active = false;
    band = gfx_rect(0, 0, 0, 0);
}

//
// Painting
//

static void paint_text(gfx_surface_t* s, int32_t x, int32_t y, const char* text, size_t len) {
    const dwm_theme_t* theme = dwm_theme();
    gfx_set_blend(s, GFX_BLEND_ALPHA);
    dwm_text_draw(s, x + 1, y + 1, text, len, theme->desktop.icon_shadow);
    gfx_set_blend(s, GFX_BLEND_COPY);
    dwm_text_draw(s, x, y, text, len, theme->desktop.icon_text);
}

// One line centered in the cell
static void paint_line(gfx_surface_t* s, const gfx_rect_t* c, int32_t y, const char* text, size_t len) {
    paint_text(s, c->x + (c->w - dwm_text_width(text, len)) / 2, y, text, len);
}

static void paint_icon(gfx_surface_t* s, const icon_t* icon) {
    const dwm_theme_t* theme = dwm_theme();
    gfx_rect_t c = cell(icon);

    if (icon->selected || icon->handle == drop_hover) {
        gfx_set_blend(s, GFX_BLEND_ALPHA);
        gfx_fill_rect(s, c.x, c.y, c.w, c.h, theme->desktop.icon_selected);
        gfx_set_blend(s, GFX_BLEND_COPY);
        gfx_draw_rect(s, c.x, c.y, c.w, c.h, theme->desktop.icon_selected_border);
    }

    const image_t* image = icon_image(icon);
    int32_t iw = image ? image->width  : IMAGE_SIZE_DEFAULT;
    int32_t ih = image ? image->height : IMAGE_SIZE_DEFAULT;
    int32_t ix = c.x + (c.w - iw) / 2, iy = c.y + IMAGE_TOP;
    if (image != NULL) {
        image_draw(s, image, ix, iy);
    } else {
        gfx_fill_rect(s, ix, iy, iw, ih, theme->frame.background);
        gfx_draw_rect(s, ix, iy, iw, ih, theme->frame.border);
    }

    // Label, centered on one line, ".." where it was cut. A selected long
    // one and the rename box are drawn over everything (paint_top).
    if (icon->handle == edit.icon || expanded(icon))
        return;
    char line[DWM_ICON_LABEL_MAX];
    size_t len = dwm_text_ellipsize(icon->label, strlen(icon->label), label_width(), "..", false,
                                    line, sizeof(line));
    paint_line(s, &c, label_top(icon), line, len);
}

// A selected icon's full name on a plate
static void paint_expanded(gfx_surface_t* s, const icon_t* icon) {
    const dwm_theme_t* theme = dwm_theme();
    gfx_rect_t c = cell(icon), p = plate(icon), all = rect_union(c, p);

    gfx_set_blend(s, GFX_BLEND_COPY);
    background_paint(s, p);         // Hides what is below (the color or the wallpaper)
    gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_fill_rect(s, p.x, p.y, p.w, p.h, theme->desktop.icon_selected);
    gfx_set_blend(s, GFX_BLEND_COPY);
    gfx_draw_rect(s, all.x, all.y, all.w, all.h, theme->desktop.icon_selected_border);

    dwm_text_line_t lines[LABEL_LINES_MAX];
    bool more;
    size_t n = dwm_text_wrap(icon->label, strlen(icon->label), label_width(), lines, LABEL_LINES_MAX, &more);
    int32_t y = label_top(icon);
    for (size_t i = 0; i < n; i++, y += line_h()) {
        char line[DWM_ICON_LABEL_MAX];
        size_t len = dwm_text_ellipsize(icon->label + lines[i].start, lines[i].len, label_width(), "..",
                                        more && i == n - 1, line, sizeof(line));
        paint_line(s, &c, y, line, len);
    }
}

static void paint_edit(gfx_surface_t* s, const icon_t* icon) {
    const dwm_theme_t* theme = dwm_theme();
    ui_edit_style_t style = {
        .background = theme->edit.background,
        .border     = theme->desktop.icon_selected_border,
        .text       = theme->edit.text,
        .cursor     = theme->edit.cursor,
        .selection  = theme->edit.selection,
    };
    edit_box(icon);
    ui_edit_paint(&edit.box, s, &style);
}

void icons_paint(gfx_surface_t* s, gfx_rect_t area) {
    for (size_t i = 0; i < count; i++) {
        if (rect_overlaps(area, cell(&icons[i])))
            paint_icon(s, &icons[i]);
    }
    for (size_t i = 0; i < count; i++) {
        if (expanded(&icons[i]) && rect_overlaps(area, extent(&icons[i])))
            paint_expanded(s, &icons[i]);
    }
    icon_t* editing = find(edit.icon);
    if (editing != NULL && rect_overlaps(area, edit_box(editing)))
        paint_edit(s, editing);

    if (band_active && rect_overlaps(area, band)) {
        const dwm_theme_t* theme = dwm_theme();
        gfx_set_blend(s, GFX_BLEND_ALPHA);
        gfx_fill_rect(s, band.x, band.y, band.w, band.h, theme->desktop.band_fill);
        gfx_draw_rect(s, band.x, band.y, band.w, band.h, theme->desktop.band_border);
        gfx_set_blend(s, GFX_BLEND_COPY);
    }
}

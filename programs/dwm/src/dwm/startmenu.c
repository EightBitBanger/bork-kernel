/*
 * Start menu (taskbar.h): the panel above the Start button, a list, and
 * the search box along its bottom, next to the Start button.
 *
 * The list shows one of two things:
 *
 *   items     the desktop's start items (dwm_start_set_items), while the
 *             search box is empty
 *   results   what search.c has found for the box's text so far, a line
 *             saying how the search is going on top
 *
 * The panel keeps one height for both, so it doesn't jump while typing.
 *
 * Between the list and the search box, at the right end, is the shut down
 * button (dwm_start_set_shutdown): "ui_shutdown" from <home>/sys/images,
 * a drawn power sign while that isn't installed. The restart button
 * (dwm_start_set_restart, "ui_restart" or a drawn arrow) sits to its left.
 * They sit apart from the list on purpose: a click must go down and come
 * up on one, and no key reaches them, so they aren't hit by accident.
 *
 * The same panel serves a tray icon too (dwm_start_open_tray): mirrored
 * against the right edge, a little smaller, a heading and the rows the
 * icon's program sent (tray.h) in place of the list and search box, with a
 * scroll bar. The program may ask for buttons too: a small refresh button
 * ("ui_refresh", a drawn arrow while that isn't installed) left of the
 * heading's title, a Connect button under its prompt's box and a
 * Disconnect button along the bottom. Like the shut down button, a click
 * must go down and come up on one; F5 is the refresh button too.
 * The mouse wheel over the panel scrolls the search results or the tray
 * rows WHEEL_ROWS a notch (Ctrl: a page); the start items don't scroll.
 * While open it has the keys (input.c: after a context menu, before
 * everything else) and the first look at every mouse event. It is painted
 * above the taskbar, below a context menu.
 */

#include <limits.h>
#include <stdio.h>          // snprintf
#include <string.h>
#include <os/input.h>
#include <os/ui.h>

#include "compositor.h"
#include "dirty.h"
#include "internal.h"
#include "search.h"
#include "taskbar.h"
#include "theme.h"

#define PANEL_W         340
#define BORDER          1
#define STRIP_W         6       // Accent strip down the left edge
#define PAD             8
#define ICON_SIZE       20      // Images in front of the lines
#define ITEM_PAD_Y      5
#define SEPARATOR_H     9
#define LIST_ROWS_MIN   7       // Results the list has room for, at least

#define PLACEHOLDER     "Search files and folders"

// The shut down button
#define POWER_IMAGE     "ui_shutdown"
#define POWER_LABEL     "Shut down"     // Shown left of the buttons while the pointer is on it
#define RESTART_IMAGE   "ui_restart"
#define RESTART_LABEL   "Restart"
#define POWER_SIZE      24              // The images, drawn at their size
#define POWER_PAD       3               // Around each: the highlight
#define POWER_GAP       4               // Between the two

// The power buttons, right to left
typedef enum { PWR_NONE = 0, PWR_OFF, PWR_RESTART } power_button_t;

// The tray panel
#define TRAY_W          300     // Start's is PANEL_W
#define TRAY_H_NUM      7       // Its height: 7 / 8 of Start's
#define TRAY_H_DEN      8
#define SCROLL_W        13
#define WHEEL_ROWS      3           // Rows a wheel notch scrolls (Ctrl: a page)
#define BARS_W          13
#define LOCK_W          7
#define GAP             6
#define REFRESH_IMAGE   "ui_refresh"
#define REFRESH_SIZE    14      // The image, drawn at this size ...
#define REFRESH_PAD     2       // ... in a highlight this much bigger all round
#define BUTTON_PAD_X    12      // Connect / Disconnect: room either side of the label
#define CONNECT_LABEL   "Connect"
#define DISCONNECT_LABEL "Disconnect"

// The tray panel's buttons
typedef enum { TBTN_NONE = 0, TBTN_REFRESH, TBTN_CONNECT, TBTN_DISCONNECT } tray_button_t;

typedef struct {
    char               label[DWM_START_LABEL_MAX];
    char               image[IMAGE_NAME_MAX];
    char               arg[DWM_START_ARG_MAX];
    uint32_t           flags;
    dwm_start_action_t action;
} item_t;

static void start_open_panel(void);

static item_t             items[DWM_START_MAX_ITEMS];
static size_t             item_count;
static dwm_start_action_t open_path;
static dwm_start_action_t shutdown_action;     // NULL: no button
static dwm_start_action_t restart_action;      // NULL: no button

static struct {
    bool      open;
    ui_edit_t box;
    bool      box_drag;         // Left button went down in the box, still held
    bool      pressed;          // A button went down inside the panel
    int       hot;              // Highlighted line (item or result), -1: none
    size_t    top;              // First result showing
    bool      tray;             // The tray panel (against the right edge)
    void*     tray_owner;       // Whose list it shows (tray.h)
    uint32_t  tray_id;
    bool      sb_drag;          // Its scroll bar's thumb is held
    int32_t   drag_y;
    size_t    drag_top;
    int       row_down;         // The clickable row a press went down on, -1: none
    uint32_t  prompt_seen;      // The prompt the box was made for (its serial)
    bool      prompt_on;        // The box is the prompt's
    power_button_t power_hot;   // The power button the pointer is on
    power_button_t power_down;  // The one the left button went down on, still held
    tray_button_t btn_hot;      // The tray panel's button under the pointer
    tray_button_t btn_down;     // The one the left button went down on, still held
} m = { .hot = -1, .row_down = -1 };

//
// Geometry
//

static int32_t line_h(void) {
    int32_t h = dwm_text_height() + 2 * ITEM_PAD_Y;
    return h > ICON_SIZE + 4 ? h : ICON_SIZE + 4;
}

// A result: its name, then its folder below in small print
static int32_t result_h(void) {
    return 2 * dwm_text_height() + 6;
}

static int32_t status_h(void) {
    return dwm_text_height() + 6;
}

static int32_t items_h(void) {
    int32_t h = 0;
    for (size_t i = 0; i < item_count; i++)
        h += (items[i].flags & DWM_START_SEPARATOR) ? SEPARATOR_H : line_h();
    return h;
}

// The box's text line: from the edit box (it sizes itself to its font)
static int32_t search_area_h(void) {
    return m.box.rect.h > 0 ? m.box.rect.h + 2 * PAD : dwm_text_height() + 8 + 2 * PAD;
}

// The row the power buttons sit in, between the list and the search box
static int32_t power_area_h(void) {
    return ((shutdown_action != NULL || restart_action != NULL) && !m.tray)
           ? POWER_SIZE + 2 * POWER_PAD + PAD : 0;
}

static int32_t list_h(void) {
    int32_t want = status_h() + LIST_ROWS_MIN * result_h();
    int32_t have = items_h();
    return have > want ? have : want;
}

static gfx_rect_t panel_rect(void) {
    gfx_rect_t work = dwm_work_area();
    int32_t w = m.tray ? TRAY_W : PANEL_W;
    if (w > work.w) w = work.w;
    int32_t h = 2 * BORDER + search_area_h() + list_h() + PAD;
    if (m.tray) h = h * TRAY_H_NUM / TRAY_H_DEN;
    else        h += power_area_h();
    if (h > work.h) h = work.h;
    int32_t x = m.tray ? work.x + work.w - w : work.x;      // The tray panel: mirrored
    return gfx_rect(x, work.y + work.h - h, w, h);
}

static int32_t content_x(void) {
    return panel_rect().x + BORDER + (m.tray ? 0 : STRIP_W);     // The tray panel's strip is on the right
}

static int32_t content_w(void) {
    return panel_rect().w - 2 * BORDER - STRIP_W;
}

static gfx_rect_t list_rect(void) {
    gfx_rect_t p = panel_rect();
    int32_t y = p.y + BORDER + PAD / 2;
    return gfx_rect(content_x(), y, content_w(), p.y + p.h - BORDER - search_area_h() - power_area_h() - y);
}

static dwm_start_action_t power_action(power_button_t which) {
    return which == PWR_OFF ? shutdown_action : which == PWR_RESTART ? restart_action : NULL;
}

// A power button, highlight included: shut down right-aligned with the
// search box in the row above it, restart to its left (or there itself
// without a shut down button). Empty when it isn't there.
static gfx_rect_t power_rect(power_button_t which) {
    if (power_area_h() == 0 || power_action(which) == NULL)
        return gfx_rect(0, 0, 0, 0);
    gfx_rect_t l = list_rect();
    int32_t size = POWER_SIZE + 2 * POWER_PAD;
    int32_t right = content_x() + content_w() - PAD;        // The box's right edge
    if (which == PWR_RESTART && shutdown_action != NULL)
        right -= size + POWER_GAP;
    return gfx_rect(right - size, l.y + l.h + PAD / 2 + 1, size, size);
}


// Place the box (it moves with the panel). Its rect stays valid until the
// next call.
static void place_box(void) {
    gfx_rect_t p = panel_rect();
    int32_t w = content_w() - 2 * PAD;
    int32_t top = p.y + p.h - BORDER - search_area_h();      // The search area, at the bottom
    ui_edit_place(&m.box, content_x() + PAD + w / 2, top + PAD + 4, w, w, p);
}

static bool searching(void) {
    return m.box.len > 0;
}

static size_t rows_showing(void) {
    gfx_rect_t l = list_rect();
    int32_t h = l.h - status_h();
    return h > 0 ? (size_t)(h / result_h()) : 0;
}

static gfx_rect_t item_rect(size_t i) {
    gfx_rect_t l = list_rect();
    int32_t y = l.y;
    for (size_t k = 0; k < i; k++)
        y += (items[k].flags & DWM_START_SEPARATOR) ? SEPARATOR_H : line_h();
    int32_t h = (items[i].flags & DWM_START_SEPARATOR) ? SEPARATOR_H : line_h();
    return gfx_rect(l.x, y, l.w, h);
}

// The line result `i` is on (empty when it isn't showing)
static gfx_rect_t result_rect(size_t i) {
    if (i < m.top || i - m.top >= rows_showing())
        return gfx_rect(0, 0, 0, 0);
    gfx_rect_t l = list_rect();
    return gfx_rect(l.x, l.y + status_h() + (int32_t)(i - m.top) * result_h(), l.w, result_h());
}

static bool contains(gfx_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static power_button_t power_at(int32_t x, int32_t y) {
    if (contains(power_rect(PWR_OFF), x, y))     return PWR_OFF;
    if (contains(power_rect(PWR_RESTART), x, y)) return PWR_RESTART;
    return PWR_NONE;
}

static size_t line_count(void) {
    return searching() ? search_count() : item_count;
}

static bool selectable(int i) {
    if (i < 0 || (size_t)i >= line_count())
        return false;
    return searching() || !(items[i].flags & DWM_START_SEPARATOR);
}

static int line_at(int32_t x, int32_t y) {
    if (!contains(list_rect(), x, y))
        return -1;
    if (searching()) {
        for (size_t i = m.top; i < search_count(); i++) {
            gfx_rect_t r = result_rect(i);
            if (r.h == 0)
                break;
            if (contains(r, x, y))
                return (int)i;
        }
        return -1;
    }
    for (size_t i = 0; i < item_count; i++) {
        if (contains(item_rect(i), x, y))
            return selectable((int)i) ? (int)i : -1;
    }
    return -1;
}

static void invalidate_panel(void) {
    if (m.open)
        compositor_invalidate(panel_rect());
}

static void invalidate_list(void) {
    if (m.open)
        compositor_invalidate(list_rect());
}

//
// The tray panel: a heading, the rows under it, a scroll bar on their right
//

static const dwm_tray_panel_t* tray_list(void) {
    return tray_panel_get(m.tray_owner, m.tray_id);
}

static size_t tray_rows_count(void) {
    const dwm_tray_panel_t* l = tray_list();
    return l != NULL ? l->count : 0;
}

static int32_t tray_head_h(void) {
    return dwm_text_height() + 10;
}

static int32_t tray_row_h(void) {
    int32_t h = dwm_text_height() + 6;
    return h < 16 ? 16 : h;
}

// A prompt from the program: its question and a box, at the bottom (where
// the start menu has its search box). The box (m.box) starts empty for
// each new prompt.
static bool tray_prompting(void) {
    const dwm_tray_panel_t* l = tray_list();
    bool on = l != NULL && l->prompt[0] != '\0';
    if (!on) {
        if (m.prompt_on)
            ui_edit_init(&m.box, "", 0, 0);         // Nothing typed stays behind
        m.prompt_on = false;
        return false;
    }
    if (!m.prompt_on || m.prompt_seen != l->prompt_serial) {
        ui_edit_init(&m.box, "", DWM_TRAY_TEXT_MAX - 1, l->secret ? UI_EDIT_SECRET : 0);
        m.prompt_seen = l->prompt_serial;
        m.prompt_on   = true;
        m.box_drag    = false;
    }
    return true;
}

static int32_t tray_prompt_h(void) {
    return tray_prompting() ? dwm_text_height() + 4 + search_area_h() : 0;
}

// Connect (under the prompt's box, when the program asked for it) and
// Disconnect share one row along the bottom
static bool tray_has_connect(void) {
    const dwm_tray_panel_t* l = tray_list();
    return l != NULL && l->connect && tray_prompting();
}

static bool tray_has_disconnect(void) {
    const dwm_tray_panel_t* l = tray_list();
    return l != NULL && l->disconnect;
}

static int32_t tray_button_h(void) {
    return dwm_text_height() + 8;
}

// The button row, padding included. Under the box the box's own padding
// is the gap above it; on its own it has a line and a gap above it.
static int32_t tray_buttons_h(void) {
    if (!tray_has_connect() && !tray_has_disconnect())
        return 0;
    return tray_button_h() + PAD + (tray_prompting() ? 0 : PAD);
}

static gfx_rect_t tray_button_rect(tray_button_t which) {
    gfx_rect_t p = panel_rect();
    if (which == TBTN_REFRESH) {
        const dwm_tray_panel_t* l = tray_list();
        if (l == NULL || !l->refresh)
            return gfx_rect(0, 0, 0, 0);
        int32_t size = REFRESH_SIZE + 2 * REFRESH_PAD;
        return gfx_rect(content_x() + PAD / 2, p.y + BORDER + (tray_head_h() - size) / 2, size, size);
    }
    bool connect = tray_has_connect();
    if ((which == TBTN_CONNECT && !connect) || (which == TBTN_DISCONNECT && !tray_has_disconnect()) ||
        which == TBTN_NONE)
        return gfx_rect(0, 0, 0, 0);
    const char* label = which == TBTN_CONNECT ? CONNECT_LABEL : DISCONNECT_LABEL;
    int32_t w = dwm_text_width(label, strlen(label)) + 2 * BUTTON_PAD_X;
    int32_t y = p.y + p.h - BORDER - tray_buttons_h() + (tray_prompting() ? 0 : PAD);
    // Connect on the right, where Enter's default sits; Disconnect on the
    // left beside it, or on the right on its own
    int32_t x = (which == TBTN_DISCONNECT && connect) ? content_x() + PAD : content_x() + content_w() - PAD - w;
    return gfx_rect(x, y, w, tray_button_h());
}

static tray_button_t tray_button_at(int32_t x, int32_t y) {
    static const tray_button_t all[] = { TBTN_REFRESH, TBTN_CONNECT, TBTN_DISCONNECT };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (contains(tray_button_rect(all[i]), x, y))
            return all[i];
    }
    return TBTN_NONE;
}

// The rows' area, scroll bar included
static gfx_rect_t tray_rows_rect(void) {
    gfx_rect_t p = panel_rect();
    int32_t y = p.y + BORDER + tray_head_h() + 1;
    return gfx_rect(content_x(), y, content_w(), p.y + p.h - BORDER - tray_prompt_h() - tray_buttons_h() - y);
}

// The box under the question (above the button row, if any)
static void tray_place_box(void) {
    gfx_rect_t p = panel_rect();
    int32_t w = content_w() - 2 * PAD;
    int32_t top = p.y + p.h - BORDER - tray_buttons_h() - search_area_h();
    ui_edit_place(&m.box, content_x() + PAD + w / 2, top + PAD + 4, w, w, p);
}

static size_t tray_rows_showing(void) {
    int32_t n = tray_rows_rect().h / tray_row_h();
    return n > 0 ? (size_t)n : 0;
}

static size_t tray_max_top(void) {
    size_t n = tray_rows_count(), rows = tray_rows_showing();
    return n > rows ? n - rows : 0;
}

typedef struct {
    gfx_rect_t up, down, track, thumb;
} scrollbar_t;

static scrollbar_t tray_scrollbar(void) {
    gfx_rect_t r = tray_rows_rect();
    scrollbar_t b;
    int32_t x = r.x + r.w - SCROLL_W;
    b.up    = gfx_rect(x, r.y, SCROLL_W, SCROLL_W);
    b.down  = gfx_rect(x, r.y + r.h - SCROLL_W, SCROLL_W, SCROLL_W);
    b.track = gfx_rect(x, r.y + SCROLL_W, SCROLL_W, r.h - 2 * SCROLL_W);
    int32_t count = (int32_t)tray_rows_count(), vis = (int32_t)tray_rows_showing();
    int32_t max = (int32_t)tray_max_top();
    int32_t th = count > 0 ? b.track.h * vis / count : b.track.h;
    if (th < 10) th = 10;
    if (th > b.track.h) th = b.track.h;
    int32_t ty = b.track.y + (max > 0 ? (b.track.h - th) * (int32_t)m.top / max : 0);
    b.thumb = gfx_rect(x + 2, ty, SCROLL_W - 4, th);
    return b;
}

static void tray_scroll_to(long top) {
    long max = (long)tray_max_top();
    if (top > max) top = max;
    if (top < 0) top = 0;
    if ((size_t)top == m.top)
        return;
    m.top = (size_t)top;
    invalidate_panel();
}

// The row under (x, y) that can be clicked, -1: none
static int tray_row_at(int32_t x, int32_t y) {
    gfx_rect_t r = tray_rows_rect();
    const dwm_tray_panel_t* l = tray_list();
    if (l == NULL || !contains(gfx_rect(r.x, r.y, r.w - SCROLL_W, r.h), x, y))
        return -1;
    size_t i = m.top + (size_t)((y - r.y) / tray_row_h());
    if ((size_t)((y - r.y) / tray_row_h()) >= tray_rows_showing() || i >= l->count ||
        !(l->rows[i].flags & DWM_PANEL_ROW_CLICK))
        return -1;
    return (int)i;
}

// Tell the program; the prompt is gone after ACCEPT / CANCEL
static void tray_tell(uint32_t param) {
    const char* text = NULL;
    if (param == DWM_PANEL_ACCEPT)
        text = m.box.text;
    void* owner = m.tray_owner;
    uint32_t id = m.tray_id;
    if (param == DWM_PANEL_ACCEPT || param == DWM_PANEL_CANCEL) {
        tray_panel_event(owner, id, param, text);
        ui_edit_init(&m.box, "", 0, 0);             // Wipes what was typed
        m.prompt_on = false;
    } else {
        tray_panel_event(owner, id, param, NULL);
    }
    invalidate_panel();
}

static void tray_key(uint16_t key, uint8_t mods) {
    long page = tray_rows_showing() > 1 ? (long)tray_rows_showing() - 1 : 1;
    if (tray_prompting()) {
        switch (ui_edit_key(&m.box, key, mods)) {
            case UI_EDIT_ACCEPT: tray_tell(DWM_PANEL_ACCEPT); return;
            case UI_EDIT_CANCEL: tray_tell(DWM_PANEL_CANCEL); return;
            case UI_EDIT_HANDLED:
                tray_place_box();
                compositor_invalidate(m.box.rect);
                return;
            default:
                break;              // Up, Down ... still scroll
        }
    }
    switch (key) {
        case KEY_ESCAPE:    dwm_start_close();                      break;
        case KEY_F5:
            if (tray_button_rect(TBTN_REFRESH).w > 0)
                tray_tell(DWM_PANEL_REFRESH);
            break;
        case KEY_UP:        tray_scroll_to((long)m.top - 1);        break;
        case KEY_DOWN:      tray_scroll_to((long)m.top + 1);        break;
        case KEY_PAGE_UP:   tray_scroll_to((long)m.top - page);     break;
        case KEY_PAGE_DOWN: tray_scroll_to((long)m.top + page);     break;
        case KEY_HOME:      tray_scroll_to(0);                      break;
        case KEY_END:       tray_scroll_to((long)tray_max_top());   break;
        default: break;
    }
}

static bool tray_mouse(int32_t x, int32_t y, bool moved, uint8_t pressed, uint8_t released, uint8_t held,
                       uint8_t mods, uint8_t clicks, bool in_panel) {
    if (moved && !m.sb_drag) {
        if (m.box_drag && held) {
            ui_edit_drag(&m.box, x);
            compositor_invalidate(m.box.rect);
        }
        int hot = tray_row_at(x, y);
        tray_button_t btn = m.box_drag ? TBTN_NONE : tray_button_at(x, y);
        if (hot != m.hot || btn != m.btn_hot) {
            m.hot     = hot;
            m.btn_hot = btn;
            invalidate_panel();
        }
    }
    if (released) {
        if (held == 0)
            m.box_drag = false;
        if ((released & INPUT_BTN_LEFT) && m.btn_down != TBTN_NONE) {
            // Down and up on the same button; let go anywhere else, nothing
            tray_button_t was = m.btn_down;
            m.btn_down = TBTN_NONE;
            invalidate_panel();
            if (tray_button_at(x, y) == was)
                tray_tell(was == TBTN_REFRESH ? DWM_PANEL_REFRESH :
                          was == TBTN_CONNECT ? DWM_PANEL_ACCEPT : DWM_PANEL_DISCONNECT);
            return true;
        }
        if ((released & INPUT_BTN_LEFT) && m.row_down >= 0) {
            int row = tray_row_at(x, y);
            bool same = row == m.row_down;
            m.row_down = -1;
            if (same)
                tray_tell((uint32_t)row);
        }
        return in_panel;
    }
    if (pressed && !in_panel) {
        // A press outside closes it. A left press on a tray icon goes no
        // further (it would open the panel again); anywhere else, and a
        // right press on an icon (its menu), it goes on to do what it does,
        // so one right click opens the menu even with the panel open.
        dwm_start_close();
        return pressed == INPUT_BTN_LEFT && tray_icon_at(x, y) >= 0;
    }
    if (m.sb_drag) {
        if (held == 0) {
            m.sb_drag = false;
            invalidate_panel();
        } else if (moved) {
            scrollbar_t b = tray_scrollbar();
            int32_t room = b.track.h - b.thumb.h;
            long max = (long)tray_max_top();
            if (room > 0)
                tray_scroll_to((long)m.drag_top + ((long)(y - m.drag_y) * max + room / 2) / room);
        }
        return true;
    }
    if (pressed & INPUT_BTN_LEFT) {
        scrollbar_t b = tray_scrollbar();
        long page = tray_rows_showing() > 1 ? (long)tray_rows_showing() - 1 : 1;
        tray_button_t btn = tray_button_at(x, y);
        if (btn != TBTN_NONE) {
            m.btn_down = btn;
            m.btn_hot  = btn;
            invalidate_panel();
            return true;
        }
        if (tray_prompting()) {
            tray_place_box();
            if (ui_edit_click(&m.box, x, y, mods, clicks)) {
                m.box_drag = true;
                compositor_invalidate(m.box.rect);
                return true;
            }
        }
        int row = tray_row_at(x, y);
        if (row >= 0) {
            m.row_down = row;
        } else if (contains(b.up, x, y)) {
            tray_scroll_to((long)m.top - 1);
        } else if (contains(b.down, x, y)) {
            tray_scroll_to((long)m.top + 1);
        } else if (contains(b.thumb, x, y)) {
            m.sb_drag  = true;
            m.drag_y   = y;
            m.drag_top = m.top;
            invalidate_panel();
        } else if (contains(b.track, x, y)) {
            tray_scroll_to((long)m.top + (y < b.thumb.y ? -page : page));
        }
    }
    return in_panel;
}

//
// Highlight and scrolling
//

static void set_hot(int i) {
    if (!selectable(i))
        i = -1;
    if (i == m.hot)
        return;
    m.hot = i;
    if (searching() && i >= 0) {
        // Keep it in view
        size_t rows = rows_showing();
        if ((size_t)i < m.top)
            m.top = (size_t)i;
        else if (rows > 0 && (size_t)i >= m.top + rows)
            m.top = (size_t)i - rows + 1;
    }
    invalidate_list();
}

static void move_hot(int step) {
    int n = (int)line_count();
    if (n == 0)
        return;
    if (searching()) {
        int i = (m.hot < 0) ? (step > 0 ? 0 : n - 1) : m.hot + step;
        if (i < 0) i = 0;
        if (i >= n) i = n - 1;
        set_hot(i);
        return;
    }
    // Items: one at a time, wrapping, over separators
    int dir = step > 0 ? 1 : -1;
    int i = m.hot;
    for (int k = 0; k < n; k++) {
        i = (i < 0) ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
        if (selectable(i)) {
            set_hot(i);
            return;
        }
    }
}

//
// Choosing
//

// Close first, then act: the action may open windows or menus
static void choose(int i) {
    if (!selectable(i))
        return;
    if (searching()) {
        char path[PATH_MAX];
        strlcpy(path, search_hit((size_t)i)->path, sizeof(path));
        dwm_start_close();
        if (open_path != NULL)
            open_path(path);
        return;
    }
    item_t it = items[i];
    dwm_start_close();
    if (it.action != NULL)
        it.action(it.arg);
}

//
// Public
//

void dwm_start_set_items(const dwm_start_item_t* list, size_t count) {
    item_count = 0;
    for (size_t i = 0; list != NULL && i < count && item_count < DWM_START_MAX_ITEMS; i++) {
        item_t* it = &items[item_count++];
        memset(it, 0, sizeof(*it));
        it->flags  = list[i].flags;
        it->action = list[i].action;
        if (list[i].label != NULL) strlcpy(it->label, list[i].label, sizeof(it->label));
        if (list[i].image != NULL) strlcpy(it->image, list[i].image, sizeof(it->image));
        if (list[i].arg   != NULL) strlcpy(it->arg,   list[i].arg,   sizeof(it->arg));
    }
    if (!searching())
        m.hot = -1;
    invalidate_panel();
}

void dwm_start_set_open(dwm_start_action_t open) {
    open_path = open;
}

static void set_power_action(dwm_start_action_t* slot, dwm_start_action_t action) {
    invalidate_panel();             // Its old size
    *slot = action;
    m.power_hot = m.power_down = PWR_NONE;
    if (m.open && !m.tray)
        place_box();
    invalidate_panel();             // Its new size
}

void dwm_start_set_shutdown(dwm_start_action_t action) {
    set_power_action(&shutdown_action, action);
}

void dwm_start_set_restart(dwm_start_action_t action) {
    set_power_action(&restart_action, action);
}

void dwm_start_set_search_roots(const char* const* roots, size_t count) {
    search_set_roots(roots, count);
}

void dwm_start_open(void) {
    if (m.open)
        return;
    m.tray = false;
    start_open_panel();
}

void dwm_start_open_tray(int index) {
    void* owner;
    uint32_t id;
    if (m.open || !tray_icon_key(index, &owner, &id))
        return;
    m.tray       = true;
    m.tray_owner = owner;
    m.tray_id    = id;
    start_open_panel();
}

void dwm_start_tray_changed(void* owner, uint32_t id) {
    if (!dwm_start_is_tray() || owner != m.tray_owner || id != m.tray_id)
        return;
    if (m.top > tray_max_top())
        m.top = tray_max_top();     // The list got shorter
    invalidate_panel();
}

void dwm_start_tray_gone(void* owner, uint32_t id) {
    if (dwm_start_is_tray() && owner == m.tray_owner && id == m.tray_id)
        dwm_start_close();
}

static void start_open_panel(void) {
    dwm_menu_close();
    if (dwm_icon_editing() != 0)
        dwm_icon_edit_end(true);
    input_cancel();                 // A drag or capture in progress ends here

    ui_edit_init(&m.box, "", SEARCH_QUERY_MAX - 1, 0);
    m.open     = true;
    m.hot      = -1;
    m.top      = 0;
    m.pressed  = false;
    m.box_drag = false;
    m.sb_drag  = false;
    m.row_down  = -1;
    m.prompt_on = false;
    m.power_hot  = PWR_NONE;
    m.power_down = PWR_NONE;
    m.btn_hot    = TBTN_NONE;
    m.btn_down   = TBTN_NONE;
    place_box();
    place_box();                    // Again: the panel's height follows the box's
    search_cancel();
    invalidate_panel();
}

void dwm_start_close(void) {
    if (!m.open)
        return;
    if (m.tray && tray_prompting())
        tray_tell(DWM_PANEL_CANCEL);        // Closed with the question open
    invalidate_panel();
    m.open = false;
    m.tray = false;
    m.tray_owner = NULL;
    m.sb_drag = false;
    m.row_down = -1;
    m.prompt_on = false;
    m.power_hot = PWR_NONE;
    m.power_down = PWR_NONE;
    m.btn_hot = TBTN_NONE;
    m.btn_down = TBTN_NONE;
    search_cancel();
}

gfx_rect_t dwm_start_rect(void) {
    return m.open ? panel_rect() : gfx_rect(0, 0, 0, 0);
}

void dwm_start_tray_scroll(size_t* top, size_t* max) {
    *top = dwm_start_is_tray() ? m.top : 0;
    *max = dwm_start_is_tray() ? tray_max_top() : 0;
}

gfx_rect_t dwm_start_tray_button(uint32_t param) {
    if (!dwm_start_is_tray())
        return gfx_rect(0, 0, 0, 0);
    switch (param) {
        case DWM_PANEL_REFRESH:    return tray_button_rect(TBTN_REFRESH);
        case DWM_PANEL_ACCEPT:     return tray_button_rect(TBTN_CONNECT);
        case DWM_PANEL_DISCONNECT: return tray_button_rect(TBTN_DISCONNECT);
        default:                   return gfx_rect(0, 0, 0, 0);
    }
}

bool dwm_start_is_tray(void) {
    return m.open && m.tray;
}

bool dwm_start_is_open(void) {
    return m.open;
}

//
// For the rest of the DWM
//

// The box's text changed: search again
static void text_changed(void) {
    place_box();
    m.hot = -1;
    m.top = 0;
    search_begin(m.box.text);
    invalidate_panel();
}

void start_key(uint16_t key, uint8_t mods) {
    if (!m.open)
        return;
    if (m.tray) {
        tray_key(key, mods);
        return;
    }
    switch (key) {
        case KEY_ESCAPE:
            if (searching()) {
                ui_edit_init(&m.box, "", SEARCH_QUERY_MAX - 1, 0);
                text_changed();
            } else {
                dwm_start_close();
            }
            return;
        case KEY_UP:        move_hot(-1);   return;
        case KEY_DOWN:      move_hot(+1);   return;
        case KEY_TAB:       move_hot((mods & INPUT_MOD_SHIFT) ? -1 : +1); return;
        case KEY_PAGE_UP:   move_hot(-(int)(rows_showing() > 1 ? rows_showing() - 1 : 1)); return;
        case KEY_PAGE_DOWN: move_hot(+(int)(rows_showing() > 1 ? rows_showing() - 1 : 1)); return;
        case KEY_ENTER:
            if (m.hot < 0)
                move_hot(+1);       // Nothing highlighted: the first line
            choose(m.hot);
            return;
        default:
            break;
    }

    char before[UI_EDIT_MAX];
    memcpy(before, m.box.text, m.box.len + 1);
    if (ui_edit_key(&m.box, key, mods) == UI_EDIT_IGNORED)
        return;
    if (strcmp(before, m.box.text) != 0)
        text_changed();
    else
        compositor_invalidate(m.box.rect);      // Cursor or selection moved
}

bool start_mouse(int32_t x, int32_t y, bool moved, uint8_t pressed, uint8_t released, uint8_t held,
                 uint8_t mods, uint8_t clicks) {
    if (!m.open)
        return false;
    gfx_rect_t panel = panel_rect();
    bool in_panel = contains(panel, x, y);

    if (m.tray)
        return tray_mouse(x, y, moved, pressed, released, held, mods, clicks, in_panel);

    if (moved) {
        if (m.box_drag) {
            ui_edit_drag(&m.box, x);
            compositor_invalidate(m.box.rect);
        } else if (in_panel) {
            set_hot(line_at(x, y));
        }
        power_button_t power_hot = m.box_drag ? PWR_NONE : power_at(x, y);
        if (power_hot != m.power_hot) {
            m.power_hot = power_hot;
            invalidate_panel();         // The button and its label
        }
    }

    if (pressed) {
        if (!in_panel) {
            dwm_start_close();
            // The Start button closes it and stays closed; anywhere else
            // the press goes on to do what it does
            return contains(taskbar_start_hit_rect(), x, y);
        }
        m.pressed = true;
        if ((pressed & INPUT_BTN_LEFT) && power_at(x, y) != PWR_NONE) {
            m.power_down = power_at(x, y);
            invalidate_panel();
            return true;
        }
        place_box();
        if ((pressed & INPUT_BTN_LEFT) && ui_edit_click(&m.box, x, y, mods, clicks)) {
            m.box_drag = true;
            compositor_invalidate(m.box.rect);
        }
        return true;
    }

    if (released) {
        bool was_drag = m.box_drag;
        if (held == 0) {
            m.box_drag = false;
        }
        if (!m.pressed)
            return in_panel;        // The press that opened it (or one before): nothing
        if (held == 0)
            m.pressed = false;
        if ((released & INPUT_BTN_LEFT) && m.power_down != PWR_NONE) {
            // Down and up on the same button: let go anywhere else, nothing
            power_button_t which = m.power_down;
            m.power_down = PWR_NONE;
            invalidate_panel();
            dwm_start_action_t action = power_action(which);
            if (power_at(x, y) == which && action != NULL) {
                dwm_start_close();
                action("");
            }
            return true;
        }
        if (!was_drag && in_panel && (released & INPUT_BTN_LEFT)) {
            int i = line_at(x, y);
            if (selectable(i))
                choose(i);
        }
        return true;
    }
    return in_panel || m.box_drag;
}

bool start_wheel(int32_t x, int32_t y, int16_t notches, uint8_t mods) {
    if (!m.open || !contains(panel_rect(), x, y))
        return false;
    if (notches == 0 || m.sb_drag || m.box_drag)
        return true;
    if (m.tray) {
        long page = tray_rows_showing() > 1 ? (long)tray_rows_showing() - 1 : 1;
        long step = (mods & INPUT_MOD_CTRL) ? page : WHEEL_ROWS;
        tray_scroll_to((long)m.top + notches * step);
        m.hot = tray_row_at(x, y);          // What is under the pointer now
        return true;
    }
    if (!searching())
        return true;                        // The items always fit
    size_t rows = rows_showing(), n = search_count();
    long page = rows > 1 ? (long)rows - 1 : 1;
    long step = (mods & INPUT_MOD_CTRL) ? page : WHEEL_ROWS;
    long max = n > rows ? (long)(n - rows) : 0;
    long top = (long)m.top + notches * step;
    if (top > max) top = max;
    if (top < 0) top = 0;
    if ((size_t)top != m.top) {
        m.top = (size_t)top;
        int hot = line_at(x, y);
        m.hot = selectable(hot) ? hot : -1;
        invalidate_list();
    }
    return true;
}

// Once per round: walk on with the search, show what it found
void start_tick(void) {
    if (!m.open || !search_busy())
        return;
    if (search_step(SEARCH_STEP_MS))
        invalidate_list();
}

bool start_busy(void) {
    return m.open && search_busy();
}

//
// Painting
//

static uint32_t dimmed(uint32_t color) {
    return (((color >> 24) / 2) << 24) | (color & 0x00FFFFFFu);
}

static void paint_items(gfx_surface_t* s, gfx_rect_t area) {
    const dwm_theme_t* t = dwm_theme();
    int32_t lh = dwm_text_height();
    for (size_t i = 0; i < item_count; i++) {
        gfx_rect_t r = item_rect(i);
        if (!rect_overlaps(area, r))
            continue;
        const item_t* it = &items[i];
        if (it->flags & DWM_START_SEPARATOR) {
            gfx_draw_hline(s, r.x + PAD, r.y + r.h / 2, r.w - 2 * PAD, t->menu.separator);
            gfx_draw_hline(s, r.x + PAD, r.y + r.h / 2 + 1, r.w - 2 * PAD, t->menu.border);
            continue;
        }
        if ((int)i == m.hot)
            gfx_fill_rect(s, r.x, r.y, r.w, r.h, t->menu.highlight);
        int32_t tx = r.x + PAD;
        if (it->image[0] != '\0') {
            small_image_draw(s, imagelib_get(it->image), tx, r.y + (r.h - ICON_SIZE) / 2, ICON_SIZE);
        }
        tx += ICON_SIZE + PAD;
        char text[DWM_START_LABEL_MAX + 4];
        size_t len = dwm_text_ellipsize(it->label, strlen(it->label), r.x + r.w - PAD - tx, "..", false,
                                        text, sizeof(text));
        dwm_text_draw(s, tx, r.y + (r.h - lh) / 2, text, len, t->menu.text);
    }
}

// A power sign, for while "ui_shutdown" isn't installed: a ring open at
// the top with a bar down into the gap
static void draw_power_sign(gfx_surface_t* s, int32_t x, int32_t y, int32_t size, uint32_t c, uint32_t bg) {
    int32_t cx = x + size / 2, cy = y + size / 2 + 1, r = size / 2 - 3;
    gfx_draw_circle(s, cx, cy, r, c);
    gfx_draw_circle(s, cx, cy, r - 1, c);
    gfx_fill_rect(s, cx - 3, cy - r - 1, 7, r / 2 + 2, bg);     // The gap
    gfx_fill_rect(s, cx - 1, y + 1, 2, size / 2, c);           // The bar
}

// A restart arrow, for while "ui_restart" isn't installed: a ring open at
// the top right, an arrow head on its upper end
static void draw_restart_sign(gfx_surface_t* s, int32_t x, int32_t y, int32_t size, uint32_t c, uint32_t bg) {
    int32_t cx = x + size / 2, cy = y + size / 2 + 1, r = size / 2 - 3;
    gfx_draw_circle(s, cx, cy, r, c);
    gfx_draw_circle(s, cx, cy, r - 1, c);
    gfx_fill_rect(s, cx + 1, cy - r - 1, r + 1, r - 1, bg);    // The gap
    int32_t hx = cx + r - 1, hy = cy - 3;                     // The head: up from the ring's end
    for (int32_t i = 0; i < 5; i++)
        gfx_draw_hline(s, hx - i, hy - 4 + i, 2 * i + 1, c);
}

static void paint_power_button(gfx_surface_t* s, power_button_t which) {
    gfx_rect_t b = power_rect(which);
    if (b.w == 0)
        return;
    const dwm_theme_t* t = dwm_theme();
    uint32_t bg = t->menu.background | 0xFF000000u;
    if (m.power_hot == which || m.power_down == which) {
        gfx_fill_rect(s, b.x, b.y, b.w, b.h, t->menu.highlight);
        bg = t->menu.highlight | 0xFF000000u;
        if (m.power_down == which && m.power_hot == which)
            gfx_draw_rect(s, b.x, b.y, b.w, b.h, t->menu.border);
    }
    int32_t ix = b.x + POWER_PAD, iy = b.y + POWER_PAD;
    const image_t* image = imagelib_get(which == PWR_OFF ? POWER_IMAGE : RESTART_IMAGE);
    if (image != NULL)
        small_image_draw(s, image, ix, iy, POWER_SIZE);
    else if (which == PWR_OFF)
        draw_power_sign(s, ix, iy, POWER_SIZE, t->menu.text, bg);
    else
        draw_restart_sign(s, ix, iy, POWER_SIZE, t->menu.text, bg);
}

static void paint_power(gfx_surface_t* s, gfx_rect_t area) {
    if (power_area_h() == 0)
        return;
    gfx_rect_t off = power_rect(PWR_OFF), restart = power_rect(PWR_RESTART);
    gfx_rect_t left = restart.w != 0 ? restart : off;     // The leftmost button
    if (!rect_overlaps(area, gfx_rect(content_x(), left.y, content_w(), left.h)))
        return;
    paint_power_button(s, PWR_OFF);
    paint_power_button(s, PWR_RESTART);

    // What the one under the pointer does, left of the buttons
    if (m.power_hot != PWR_NONE) {
        const dwm_theme_t* t = dwm_theme();
        const char* label = (m.power_hot == PWR_OFF) ? POWER_LABEL : RESTART_LABEL;
        int32_t lh = dwm_text_height();
        int32_t w = dwm_text_width(label, strlen(label));
        dwm_text_draw(s, left.x - PAD - w, left.y + (left.h - lh) / 2, label, strlen(label),
                      t->client.text_muted);
    }
}

static void paint_status(gfx_surface_t* s, gfx_rect_t l) {
    const dwm_theme_t* t = dwm_theme();
    char text[96];
    size_t found = search_count() + search_more();
    if (search_busy())
        snprintf(text, sizeof(text), "Searching... %u found in %u folders", (unsigned)found,
                 (unsigned)search_folders());
    else if (found == 0)
        snprintf(text, sizeof(text), "Nothing found in %u folders", (unsigned)search_folders());
    else if (search_more() > 0)
        snprintf(text, sizeof(text), "%u found, the first %u shown", (unsigned)found, (unsigned)search_count());
    else
        snprintf(text, sizeof(text), "%u found%s", (unsigned)found, search_partial() ? " (some folders skipped)" : "");
    char cut[100];
    size_t len = dwm_text_ellipsize(text, strlen(text), l.w - 2 * PAD, "..", false, cut, sizeof(cut));
    dwm_text_draw(s, l.x + PAD, l.y + 2, cut, len, t->client.text_muted);
    gfx_draw_hline(s, l.x + PAD, l.y + status_h() - 2, l.w - 2 * PAD, t->menu.separator);
}

static void paint_results(gfx_surface_t* s, gfx_rect_t area) {
    const dwm_theme_t* t = dwm_theme();
    gfx_rect_t l = list_rect();
    if (rect_overlaps(area, gfx_rect(l.x, l.y, l.w, status_h())))
        paint_status(s, l);

    int32_t lh = dwm_text_height();
    const image_t* folder = imagelib_get("icon_folder");
    const image_t* file   = imagelib_get("icon_file");
    for (size_t i = m.top; i < search_count(); i++) {
        gfx_rect_t r = result_rect(i);
        if (r.h == 0)
            break;
        if (!rect_overlaps(area, r))
            continue;
        const search_hit_t* hit = search_hit(i);
        if ((int)i == m.hot)
            gfx_fill_rect(s, r.x, r.y, r.w, r.h, t->menu.highlight);

        small_image_draw(s, hit->folder ? folder : file, r.x + PAD, r.y + (r.h - ICON_SIZE) / 2, ICON_SIZE);
        int32_t tx = r.x + PAD + ICON_SIZE + PAD;
        int32_t room = r.x + r.w - PAD - tx;

        // Name, then the folder it is in
        const char* slash = strrchr(hit->path, '/');
        const char* name = slash != NULL ? slash + 1 : hit->path;
        size_t dir_len = (slash != NULL && slash != hit->path) ? (size_t)(slash - hit->path) : 1;
        char text[PATH_MAX];
        size_t len = dwm_text_ellipsize(name, strlen(name), room, "..", false, text, sizeof(text));
        dwm_text_draw(s, tx, r.y + 2, text, len, t->menu.text);

        // A long folder keeps its end: "..." then as much of the tail as fits
        const char* dir = hit->path;
        size_t skip = 0;
        while (skip < dir_len && dwm_text_width(dir + skip, dir_len - skip) +
                                 (skip ? dwm_text_width("...", 3) : 0) > room)
            skip++;
        if (skip > 0) {
            int n = snprintf(text, sizeof(text), "...%.*s", (int)(dir_len - skip), dir + skip);
            len = n > 0 ? (size_t)n : 0;
        } else {
            memcpy(text, dir, dir_len);
            len = dir_len;
        }
        dwm_text_draw(s, tx, r.y + 2 + lh, text, len, (int)i == m.hot ? t->client.text_soft : t->client.text_muted);
    }

    // More below / above than shows
    size_t rows = rows_showing();
    if (search_count() > m.top + rows || m.top > 0) {
        char more[32];
        snprintf(more, sizeof(more), "%u-%u of %u", (unsigned)(m.top + 1),
                 (unsigned)(m.top + rows < search_count() ? m.top + rows : search_count()),
                 (unsigned)search_count());
        int32_t w = dwm_text_width(more, strlen(more));
        dwm_text_draw(s, l.x + l.w - PAD - w, l.y + 2, more, strlen(more), dimmed(t->client.text_muted));
    }
}

static uint32_t mix(uint32_t a, uint32_t b, uint32_t alpha) {
    uint32_t out = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        out |= ((ca * alpha + cb * (255 - alpha)) / 255) << sh;
    }
    return out;
}

// Signal bars: four, rising left to right, `n` lit
static void draw_bars(gfx_surface_t* s, int32_t x, int32_t bottom, int n, uint32_t on, uint32_t off) {
    for (int i = 0; i < 4; i++) {
        int32_t bh = 3 + 3 * i;
        gfx_fill_rect(s, x + i * 3, bottom - bh, 2, bh, i < n ? on : off);
    }
}

// A small padlock
static void draw_lock(gfx_surface_t* s, int32_t x, int32_t y, uint32_t c) {
    gfx_draw_rect(s, x + 1, y, 5, 4, c);
    gfx_fill_rect(s, x, y + 3, 7, 5, c);
}

// A small triangle in a scroll bar button
static void draw_arrow(gfx_surface_t* s, gfx_rect_t r, bool up, uint32_t c) {
    int32_t cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    for (int32_t i = 0; i < 3; i++)
        gfx_draw_hline(s, cx - i, up ? cy - 1 + i : cy + 1 - i, 2 * i + 1, c);
}

// A circular arrow, for while "ui_refresh" isn't installed: most of a ring,
// open at the top right, an arrowhead on its end
static void draw_refresh_sign(gfx_surface_t* s, int32_t x, int32_t y, int32_t size, uint32_t c, uint32_t bg) {
    int32_t cx = x + size / 2, cy = y + size / 2, r = size / 2 - 2;
    gfx_draw_circle(s, cx, cy, r, c);
    gfx_draw_circle(s, cx, cy, r - 1, c);
    gfx_fill_rect(s, cx + 1, y, size - size / 2 - 1, size / 2 - 1, bg);    // The gap, top right
    for (int32_t i = 0; i < 4; i++)                                        // The arrowhead, pointing right
        gfx_draw_vline(s, cx + 1 + i, cy - r - 3 + i, 7 - 2 * i, c);
}

// A labelled button: the theme's button colours, the edge lit under the
// pointer, filled while held down on it
static void paint_tray_button(gfx_surface_t* s, tray_button_t which, const char* label) {
    gfx_rect_t b = tray_button_rect(which);
    if (b.w == 0)
        return;
    const dwm_theme_t* t = dwm_theme();
    bool hot = m.btn_hot == which, down = m.btn_down == which && hot;
    gfx_fill_rect(s, b.x, b.y, b.w, b.h, down ? t->button.fill_pressed : t->button.fill);
    gfx_draw_rect(s, b.x, b.y, b.w, b.h, down ? t->button.border_pressed : hot ? t->button.border_active
                                                                               : t->button.border);
    size_t len = strlen(label);
    dwm_text_draw(s, b.x + (b.w - dwm_text_width(label, len)) / 2, b.y + (b.h - dwm_text_height()) / 2, label, len,
                  down ? t->button.fill : t->button.text);
}

static void paint_tray(gfx_surface_t* s) {
    const dwm_theme_t* t = dwm_theme();
    const dwm_tray_panel_t* list = tray_list();
    gfx_rect_t p = panel_rect();
    int32_t lh = dwm_text_height();
    uint32_t bg    = t->menu.background | 0xFF000000u;
    uint32_t text  = t->menu.text;
    uint32_t muted = t->client.text_muted;
    uint32_t dim   = mix(text, bg, 60);
    char cut[64];
    size_t len;

    // Heading: the refresh button and the title left, status right
    int32_t x = content_x(), w = content_w();
    int32_t hy = p.y + BORDER + (tray_head_h() - lh) / 2;
    const char* status = list != NULL ? list->status : "";
    int32_t sw = dwm_text_width(status, strlen(status));
    if (sw > w / 2) sw = w / 2;
    int32_t title_x = x + PAD;
    gfx_rect_t rb = tray_button_rect(TBTN_REFRESH);
    if (rb.w > 0) {
        uint32_t under = bg;
        if (m.btn_hot == TBTN_REFRESH || m.btn_down == TBTN_REFRESH) {
            gfx_fill_rect(s, rb.x, rb.y, rb.w, rb.h, t->menu.highlight);
            under = t->menu.highlight | 0xFF000000u;
            if (m.btn_down == TBTN_REFRESH && m.btn_hot == TBTN_REFRESH)
                gfx_draw_rect(s, rb.x, rb.y, rb.w, rb.h, t->menu.border);
        }
        const image_t* image = imagelib_get(REFRESH_IMAGE);
        if (image != NULL)
            small_image_draw(s, image, rb.x + REFRESH_PAD, rb.y + REFRESH_PAD, REFRESH_SIZE);
        else
            draw_refresh_sign(s, rb.x + REFRESH_PAD, rb.y + REFRESH_PAD, REFRESH_SIZE, text, under);
        title_x = rb.x + rb.w + PAD / 2;
    }
    if (list != NULL) {
        len = dwm_text_ellipsize(list->title, strlen(list->title), x + w - 2 * PAD - sw - title_x, "..", false, cut,
                                 sizeof(cut));
        dwm_text_draw(s, title_x, hy, cut, len, text);
        len = dwm_text_ellipsize(status, strlen(status), sw, "..", false, cut, sizeof(cut));
        dwm_text_draw(s, x + w - PAD - dwm_text_width(cut, len), hy, cut, len, list->busy ? t->client.accent : muted);
    }
    gfx_draw_hline(s, x, p.y + BORDER + tray_head_h(), w, t->menu.separator);

    // The rows
    gfx_rect_t r = tray_rows_rect();
    int32_t rh = tray_row_h();
    int32_t right = r.x + r.w - SCROLL_W - PAD / 2;
    if (m.top > tray_max_top())
        m.top = tray_max_top();
    gfx_save(s);
    gfx_rect_t clip = gfx_rect(r.x, r.y, r.w - SCROLL_W, r.h);
    gfx_intersect_clip(s, &clip);
    size_t showing = tray_rows_showing();
    for (size_t i = 0; list != NULL && i < showing && m.top + i < list->count; i++) {
        const dwm_tray_row_t* row = &list->rows[m.top + i];
        int32_t y = r.y + (int32_t)i * rh, mid = y + rh / 2, ty = y + (rh - lh) / 2;
        bool note = (row->flags & DWM_PANEL_ROW_NOTE) != 0;
        uint32_t c = (note || (row->flags & DWM_PANEL_ROW_MUTED)) ? muted : text;
        if (row->flags & DWM_PANEL_ROW_ACTIVE)
            c = t->client.accent;
        if ((int)(m.top + i) == m.hot)
            gfx_fill_rect(s, r.x, y, r.w - SCROLL_W - 1, rh, t->menu.highlight);
        int32_t tx = r.x + PAD;
        if (!note) {
            if (row->level != DWM_PANEL_NO_LEVEL)
                draw_bars(s, tx, mid + 5, row->level, text, dim);
            if (row->flags & DWM_PANEL_ROW_LOCK)
                draw_lock(s, tx + BARS_W + GAP, mid - 4, muted);
            tx += BARS_W + GAP + LOCK_W + GAP;
        }
        int32_t side_w = dwm_text_width(row->side, strlen(row->side));
        if (side_w > 0) {
            dwm_text_draw(s, right - side_w, ty, row->side, strlen(row->side), muted);
            side_w += GAP;
        }
        len = dwm_text_ellipsize(row->text, strlen(row->text), right - side_w - tx, "..", false, cut, sizeof(cut));
        dwm_text_draw(s, tx, ty, cut, len, c);
    }
    gfx_restore(s);

    // The scroll bar: always there, its thumb filling the track while
    // everything fits
    scrollbar_t b = tray_scrollbar();
    bool can = tray_max_top() > 0;
    gfx_draw_vline(s, b.up.x - 1, r.y, r.h, t->menu.separator);
    gfx_fill_rect(s, b.track.x, b.track.y, b.track.w, b.track.h, mix(text, bg, 25));
    gfx_fill_rect(s, b.thumb.x, b.thumb.y, b.thumb.w, b.thumb.h, can ? (m.sb_drag ? text : muted) : dim);
    draw_arrow(s, b.up,   true,  can && m.top > 0 ? text : dim);
    draw_arrow(s, b.down, false, can && m.top < tray_max_top() ? text : dim);

    // The question and its box
    if (tray_prompting()) {
        int32_t qy = r.y + r.h;
        gfx_draw_hline(s, x, qy, w, t->menu.separator);
        len = dwm_text_ellipsize(list->prompt, strlen(list->prompt), w - 2 * PAD, "..", false, cut, sizeof(cut));
        dwm_text_draw(s, x + PAD, qy + 4, cut, len, text);
        tray_place_box();
        ui_edit_style_t style = {
            .background = t->edit.background,
            .border     = t->edit.border,
            .text       = t->edit.text,
            .cursor     = t->edit.cursor,
            .selection  = t->edit.selection,
        };
        ui_edit_paint(&m.box, s, &style);
    } else if (tray_has_disconnect()) {
        gfx_draw_hline(s, x, r.y + r.h, w, t->menu.separator);     // Above the button row
    }
    paint_tray_button(s, TBTN_CONNECT, CONNECT_LABEL);
    paint_tray_button(s, TBTN_DISCONNECT, DISCONNECT_LABEL);
}

void start_paint(gfx_surface_t* s, gfx_rect_t area) {
    if (!m.open)
        return;
    gfx_rect_t p = panel_rect();
    if (!rect_overlaps(area, p))
        return;
    const dwm_theme_t* t = dwm_theme();

    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_fill_rect(s, p.x + BORDER, p.y + BORDER, p.w - 2 * BORDER, p.h - 2 * BORDER, t->menu.background);
    gfx_set_blend(s, old);
    gfx_draw_rect(s, p.x, p.y, p.w, p.h, t->menu.border);
    int32_t strip_x = m.tray ? p.x + p.w - BORDER - STRIP_W : p.x + BORDER;   // Mirrored too
    gfx_fill_gradient(s, strip_x, p.y + BORDER, STRIP_W, p.h - 2 * BORDER,
                      t->taskbar.gradient_high, t->taskbar.gradient_low, GFX_GRADIENT_VERTICAL);
    if (m.tray) {
        paint_tray(s);
        return;
    }

    // Search box, its hint while empty
    place_box();
    ui_edit_style_t style = {
        .background = t->edit.background,
        .border     = t->edit.border,
        .text       = t->edit.text,
        .cursor     = t->edit.cursor,
        .selection  = t->edit.selection,
    };
    if (rect_overlaps(area, m.box.rect)) {
        ui_edit_paint(&m.box, s, &style);
        if (m.box.len == 0) {
            int32_t lh = dwm_text_height();
            gfx_rect_t b = m.box.rect;
            gfx_save(s);
            gfx_intersect_clip(s, &b);
            dwm_text_draw(s, b.x + 6, b.y + (b.h - lh) / 2, PLACEHOLDER, strlen(PLACEHOLDER),
                          dimmed(t->client.text_muted));
            gfx_restore(s);
        }
    }

    gfx_rect_t l = list_rect();
    gfx_draw_hline(s, l.x, l.y + l.h, l.w, t->menu.separator);     // Above the button row and the search box
    paint_power(s, area);
    gfx_save(s);
    gfx_intersect_clip(s, &l);
    if (searching())
        paint_results(s, area);
    else
        paint_items(s, area);
    gfx_restore(s);
}

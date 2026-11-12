/*
 * System tray (tray.h): the icons programs put on the taskbar, their
 * tooltips, clicks and menus.
 *
 * The icons sit in one array in the order they came (the oldest leftmost);
 * a removal closes the gap. Everything an icon shows is the DWM's own copy,
 * made when the request came: the picture fit into DWM_TRAY_ICON_PX square
 * and the tooltip with control characters blanked.
 */

#include <errno.h>
#include <string.h>
#include <os/input.h>

#include "compositor.h"
#include "dirty.h"
#include "internal.h"
#include "taskbar.h"
#include "theme.h"
#include "timer.h"
#include "tray.h"

#define ICON_GAP            6       // Between icons, and before the first
#define HOT_PAD             2       // Hover highlight around an icon
#define TIP_PAD_X           6
#define TIP_PAD_Y           3
#define TIP_LIFT            4       // Tooltip bottom above the strip
#define DOUBLE_CLICK_MS     500

typedef struct {
    void*      owner;
    uint32_t   pid;
    uint32_t   id;
    uint64_t   menu_until;          // A menu may open until then (0: not now)
    int32_t    click_x, click_y;    // The pointer at the last click (screen): menus open there
    gfx_rect_t rect;                // Screen (tray_layout)
    bool       panel;               // A left click opens the tray panel (WM_TRAY_PANEL)
    // The menu kept for a right click (tray_set_menu), 0 items: none
    size_t     menu_count;
    char       menu_labels[DWM_TRAY_MENU_MAX][DWM_MENU_LABEL_MAX];
    uint32_t   menu_ids[DWM_TRAY_MENU_MAX];
    uint32_t   menu_flags[DWM_TRAY_MENU_MAX];
    char       tip[DWM_TRAY_TIP_MAX];
    uint32_t   pixels[DWM_TRAY_ICON_PX * DWM_TRAY_ICON_PX];    // Straight alpha
} icon_t;

static icon_t     icons[DWM_TRAY_MAX];
static size_t     count;
static uint32_t   serial = 1;
static uint32_t   places = 1;   // Bumped when icons come or go: an index may name another icon
static gfx_rect_t area;             // The icons' part of the strip
static int        hover = -1;

// The tray panel's list, for the icon that sent it last
static struct {
    void*            owner;         // NULL: none
    uint32_t         id;
    size_t           total;
    dwm_tray_panel_t list;
    char             text[DWM_TRAY_TEXT_MAX];   // The prompt's answer, until taken
    bool             has_text;
} panel;

static dwm_tray_panel_fn on_panel;

static void wipe(void* p, size_t n) {
    volatile uint8_t* v = (volatile uint8_t*)p;
    while (n-- > 0)
        *v++ = 0;
}

static dwm_tray_click_fn   on_click;
static dwm_tray_command_fn on_command;

// The last click, for double clicks
static struct {
    void*    owner;
    uint32_t id;
    uint8_t  button;
    uint64_t time;
} last_click;

// The tray menu open (or last opened): who it is for, and the owner's ids
// for its items (the DWM's items carry their index)
static struct {
    void*    owner;                 // NULL: gone, choosing does nothing
    uint32_t id;
    uint32_t commands[DWM_TRAY_MENU_MAX];
} menu_for;

//
// Helpers
//

static icon_t* find(void* owner, uint32_t id) {
    for (size_t i = 0; i < count; i++) {
        if (icons[i].owner == owner && icons[i].id == id)
            return &icons[i];
    }
    return NULL;
}

static bool size_ok(const uint32_t* pixels, int32_t w, int32_t h) {
    return pixels != NULL && w > 0 && h > 0 && w <= DWM_TRAY_SOURCE_MAX && h <= DWM_TRAY_SOURCE_MAX;
}

// Fit the w x h picture into a DWM_TRAY_ICON_PX square, centered and keeping
// its shape: each pixel the alpha-weighted average of the source pixels it
// covers (at least one, so small pictures grow by repeating pixels)
static void fit(uint32_t* dst, const uint32_t* src, int32_t w, int32_t h) {
    const int32_t size = DWM_TRAY_ICON_PX;
    int32_t dw = size, dh = size;
    if (w >= h) dh = (h * size + w / 2) / w;
    else        dw = (w * size + h / 2) / h;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int32_t ox = (size - dw) / 2, oy = (size - dh) / 2;

    memset(dst, 0, (size_t)size * (size_t)size * sizeof(uint32_t));
    for (int32_t dy = 0; dy < dh; dy++) {
        int32_t y0 = dy * h / dh, y1 = (dy + 1) * h / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int32_t dx = 0; dx < dw; dx++) {
            int32_t x0 = dx * w / dw, x1 = (dx + 1) * w / dw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int32_t y = y0; y < y1; y++) {
                for (int32_t x = x0; x < x1; x++) {
                    uint32_t c = src[y * w + x];
                    uint32_t ca = c >> 24;
                    n++;
                    a += ca;
                    r += ((c >> 16) & 0xFF) * ca;
                    g += ((c >> 8) & 0xFF) * ca;
                    b += (c & 0xFF) * ca;
                }
            }
            uint32_t px = 0;
            if (a > 0)
                px = ((a / n) << 24) | ((r / a) << 16) | ((g / a) << 8) | (b / a);
            dst[(oy + dy) * size + ox + dx] = px;
        }
    }
}

static void copy_tip(char* dst, const char* tip) {
    size_t i = 0;
    if (tip != NULL) {
        for (; i < DWM_TRAY_TIP_MAX - 1 && tip[i] != '\0'; i++) {
            unsigned char c = (unsigned char)tip[i];
            dst[i] = (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
        }
    }
    dst[i] = '\0';
}

static gfx_rect_t tip_rect(void);

static void changed(void) {
    serial++;
    if (serial == 0)
        serial = 1;
}

// Hide the tooltip (an icon went, so the hovered index means nothing now)
static void unhover(void) {
    if (hover >= 0)
        compositor_invalidate(tip_rect());
    hover = -1;
}

static void remove_at(size_t i) {
    void* owner = icons[i].owner;
    uint32_t id = icons[i].id;
    if (panel.owner == owner && panel.id == id)
        wipe(&panel, sizeof(panel));
    unhover();
    compositor_invalidate(icons[i].rect);
    memmove(&icons[i], &icons[i + 1], (count - i - 1) * sizeof(icon_t));
    count--;
    memset(&icons[count], 0, sizeof(icon_t));
    places++;
    changed();
    dwm_start_tray_gone(owner, id);         // Its panel closes
}

//
// Requests
//

void tray_set_handlers(dwm_tray_click_fn click, dwm_tray_command_fn command) {
    on_click   = click;
    on_command = command;
}

int tray_add(void* owner, uint32_t pid, uint32_t id, const uint32_t* pixels, int32_t w, int32_t h,
             const char* tip) {
    if (owner == NULL || !size_ok(pixels, w, h))
        return EINVAL;
    if (find(owner, id) != NULL)
        return EEXIST;
    size_t mine = 0;
    for (size_t i = 0; i < count; i++)
        if (icons[i].pid == pid)
            mine++;
    if (mine >= DWM_TRAY_PER_PROCESS)
        return EMFILE;
    if (count >= DWM_TRAY_MAX)
        return ENOSPC;

    icon_t* ic = &icons[count++];
    memset(ic, 0, sizeof(*ic));
    ic->owner = owner;
    ic->pid   = pid;
    ic->id    = id;
    fit(ic->pixels, pixels, w, h);
    copy_tip(ic->tip, tip);
    unhover();          // The strip is laid out again
    places++;
    changed();
    return 0;
}

int tray_modify(void* owner, uint32_t pid, uint32_t id, const uint32_t* pixels, int32_t w, int32_t h,
                const char* tip) {
    icon_t* ic = find(owner, id);
    if (ic == NULL)
        return ENOENT;
    if (ic->pid != pid)
        return EPERM;
    if (pixels != NULL && !size_ok(pixels, w, h))
        return EINVAL;
    if (pixels != NULL)
        fit(ic->pixels, pixels, w, h);
    if (tip != NULL) {
        int index = (int)(ic - icons);
        if (hover == index)
            compositor_invalidate(tip_rect());      // Old size
        copy_tip(ic->tip, tip);
        if (hover == index)
            compositor_invalidate(tip_rect());      // New size
    }
    compositor_invalidate(ic->rect);
    changed();
    return 0;
}

int tray_set_panel(void* owner, uint32_t pid, uint32_t id) {
    icon_t* ic = find(owner, id);
    if (ic == NULL)
        return ENOENT;
    if (ic->pid != pid)
        return EPERM;
    ic->panel = true;
    return 0;
}

bool tray_icon_panel(int index) {
    return index >= 0 && (size_t)index < count && icons[index].panel;
}

bool tray_icon_key(int index, void** owner, uint32_t* id) {
    if (index < 0 || (size_t)index >= count)
        return false;
    *owner = icons[index].owner;
    *id    = icons[index].id;
    return true;
}

// Text from a request: cut at the end of its field, control characters blanked
static void copy_text(char* out, const char* in, size_t size) {
    size_t i = 0;
    for (; i + 1 < size && in[i] != '\0'; i++)
        out[i] = ((unsigned char)in[i] < 0x20 || in[i] == 0x7F) ? ' ' : in[i];
    out[i] = '\0';
}

int tray_panel_rows(void* owner, uint32_t pid, uint32_t id, const char* title, const char* status,
                    uint32_t flags, size_t first, size_t total, const dwm_tray_row_t* rows, size_t n) {
    icon_t* ic = find(owner, id);
    if (ic == NULL)
        return ENOENT;
    if (ic->pid != pid)
        return EPERM;
    if (total > DWM_TRAY_PANEL_ROWS_MAX || first + n > total || (n > 0 && rows == NULL))
        return EINVAL;
    if (first == 0) {
        // A new list; a prompt and an answer waiting carry over
        char prompt[DWM_TRAY_TIP_MAX];
        bool secret = false, connect = false, same = panel.owner == owner && panel.id == id;
        uint32_t serial = panel.list.prompt_serial;
        if (same) {
            memcpy(prompt, panel.list.prompt, sizeof(prompt));
            secret = panel.list.secret;
            connect = panel.list.connect;
        } else {
            prompt[0] = '\0';
            wipe(panel.text, sizeof(panel.text));
            panel.has_text = false;
        }
        memset(&panel.list, 0, sizeof(panel.list));
        memcpy(panel.list.prompt, prompt, sizeof(prompt));
        panel.list.secret        = secret;
        panel.list.connect       = connect;
        panel.list.prompt_serial = serial;
        panel.owner = owner;
        panel.id    = id;
        panel.total = total;
        copy_text(panel.list.title,  title  != NULL ? title  : "", sizeof(panel.list.title));
        copy_text(panel.list.status, status != NULL ? status : "", sizeof(panel.list.status));
        panel.list.busy       = (flags & DWM_PANEL_BUSY) != 0;
        panel.list.refresh    = (flags & DWM_PANEL_HAS_REFRESH) != 0;
        panel.list.disconnect = (flags & DWM_PANEL_HAS_DISCONNECT) != 0;
    } else if (panel.owner != owner || panel.id != id || first != panel.list.count || total != panel.total) {
        return EINVAL;          // Not the next piece of the list being sent
    }
    for (size_t i = 0; i < n; i++) {
        dwm_tray_row_t* r = &panel.list.rows[first + i];
        copy_text(r->text, rows[i].text, sizeof(r->text));
        copy_text(r->side, rows[i].side, sizeof(r->side));
        r->level = rows[i].level > 4 ? DWM_PANEL_NO_LEVEL : rows[i].level;
        r->flags = rows[i].flags & (DWM_PANEL_ROW_LOCK | DWM_PANEL_ROW_MUTED | DWM_PANEL_ROW_NOTE |
                                    DWM_PANEL_ROW_CLICK | DWM_PANEL_ROW_ACTIVE);
    }
    panel.list.count = first + n;
    dwm_start_tray_changed(owner, id);
    return 0;
}

const dwm_tray_panel_t* tray_panel_get(void* owner, uint32_t id) {
    if (owner == NULL || panel.owner != owner || panel.id != id)
        return NULL;
    return &panel.list;
}

int tray_panel_prompt(void* owner, uint32_t pid, uint32_t id, const char* question, bool secret,
                      bool connect) {
    icon_t* ic = find(owner, id);
    if (ic == NULL)
        return ENOENT;
    if (ic->pid != pid)
        return EPERM;
    if (panel.owner != owner || panel.id != id) {
        wipe(&panel, sizeof(panel));        // Its first word to the panel
        panel.owner = owner;
        panel.id    = id;
    }
    copy_text(panel.list.prompt, question != NULL ? question : "", sizeof(panel.list.prompt));
    panel.list.secret  = secret;
    panel.list.connect = connect;
    panel.list.prompt_serial++;
    dwm_start_tray_changed(owner, id);
    return 0;
}

int tray_panel_take_text(void* owner, uint32_t pid, uint32_t id, char* out, size_t size) {
    icon_t* ic = find(owner, id);
    if (ic == NULL)
        return ENOENT;
    if (ic->pid != pid)
        return EPERM;
    if (panel.owner != owner || panel.id != id || !panel.has_text)
        return ENOENT;
    strlcpy(out, panel.text, size);
    wipe(panel.text, sizeof(panel.text));
    panel.has_text = false;
    return 0;
}

void tray_set_panel_handler(dwm_tray_panel_fn fn) {
    on_panel = fn;
}

void tray_panel_event(void* owner, uint32_t id, uint32_t param, const char* text) {
    if (owner == NULL || find(owner, id) == NULL)
        return;
    if (param == DWM_PANEL_ACCEPT || param == DWM_PANEL_CANCEL) {
        if (panel.owner == owner && panel.id == id) {
            panel.list.prompt[0] = '\0';
            wipe(panel.text, sizeof(panel.text));
            panel.has_text = false;
            if (param == DWM_PANEL_ACCEPT && text != NULL) {
                strlcpy(panel.text, text, sizeof(panel.text));
                panel.has_text = true;
            }
        }
    }
    if (on_panel != NULL)
        on_panel(owner, id, param);
}

int tray_remove(void* owner, uint32_t pid, uint32_t id) {
    icon_t* ic = find(owner, id);
    if (ic == NULL)
        return ENOENT;
    if (ic->pid != pid)
        return EPERM;
    remove_at((size_t)(ic - icons));
    return 0;
}

void tray_remove_owner(void* owner) {
    if (owner == NULL)
        return;
    for (size_t i = count; i-- > 0;) {
        if (icons[i].owner == owner)
            remove_at(i);
    }
    if (last_click.owner == owner)
        memset(&last_click, 0, sizeof(last_click));
    if (menu_for.owner == owner) {
        menu_close_for_user(&menu_for);     // Only if it is still the one showing
        menu_for.owner = NULL;
    }
}

// A tray menu item was chosen: the owner hears of it, if it and the icon
// are still there
static void menu_chosen(const dwm_menu_context_t* ctx) {
    if (ctx->id >= DWM_TRAY_MENU_MAX || menu_for.owner == NULL)
        return;
    if (find(menu_for.owner, menu_for.id) == NULL)
        return;
    if (on_command != NULL)
        on_command(menu_for.owner, menu_for.id, menu_for.commands[ctx->id]);
}

// Open `items` above icon `ic`. Its items carry their index; the owner's
// ids go in menu_for.
static bool open_menu(icon_t* ic, const dwm_menu_item_t* items, size_t n) {
    if (rect_empty(ic->rect))
        return false;       // Not on the strip (no room)
    dwm_menu_item_t built[DWM_TRAY_MENU_MAX];
    for (size_t i = 0; i < n; i++) {
        built[i]        = items[i];
        built[i].id     = (uint32_t)i;
        built[i].action = menu_chosen;
        built[i].state  = NULL;
        menu_for.commands[i] = items[i].id;
    }
    menu_for.owner = ic->owner;
    menu_for.id    = ic->id;
    dwm_menu_t menu = { built, n, &menu_for };

    // At the pointer, as a window's menu opens: over the taskbar it flips
    // up above it
    return dwm_menu_show(DWM_DESKTOP, &menu, ic->click_x, ic->click_y);
}

int tray_menu(void* owner, uint32_t pid, uint32_t id, const dwm_menu_item_t* items, size_t n) {
    icon_t* ic = find(owner, id);
    if (ic == NULL)
        return ENOENT;
    if (ic->pid != pid)
        return EPERM;
    if (items == NULL || n == 0 || n > DWM_TRAY_MENU_MAX)
        return EINVAL;
    uint64_t now = dwm_time_ms();
    if (ic->menu_until == 0 || now > ic->menu_until)
        return EPERM;       // No click on it just now
    if (!open_menu(ic, items, n))
        return EINVAL;
    ic->menu_until = 0;     // One per click
    return 0;
}

int tray_set_menu(void* owner, uint32_t pid, uint32_t id, const dwm_menu_item_t* items, size_t n) {
    icon_t* ic = find(owner, id);
    if (ic == NULL)
        return ENOENT;
    if (ic->pid != pid)
        return EPERM;
    if (n > DWM_TRAY_MENU_MAX || (n > 0 && items == NULL))
        return EINVAL;
    for (size_t i = 0; i < n; i++) {
        copy_text(ic->menu_labels[i], items[i].label != NULL ? items[i].label : "", DWM_MENU_LABEL_MAX);
        ic->menu_ids[i]   = items[i].id;
        ic->menu_flags[i] = items[i].flags;
    }
    ic->menu_count = n;
    return 0;
}

// A right click on an icon with a kept menu: open it now
static bool open_kept_menu(icon_t* ic) {
    if (ic->menu_count == 0)
        return false;
    dwm_menu_item_t items[DWM_TRAY_MENU_MAX];
    memset(items, 0, sizeof(items));
    for (size_t i = 0; i < ic->menu_count; i++) {
        items[i].label = ic->menu_labels[i];
        items[i].id    = ic->menu_ids[i];
        items[i].flags = ic->menu_flags[i];
    }
    return open_menu(ic, items, ic->menu_count);
}

size_t tray_count(void) {
    return count;
}

uint32_t tray_serial(void) {
    return serial;
}

uint32_t tray_places_serial(void) {
    return places;
}

int32_t tray_width(void) {
    if (count == 0)
        return 0;
    return (int32_t)count * (DWM_TRAY_ICON_PX + ICON_GAP) + ICON_GAP;
}

//
// The strip
//

void tray_layout(gfx_rect_t a) {
    area = a;
    int32_t y = a.y + (a.h - DWM_TRAY_ICON_PX) / 2;
    for (size_t i = 0; i < count; i++) {
        int32_t x = a.x + ICON_GAP + (int32_t)i * (DWM_TRAY_ICON_PX + ICON_GAP);
        // Past the room: no place, so it can't be hit or drawn
        icons[i].rect = (x + DWM_TRAY_ICON_PX <= a.x + a.w) ? gfx_rect(x, y, DWM_TRAY_ICON_PX, DWM_TRAY_ICON_PX)
                                                            : gfx_rect(0, 0, 0, 0);
    }
}

// An icon's rectangle grown a little: what is hit, hovered and highlighted
static gfx_rect_t hot_rect(size_t i) {
    gfx_rect_t r = icons[i].rect;
    if (rect_empty(r))
        return r;
    return gfx_rect(r.x - HOT_PAD, r.y - HOT_PAD, r.w + 2 * HOT_PAD, r.h + 2 * HOT_PAD);
}

int tray_icon_at(int32_t x, int32_t y) {
    for (size_t i = 0; i < count; i++) {
        gfx_rect_t r = hot_rect(i);
        if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h)
            return (int)i;
    }
    return -1;
}

gfx_rect_t tray_icon_rect(int index) {
    if (index < 0 || (size_t)index >= count)
        return gfx_rect(0, 0, 0, 0);
    return hot_rect((size_t)index);
}

// Above the hovered icon, kept on the screen; empty when there is no tip
static gfx_rect_t tip_rect(void) {
    if (hover < 0 || (size_t)hover >= count || icons[hover].tip[0] == '\0')
        return gfx_rect(0, 0, 0, 0);
    const icon_t* ic = &icons[hover];
    int32_t w = dwm_text_width(ic->tip, strlen(ic->tip)) + 2 * TIP_PAD_X;
    int32_t h = dwm_text_height() + 2 * TIP_PAD_Y;
    int32_t sw = compositor_width();
    int32_t x = ic->rect.x + ic->rect.w / 2 - w / 2;
    if (x + w > sw) x = sw - w;
    if (x < 0) x = 0;
    int32_t y = taskbar_rect().y - TIP_LIFT - h;
    if (y < 0) y = 0;
    return gfx_rect(x, y, w, h);
}

void tray_hover(int index) {
    if (index >= (int)count)
        index = -1;
    if (index == hover)
        return;
    if (hover >= 0) {
        compositor_invalidate(tip_rect());
        compositor_invalidate(hot_rect((size_t)hover));
    }
    hover = index;
    if (hover >= 0) {
        compositor_invalidate(tip_rect());
        compositor_invalidate(hot_rect((size_t)hover));
    }
}

void tray_click(int index, uint8_t button, int32_t x, int32_t y) {
    if (index < 0 || (size_t)index >= count)
        return;
    icon_t* ic = &icons[index];
    uint64_t now = dwm_time_ms();

    dwm_tray_click_t click;
    memset(&click, 0, sizeof(click));
    click.button = button;
    click.clicks = 1;
    click.x      = x;
    click.y      = y;
    click.rect   = ic->rect;
    if (last_click.owner == ic->owner && last_click.id == ic->id && last_click.button == button &&
        now - last_click.time <= DOUBLE_CLICK_MS) {
        click.clicks = 2;
        memset(&last_click, 0, sizeof(last_click));     // A third click starts over
    } else {
        last_click.owner  = ic->owner;
        last_click.id     = ic->id;
        last_click.button = button;
        last_click.time   = now;
    }

    // Clicking hides the tooltip; the program may answer with a menu,
    // unless the DWM opened the one it keeps for the icon already
    tray_hover(-1);
    ic->click_x = x;
    ic->click_y = y;
    bool opened = button == INPUT_BTN_RIGHT && open_kept_menu(ic);
    ic->menu_until = opened ? 0 : now + DWM_TRAY_MENU_MS;
    if (on_click != NULL)
        on_click(ic->owner, ic->id, &click);
}

void tray_paint(gfx_surface_t* s, gfx_rect_t a) {
    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
    for (size_t i = 0; i < count; i++) {
        const icon_t* ic = &icons[i];
        if (rect_empty(ic->rect) || !rect_overlaps(a, hot_rect(i)))
            continue;
        if ((int)i == hover) {
            gfx_rect_t h = hot_rect(i);
            gfx_fill_rect(s, h.x, h.y, h.w, h.h, 0x40FFFFFF);
        }
        gfx_draw_pixels(s, ic->pixels, DWM_TRAY_ICON_PX, DWM_TRAY_ICON_PX, 0, ic->rect.x, ic->rect.y);
    }
    gfx_set_blend(s, old);
}

void tray_paint_tip(gfx_surface_t* s, gfx_rect_t a) {
    gfx_rect_t r = tip_rect();
    if (rect_empty(r) || !rect_overlaps(a, r))
        return;
    const dwm_theme_t* t = dwm_theme();
    const char* tip = icons[hover].tip;
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, t->menu.background | 0xFF000000u);
    gfx_draw_rect(s, r.x, r.y, r.w, r.h, t->menu.border);
    dwm_text_draw(s, r.x + TIP_PAD_X, r.y + TIP_PAD_Y, tip, strlen(tip), t->menu.text);
}

void tray_shutdown(void) {
    memset(icons, 0, sizeof(icons));
    memset(&last_click, 0, sizeof(last_click));
    memset(&menu_for, 0, sizeof(menu_for));
    wipe(&panel, sizeof(panel));
    count = 0;
    hover = -1;
    places++;
    changed();
}

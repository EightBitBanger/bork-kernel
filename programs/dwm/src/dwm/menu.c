/*
 * Context menus (menu.h).
 *
 * One menu is open at a time. dwm_menu_show() copies what it will show
 * (after the state callbacks, labels included), submenus and all, into a
 * pool of entries split into tables (one per menu), so the caller's tables
 * may change or go away while it is open. The levels on the screen are a
 * stack: level 0 is the menu, each one after it the submenu of the
 * highlighted item in the one before. The compositor paints it above every
 * window (menu_paint), translucent as the theme says; input.c hands it the
 * mouse and the keys while it is open (menu_mouse, menu_key).
 */

#include <string.h>

#include "compositor.h"
#include "dirty.h"
#include "internal.h"
#include "theme.h"
#include "timer.h"

#define BORDER          1
#define PAD_TOP         2       // Inside the border, above the first item and below the last
#define ITEM_PAD_Y      3       // Above and below a label
#define CHECK_COLUMN    16      // Left of the labels, for the check mark
#define PAD_RIGHT       12
#define ARROW_GAP       12      // Between the widest label and a submenu's ">"
#define ARROW_PAD       5       // Right of the ">", to the border
#define SEPARATOR_H     7
#define MIN_WIDTH       96
#define DWM_MENU_TABLES 24      // Menus in one opening (the menu and its submenus)

#define ARROW_TEXT      ">"

typedef struct {
    char              label[DWM_MENU_LABEL_MAX];
    uint32_t          id;
    uint32_t          flags;
    dwm_menu_action_t action;
    int               sub;      // Table of its submenu, -1: none
    int32_t           y, h;     // Offset from the menu's top, height
} entry_t;

typedef struct {
    size_t   first, count;      // Entries in the pool
    int32_t  w, h;              // Size on the screen
    void*    user;              // The dwm_menu_t's
} table_t;

typedef struct {
    int        table;
    gfx_rect_t rect;            // Screen
    int        hot;             // Highlighted item (in the table), -1: none
} level_t;

static struct {
    bool               open;
    dwm_menu_context_t ctx;
    entry_t            pool[DWM_MENU_POOL];
    size_t             used;
    table_t            tables[DWM_MENU_TABLES];
    size_t             table_count;
    level_t            levels[DWM_MENU_DEPTH];
    int                depth;   // Levels showing (1 .. DWM_MENU_DEPTH)
    bool               pressed; // A button went down inside while open
} m;

static const dwm_menu_t* desktop_menu = NULL;
static const dwm_menu_t* window_menu  = NULL;

//
// Geometry and highlighting
//

static bool contains(gfx_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static entry_t* entry(int level, int i) {
    const table_t* t = &m.tables[m.levels[level].table];
    return &m.pool[t->first + (size_t)i];
}

static size_t level_count(int level) {
    return m.tables[m.levels[level].table].count;
}

static bool selectable(int level, int i) {
    return level >= 0 && level < m.depth && i >= 0 && (size_t)i < level_count(level) &&
           !(entry(level, i)->flags & (DWM_MENU_SEPARATOR | DWM_MENU_DISABLED));
}

static gfx_rect_t item_rect(int level, int i) {
    gfx_rect_t r = m.levels[level].rect;
    const entry_t* e = entry(level, i);
    return gfx_rect(r.x + BORDER, r.y + e->y, r.w - 2 * BORDER, e->h);
}

// The innermost level the point is on, -1: none
static int level_at(int32_t x, int32_t y) {
    for (int l = m.depth - 1; l >= 0; l--)
        if (contains(m.levels[l].rect, x, y))
            return l;
    return -1;
}

static int item_at(int level, int32_t x, int32_t y) {
    if (level < 0 || !contains(m.levels[level].rect, x, y))
        return -1;
    for (size_t i = 0; i < level_count(level); i++) {
        if (contains(item_rect(level, (int)i), x, y))
            return (int)i;
    }
    return -1;
}

static void set_hot(int level, int i) {
    level_t* lv = &m.levels[level];
    if (!selectable(level, i))
        i = -1;
    if (i == lv->hot)
        return;
    if (lv->hot >= 0)
        compositor_invalidate(item_rect(level, lv->hot));
    lv->hot = i;
    if (i >= 0)
        compositor_invalidate(item_rect(level, i));
}

// Shut every level after `level`
static void close_after(int level) {
    while (m.depth > level + 1) {
        m.depth--;
        compositor_invalidate(m.levels[m.depth].rect);
    }
}

// Open the submenu of item i of `level` beside it: to the right, or the
// left when there is no room; its first item level with the parent item
static bool open_sub(int level, int i) {
    if (!selectable(level, i))
        return false;
    int sub = entry(level, i)->sub;
    if (sub < 0)
        return false;
    if (m.depth > level + 1 && m.levels[level + 1].table == sub)
        return true;            // Already showing
    close_after(level);
    if (level + 1 >= DWM_MENU_DEPTH)
        return false;

    int32_t sw = compositor_width(), sh = compositor_height();
    const table_t* t = &m.tables[sub];
    gfx_rect_t parent = m.levels[level].rect;
    gfx_rect_t ir = item_rect(level, i);
    int32_t w = t->w, h = t->h;
    int32_t x = parent.x + parent.w - BORDER;
    if (x + w > sw)
        x = (parent.x - w + BORDER >= 0) ? parent.x - w + BORDER : sw - w;
    int32_t y = ir.y - BORDER - PAD_TOP;
    if (y + h > sh) y = sh - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    level_t* lv = &m.levels[level + 1];
    lv->table = sub;
    lv->rect  = gfx_rect(x, y, w, h);
    lv->hot   = -1;
    m.depth   = level + 2;
    compositor_invalidate(lv->rect);
    return true;
}

// Next selectable item of `level` in direction dir (+1 / -1), wrapping around
static void move_hot(int level, int dir) {
    int n = (int)level_count(level);
    int i = m.levels[level].hot;
    for (int step = 0; step < n; step++) {
        i = (i < 0) ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
        if (selectable(level, i)) {
            set_hot(level, i);
            return;
        }
    }
}

// Close first, then act: the action may open another menu. A submenu's
// item opens its submenu instead.
static void activate(int level, int i) {
    if (!selectable(level, i))
        return;
    entry_t e = *entry(level, i);
    if (e.sub >= 0) {
        set_hot(level, i);
        if (open_sub(level, i)) {
            m.levels[level + 1].hot = -1;
            move_hot(level + 1, +1);
        }
        return;
    }
    dwm_menu_context_t ctx = m.ctx;
    ctx.id   = e.id;
    ctx.user = m.tables[m.levels[level].table].user;
    dwm_menu_close();

    if (ctx.window != DWM_DESKTOP && !dwm_window_exists(ctx.window))
        return;
    if (e.action != NULL) {
        e.action(&ctx);
        return;
    }

    dwm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type     = DWM_EVENT_MENU_COMMAND;
    ev.x        = ctx.x;
    ev.y        = ctx.y;
    ev.param[0] = e.id;
    ev.param[1] = ctx.icon;
    ev.time_ms  = (uint32_t)dwm_time_ms();
    dwm_post(ctx.window, &ev);
}

//
// Building: the menu and its submenus copied into the pool
//

// One table for `menu` (its submenus first, depth levels down at most), -1
// when nothing would show or there is no room
static int build_table(const dwm_menu_t* menu, dwm_menu_context_t* ctx, int depth) {
    if (menu == NULL || menu->items == NULL || menu->count == 0 || depth >= DWM_MENU_DEPTH ||
        m.table_count == DWM_MENU_TABLES)
        return -1;

    // What shows: hidden items out, no separator first, last or doubled.
    // Built on the stack: submenus take their entries from the pool first.
    entry_t built[DWM_MENU_MAX_ITEMS];
    size_t n = 0;
    void* saved_user = ctx->user;
    for (size_t i = 0; i < menu->count && n < DWM_MENU_MAX_ITEMS; i++) {
        const dwm_menu_item_t* it = &menu->items[i];
        uint32_t flags = it->flags;
        if (it->state != NULL) {
            ctx->id = it->id;
            ctx->user = menu->user;
            flags |= it->state(ctx);
        }
        if (flags & DWM_MENU_HIDDEN)
            continue;
        if ((flags & DWM_MENU_SEPARATOR) && (n == 0 || (built[n - 1].flags & DWM_MENU_SEPARATOR)))
            continue;

        entry_t* e = &built[n++];
        memset(e, 0, sizeof(*e));
        if (!(flags & DWM_MENU_SEPARATOR) && it->label != NULL)
            strlcpy(e->label, it->label, sizeof(e->label));
        e->id     = it->id;
        e->flags  = flags;
        e->action = it->action;
        e->sub    = -1;
        if (it->submenu != NULL && !(flags & DWM_MENU_SEPARATOR)) {
            e->sub = build_table(it->submenu, ctx, depth + 1);
            if (e->sub < 0)
                e->flags |= DWM_MENU_DISABLED;      // Nothing in it
        }
    }
    ctx->user = saved_user;
    while (n > 0 && (built[n - 1].flags & DWM_MENU_SEPARATOR))
        n--;
    if (n == 0 || m.used + n > DWM_MENU_POOL || m.table_count == DWM_MENU_TABLES)
        return -1;

    // Layout
    int32_t lh = dwm_text_height();
    int32_t y_at = BORDER + PAD_TOP;
    int32_t widest = 0;
    bool arrows = false;
    for (size_t i = 0; i < n; i++) {
        built[i].y = y_at;
        built[i].h = (built[i].flags & DWM_MENU_SEPARATOR) ? SEPARATOR_H : lh + 2 * ITEM_PAD_Y;
        y_at += built[i].h;
        int32_t w = dwm_text_width(built[i].label, strlen(built[i].label));
        if (w > widest)
            widest = w;
        if (built[i].sub >= 0)
            arrows = true;
    }
    int32_t mw = 2 * BORDER + CHECK_COLUMN + widest +
                 (arrows ? ARROW_GAP + dwm_text_width(ARROW_TEXT, 1) + ARROW_PAD : PAD_RIGHT);
    int32_t mh = y_at + PAD_TOP + BORDER;
    int32_t sw = compositor_width(), sh = compositor_height();
    if (mw < MIN_WIDTH) mw = MIN_WIDTH;
    if (mw > sw) mw = sw;
    if (mh > sh) mh = sh;

    table_t* t = &m.tables[m.table_count];
    t->first = m.used;
    t->count = n;
    t->w     = mw;
    t->h     = mh;
    t->user  = menu->user;
    memcpy(&m.pool[m.used], built, n * sizeof(entry_t));
    m.used += n;
    return (int)m.table_count++;
}

//
// Public
//

bool dwm_menu_show(dwm_handle_t window, const dwm_menu_t* menu, int32_t x, int32_t y) {
    if (menu == NULL || menu->items == NULL || menu->count == 0)
        return false;

    int32_t sw = compositor_width(), sh = compositor_height();
    gfx_surface_t* s = compositor_screen();
    if (s == NULL || sw <= 0 || sh <= 0)
        return false;

    // Opening point on the screen
    int32_t sx = x, sy = y;
    if (window != DWM_DESKTOP) {
        dwm_window_t* w = window_get(window);
        if (w == NULL || !w->visible)
            return false;
        gfx_rect_t c = window_client(w);
        sx += c.x;
        sy += c.y;
    }

    dwm_menu_close();
    input_cancel();         // A drag or capture in progress ends here

    dwm_menu_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.window = window;
    ctx.icon   = (window == DWM_DESKTOP) ? dwm_icon_at(sx, sy) : 0;
    ctx.x      = x;
    ctx.y      = y;
    ctx.user   = menu->user;

    m.used = 0;
    m.table_count = 0;
    int top = build_table(menu, &ctx, 0);
    ctx.id = 0;
    ctx.user = menu->user;
    if (top < 0)
        return false;

    // On the screen: flip left / up at the edges, else push it in
    int32_t mw = m.tables[top].w, mh = m.tables[top].h;
    if (sx + mw > sw) sx = (sx - mw >= 0) ? sx - mw : sw - mw;
    if (sy + mh > sh) sy = (sy - mh >= 0) ? sy - mh : sh - mh;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;

    m.ctx     = ctx;
    m.levels[0].table = top;
    m.levels[0].rect  = gfx_rect(sx, sy, mw, mh);
    m.levels[0].hot   = -1;
    m.depth   = 1;
    m.pressed = false;
    m.open    = true;
    compositor_invalidate(m.levels[0].rect);
    return true;
}

void dwm_menu_close(void) {
    if (!m.open)
        return;
    for (int l = 0; l < m.depth; l++)
        compositor_invalidate(m.levels[l].rect);
    m.open    = false;
    m.depth   = 0;
    m.used    = 0;
    m.table_count = 0;
    m.pressed = false;
}

bool dwm_menu_is_open(void) {
    return m.open;
}

void dwm_set_desktop_menu(const dwm_menu_t* menu) { desktop_menu = menu; }
void dwm_set_window_menu(const dwm_menu_t* menu)  { window_menu = menu; }
const dwm_menu_t* dwm_get_desktop_menu(void)      { return desktop_menu; }
const dwm_menu_t* dwm_get_window_menu(void)       { return window_menu; }

//
// For the rest of the DWM
//

// The pointer rests on item i of `level`: it is highlighted, its submenu
// opens, and any other submenu of that level shuts
static void hover(int level, int i) {
    if (!selectable(level, i))
        return;                 // A separator, a dimmed item, the border: all stays as it is
    set_hot(level, i);
    if (entry(level, i)->sub >= 0)
        open_sub(level, i);
    else
        close_after(level);
}

void menu_mouse(int32_t x, int32_t y, bool moved, uint8_t pressed, uint8_t released, uint8_t held) {
    if (!m.open)
        return;
    int level = level_at(x, y);
    bool inside = level >= 0;

    if (moved && inside) {
        hover(level, item_at(level, x, y));
    }

    if (pressed) {
        if (!inside) {
            dwm_menu_close();
            return;
        }
        m.pressed = true;
    }

    // Only a click made inside chooses: the button that opened the menu
    // (dropped down from a press) does nothing when it comes back up
    if (released && m.pressed && inside) {
        int i = item_at(level, x, y);
        if (selectable(level, i)) {
            if (entry(level, i)->sub >= 0)
                hover(level, i);        // Opens it; nothing chosen
            else {
                activate(level, i);
                return;
            }
        }
    }
    if (held == 0)
        m.pressed = false;
}

void menu_key(uint16_t key) {
    int l = m.depth - 1;
    if (l < 0)
        return;
    switch (key) {
        case KEY_ESCAPE:
            if (l > 0)
                close_after(l - 1);     // Back to the parent, its item still highlighted
            else
                dwm_menu_close();
            break;
        case KEY_LEFT:
            if (l > 0)
                close_after(l - 1);
            break;
        case KEY_RIGHT: {
            int i = m.levels[l].hot;
            if (selectable(l, i) && entry(l, i)->sub >= 0)
                activate(l, i);         // Opens it, first item highlighted
            break;
        }
        case KEY_UP:                        move_hot(l, -1);    break;
        case KEY_DOWN: case KEY_TAB:        move_hot(l, +1);    break;
        case KEY_HOME: set_hot(l, -1);      move_hot(l, +1);    break;
        case KEY_END:  set_hot(l, -1);      move_hot(l, -1);    break;
        case KEY_ENTER: case ' ':           activate(l, m.levels[l].hot);   break;
        default:                                                break;
    }
}

bool menu_default(const dwm_event_t* ev) {
    const dwm_menu_t* menu = (ev->window == DWM_DESKTOP) ? desktop_menu : window_menu;
    return menu != NULL && dwm_menu_show(ev->window, menu, ev->x, ev->y);
}

void menu_window_gone(dwm_handle_t window) {
    if (m.open && m.ctx.window == window)
        dwm_menu_close();
}

void menu_close_for_user(const void* user) {
    if (m.open && m.ctx.user == user)
        dwm_menu_close();
}

//
// Painting
//

static uint32_t dimmed(uint32_t color) {
    return (((color >> 24) / 2) << 24) | (color & 0x00FFFFFFu);
}

static void draw_check(gfx_surface_t* s, int32_t x, int32_t y, int32_t h, uint32_t color) {
    int32_t mid = y + h / 2;
    gfx_draw_line(s, x, mid, x + 2, mid + 2, color);
    gfx_draw_line(s, x + 3, mid + 1, x + 6, mid - 2, color);
}

static void paint_level(gfx_surface_t* s, gfx_rect_t area, int level) {
    const dwm_theme_menu_t* t = &dwm_theme()->menu;
    gfx_rect_t r = m.levels[level].rect;
    if (!rect_overlaps(area, r))
        return;

    gfx_fill_rect(s, r.x + BORDER, r.y + BORDER, r.w - 2 * BORDER, r.h - 2 * BORDER, t->background);
    gfx_draw_rect(s, r.x, r.y, r.w, r.h, t->border);

    int32_t lh = dwm_text_height();
    int32_t aw = dwm_text_width(ARROW_TEXT, 1);
    for (size_t i = 0; i < level_count(level); i++) {
        const entry_t* e = entry(level, (int)i);
        gfx_rect_t ir = item_rect(level, (int)i);
        if (!rect_overlaps(area, ir))
            continue;

        if (e->flags & DWM_MENU_SEPARATOR) {
            gfx_draw_hline(s, ir.x + 4, ir.y + ir.h / 2, ir.w - 8, t->separator);
            continue;
        }
        if ((int)i == m.levels[level].hot)
            gfx_fill_rect(s, ir.x, ir.y, ir.w, ir.h, t->highlight);

        uint32_t color = (e->flags & DWM_MENU_DISABLED) ? dimmed(t->text) : t->text;
        int32_t ty = ir.y + (ir.h - lh) / 2;
        if (e->flags & DWM_MENU_CHECKED)
            draw_check(s, ir.x + 5, ty, lh, color);
        dwm_text_draw(s, ir.x + CHECK_COLUMN, ty, e->label, strlen(e->label), color);
        if (e->sub >= 0)    // At the right edge
            dwm_text_draw(s, ir.x + ir.w - ARROW_PAD - aw, ty, ARROW_TEXT, 1, color);
    }
}

// Every level, the menu first: each submenu lies over the one it came from
void menu_paint(gfx_surface_t* s, gfx_rect_t area) {
    if (!m.open)
        return;
    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
    for (int l = 0; l < m.depth; l++)
        paint_level(s, area, l);
    gfx_set_blend(s, old);
}

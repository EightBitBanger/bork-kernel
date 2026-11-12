/*
 * Window table, stacking order, geometry and focus.
 *
 * Windows live in a table of slots that grows (doubling) as more are open,
 * up to DWM_MAX_WINDOWS. Each slot's window is allocated once and stays put,
 * so window pointers survive the table growing. A handle is
 * (generation << SLOT_BITS) | (slot + 1): the generation goes up each time a
 * slot is freed, so a stale handle never names the window that reused it.
 *
 * The stacking order is a doubly linked list, bottom to top. TOPMOST windows
 * form the upper part of it: raising a normal window puts it just under the
 * lowest TOPMOST one.
 */

#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "compositor.h"
#include "../resources/imagelib.h"

#define MIN_WIDTH_DEFAULT   64
#define MIN_HEIGHT_DEFAULT  32

#define CASCADE_START_X     150     // Clear of the first column of desktop icons
#define CASCADE_START_Y     120
#define CASCADE_STEP        24
#define CASCADE_STEPS       10

#define CLOSE_IMAGE         "ui_close"
#define CLOSE_SIZE          3       // Drawn close box when the image is missing
#define MINIMIZE_IMAGE      "ui_minimize"
#define MINIMIZE_SIZE       12      // Drawn minimize box when the image is missing
#define MINIMIZE_GAP        2       // Between the minimize and close boxes

#define CLOSE_MARGIN        4       // Gap to the titlebar's right edge
#define TITLE_TEXT_PAD      3       // Above and below the title text, at least
#define CLOSE_NUDGE_X       3
#define CLOSE_NUDGE_Y      -1

#define SLOT_BITS           12
#define SLOT_MASK           ((1u << SLOT_BITS) - 1)
#define GENERATION_MAX      (0xFFFFFFFFu >> SLOT_BITS)
#define SLOTS_INITIAL       32

_Static_assert(DWM_MAX_WINDOWS <= SLOT_MASK, "window slots must fit in a handle");

static dwm_window_t** windows;      // `slots` entries, each allocated on first use
static uint32_t*      generation;
static size_t         slots;
static size_t         window_count = 0;

static dwm_window_t* bottom = NULL;
static dwm_window_t* top    = NULL;

static dwm_handle_t focus   = DWM_DESKTOP;
static unsigned     cascade = 0;

static dwm_proc_t   desktop_proc = NULL;
static void*        desktop_user = NULL;

//
// Lookup
//

dwm_window_t* window_get(dwm_handle_t handle) {
    uint32_t slot = (handle & SLOT_MASK);
    if (slot == 0 || slot > slots)
        return NULL;
    dwm_window_t* w = windows[slot - 1];
    return (w != NULL && w->handle == handle) ? w : NULL;
}

dwm_window_t* window_bottom(void) {
    return bottom;
}

bool dwm_window_exists(dwm_handle_t window) {
    return window_get(window) != NULL;
}

size_t dwm_window_count(void) {
    return window_count;
}

void* dwm_window_user(dwm_handle_t window) {
    dwm_window_t* w = window_get(window);
    return w ? w->user : NULL;
}

uint32_t dwm_window_style(dwm_handle_t window) {
    dwm_window_t* w = window_get(window);
    return w ? w->style : 0;
}

//
// Geometry
//

static bool rect_contains(gfx_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

int32_t window_border(const dwm_window_t* w) {
    return (w->style & DWM_STYLE_NOBORDER) ? 0 : DWM_BORDER_WIDTH;
}

int32_t window_title_height(const dwm_window_t* w) {
    if (w->style & (DWM_STYLE_NOBORDER | DWM_STYLE_NOTITLEBAR))
        return 0;
    // Taller for a large UI font
    int32_t h = dwm_text_height() + 2 * TITLE_TEXT_PAD;
    return h > DWM_TITLEBAR_HEIGHT ? h : DWM_TITLEBAR_HEIGHT;
}

gfx_rect_t window_frame(const dwm_window_t* w) {
    int32_t b = window_border(w);
    return gfx_rect(w->x, w->y, w->width + 2 * b, w->height + window_title_height(w) + 2 * b);
}

gfx_rect_t window_client(const dwm_window_t* w) {
    int32_t b = window_border(w);
    return gfx_rect(w->x + b, w->y + b + window_title_height(w), w->width, w->height);
}

gfx_rect_t window_titlebar(const dwm_window_t* w) {
    int32_t t = window_title_height(w);
    if (t == 0)
        return gfx_rect(0, 0, 0, 0);
    int32_t b = window_border(w);
    return gfx_rect(w->x + b, w->y + b, w->width, t);
}

gfx_rect_t window_close_box(const dwm_window_t* w) {
    gfx_rect_t bar = window_titlebar(w);
    if (bar.h == 0 || (w->style & DWM_STYLE_NOCLOSEBOX))
        return gfx_rect(0, 0, 0, 0);

    const image_t* image = imagelib_get(CLOSE_IMAGE);
    int32_t cw = image ? image->width  : CLOSE_SIZE;
    int32_t ch = image ? image->height : CLOSE_SIZE;

    return gfx_rect(bar.x + bar.w - cw - CLOSE_MARGIN + CLOSE_NUDGE_X,
                bar.y + (bar.h - ch) / 2 + CLOSE_NUDGE_Y, cw, ch);
}

bool window_can_minimize(const dwm_window_t* w) {
    return window_title_height(w) > 0 && !(w->style & DWM_STYLE_NOMINIMIZE);
}

// Left of the close box, or where it would be without one
gfx_rect_t window_minimize_box(const dwm_window_t* w) {
    if (!window_can_minimize(w))
        return gfx_rect(0, 0, 0, 0);
    gfx_rect_t bar = window_titlebar(w);

    const image_t* image = imagelib_get(MINIMIZE_IMAGE);
    int32_t mw = image ? image->width  : MINIMIZE_SIZE;
    int32_t mh = image ? image->height : MINIMIZE_SIZE;

    gfx_rect_t close = window_close_box(w);
    int32_t right = (close.w > 0) ? close.x - MINIMIZE_GAP : bar.x + bar.w - CLOSE_MARGIN + CLOSE_NUDGE_X;
    return gfx_rect(right - mw, bar.y + (bar.h - mh) / 2 + CLOSE_NUDGE_Y, mw, mh);
}

gfx_rect_t window_grip(const dwm_window_t* w) {
    if (!(w->style & DWM_STYLE_RESIZABLE))
        return gfx_rect(0, 0, 0, 0);
    gfx_rect_t c = window_client(w);
    return gfx_rect(c.x + c.w - DWM_GRIP_SIZE, c.y + c.h - DWM_GRIP_SIZE, DWM_GRIP_SIZE, DWM_GRIP_SIZE);
}

bool window_focused(const dwm_window_t* w) {
    return w != NULL && w->handle == focus;
}

dwm_window_t* window_top_visible_at(int32_t x, int32_t y) {
    for (dwm_window_t* w = top; w != NULL; w = w->below) {
        if (w->visible && rect_contains(window_frame(w), x, y))
            return w;
    }
    return NULL;
}

dwm_handle_t dwm_window_at(int32_t x, int32_t y) {
    dwm_window_t* w = window_top_visible_at(x, y);
    return w ? w->handle : DWM_DESKTOP;
}

dwm_hit_t window_hit(const dwm_window_t* w, int32_t x, int32_t y) {
    if (w == NULL || !rect_contains(window_frame(w), x, y))
        return HIT_NONE;
    if (rect_contains(window_grip(w), x, y))      return HIT_GRIP;
    if (rect_contains(window_close_box(w), x, y)) return HIT_CLOSE;
    if (rect_contains(window_minimize_box(w), x, y)) return HIT_MINIMIZE;
    if (rect_contains(window_titlebar(w), x, y))  return HIT_TITLEBAR;
    if (rect_contains(window_client(w), x, y))    return HIT_CLIENT;
    return HIT_BORDER;
}

//
// Procedures
//

void dwm_set_desktop_proc(dwm_proc_t proc, void* user) {
    desktop_proc = proc;
    desktop_user = user;
}

bool window_call(dwm_handle_t handle, const dwm_event_t* ev) {
    dwm_event_t e = *ev;
    e.window = handle;

    if (handle == DWM_DESKTOP)
        return desktop_proc ? desktop_proc(DWM_DESKTOP, &e, desktop_user) : false;

    dwm_window_t* w = window_get(handle);
    if (w == NULL || w->proc == NULL)
        return false;
    return w->proc(handle, &e, w->user);
}

static void post_type(dwm_handle_t handle, uint16_t type) {
    dwm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    dwm_post(handle, &ev);
}

//
// Stacking
//

static void unlink_window(dwm_window_t* w) {
    if (w->below) w->below->above = w->above;
    else          bottom = w->above;
    if (w->above) w->above->below = w->below;
    else          top = w->below;
    w->below = w->above = NULL;
}

// Put `w` just below `ref` (NULL: on top)
static void link_below(dwm_window_t* w, dwm_window_t* ref) {
    if (ref == NULL) {
        w->below = top;
        w->above = NULL;
        if (top) top->above = w;
        else     bottom = w;
        top = w;
        return;
    }
    w->above = ref;
    w->below = ref->below;
    if (ref->below) ref->below->above = w;
    else            bottom = w;
    ref->below = w;
}

// Where `w` goes when raised: the top, or just below the lowest TOPMOST window
static dwm_window_t* raise_position(const dwm_window_t* w) {
    if (w->style & DWM_STYLE_TOPMOST)
        return NULL;
    dwm_window_t* lowest_topmost = NULL;
    for (dwm_window_t* v = top; v != NULL && (v->style & DWM_STYLE_TOPMOST); v = v->below) {
        if (v != w)
            lowest_topmost = v;
    }
    return lowest_topmost;
}

void dwm_window_raise(dwm_handle_t window) {
    dwm_window_t* w = window_get(window);
    if (w == NULL)
        return;

    dwm_window_t* ref = raise_position(w);
    if (w->above == ref)
        return;         // Already there

    unlink_window(w);
    link_below(w, ref);
    if (w->visible)
        compositor_invalidate(window_frame(w));
}

//
// Focus
//

static void invalidate_titlebar(dwm_window_t* w) {
    if (w != NULL && w->visible)
        compositor_invalidate(window_titlebar(w));
}

void dwm_window_focus(dwm_handle_t window) {
    dwm_window_t* w = window_get(window);
    if (w != NULL && !w->visible)
        return;
    dwm_handle_t next = w ? w->handle : DWM_DESKTOP;
    if (next == focus)
        return;

    dwm_window_t* old = window_get(focus);
    focus = next;

    if (old != NULL) {
        invalidate_titlebar(old);
        post_type(old->handle, DWM_EVENT_FOCUS_LOST);
    }
    if (w != NULL) {
        invalidate_titlebar(w);
        post_type(w->handle, DWM_EVENT_FOCUS_GAINED);
    }
}

dwm_handle_t dwm_window_get_focus(void) {
    return focus;
}

// After the focused window goes away or hides: the topmost visible window
// that accepts focus, or the desktop
static void focus_next(const dwm_window_t* leaving) {
    for (dwm_window_t* w = top; w != NULL; w = w->below) {
        if (w != leaving && w->visible && !(w->style & DWM_STYLE_NOFOCUS)) {
            dwm_window_focus(w->handle);
            return;
        }
    }
    dwm_window_focus(DWM_DESKTOP);
}

//
// Create and destroy
//

// More slots: double the table, up to DWM_MAX_WINDOWS. False when full or
// out of memory.
static bool grow_slots(void) {
    if (slots >= DWM_MAX_WINDOWS)
        return false;
    size_t grow = slots ? slots * 2 : SLOTS_INITIAL;
    if (grow > DWM_MAX_WINDOWS)
        grow = DWM_MAX_WINDOWS;
    dwm_window_t** w = (dwm_window_t**)realloc(windows, grow * sizeof(*w));
    if (w == NULL)
        return false;
    windows = w;
    uint32_t* g = (uint32_t*)realloc(generation, grow * sizeof(*g));
    if (g == NULL)
        return false;               // `windows` is just roomier; slots unchanged
    generation = g;
    memset(&windows[slots], 0, (grow - slots) * sizeof(*w));
    memset(&generation[slots], 0, (grow - slots) * sizeof(*g));
    slots = grow;
    return true;
}

dwm_handle_t dwm_window_create(const dwm_window_desc_t* desc) {
    if (desc == NULL || desc->proc == NULL || desc->width <= 0 || desc->height <= 0)
        return 0;
    if (desc->width > GFX_SIZE_MAX || desc->height > GFX_SIZE_MAX)
        return 0;

    size_t slot = 0;
    while (slot < slots && windows[slot] != NULL && windows[slot]->handle != 0)
        slot++;
    if (slot == slots && !grow_slots())
        return 0;
    if (windows[slot] == NULL) {
        windows[slot] = (dwm_window_t*)malloc(sizeof(dwm_window_t));
        if (windows[slot] == NULL)
            return 0;
        memset(windows[slot], 0, sizeof(dwm_window_t));
    }

    if (generation[slot] == 0 || generation[slot] > GENERATION_MAX)
        generation[slot] = 1;

    dwm_window_t* w = windows[slot];
    memset(w, 0, sizeof(*w));
    w->handle     = (generation[slot] << SLOT_BITS) | (uint32_t)(slot + 1);
    w->style      = desc->style;
    w->width      = desc->width;
    w->height     = desc->height;
    w->min_width  = desc->min_width  > 0 ? desc->min_width  : MIN_WIDTH_DEFAULT;
    w->min_height = desc->min_height > 0 ? desc->min_height : MIN_HEIGHT_DEFAULT;
    w->proc       = desc->proc;
    w->user       = desc->user;
    if (desc->title != NULL)
        strncpy(w->title, desc->title, sizeof(w->title) - 1);

    if (desc->x == DWM_POS_DEFAULT || desc->y == DWM_POS_DEFAULT) {
        int32_t offset = (int32_t)(cascade++ % CASCADE_STEPS) * CASCADE_STEP;
        w->x = (desc->x == DWM_POS_DEFAULT) ? CASCADE_START_X + offset : desc->x;
        w->y = (desc->y == DWM_POS_DEFAULT) ? CASCADE_START_Y + offset : desc->y;
    } else {
        w->x = desc->x;
        w->y = desc->y;
    }

    // In the stack (hidden for now), so CREATE can already use the handle
    link_below(w, raise_position(w));
    window_count++;

    dwm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DWM_EVENT_CREATE;
    window_call(w->handle, &ev);

    // CREATE may have destroyed it
    dwm_handle_t handle = w->handle;
    if (window_get(handle) == NULL)
        return 0;

    if (!(w->style & DWM_STYLE_HIDDEN))
        dwm_window_show(handle, true);
    return handle;
}

static void destroy_now(dwm_window_t* w) {
    // A procedure calling dwm_window_destroy from its own DESTROY
    if (w->destroying)
        return;
    w->destroying = true;
    dwm_handle_t handle = w->handle;

    dwm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DWM_EVENT_DESTROY;
    window_call(handle, &ev);

    if (w->visible)
        compositor_invalidate(window_frame(w));

    unlink_window(w);
    input_window_gone(handle);
    menu_window_gone(handle);
    timer_window_gone(handle);

    size_t slot = (size_t)((handle & SLOT_MASK) - 1);
    generation[slot]++;
    memset(w, 0, sizeof(*w));
    window_count--;

    if (focus == handle) {
        focus = DWM_DESKTOP;        // Nothing to tell: it is gone
        focus_next(NULL);
    }
}

void dwm_window_destroy(dwm_handle_t window) {
    dwm_window_t* w = window_get(window);
    if (w == NULL)
        return;
    if (compositor_painting()) {
        w->doomed = true;
        return;
    }
    destroy_now(w);
}

void window_reap(void) {
    for (size_t i = 0; i < slots; i++) {
        if (windows[i] != NULL && windows[i]->handle != 0 && windows[i]->doomed)
            destroy_now(windows[i]);
    }
}

void window_destroy_all(void) {
    while (top != NULL)
        destroy_now(top);
    for (size_t i = 0; i < slots; i++)
        free(windows[i]);
    free(windows);
    free(generation);
    windows    = NULL;
    generation = NULL;
    slots      = 0;
}

void dwm_window_close(dwm_handle_t window) {
    if (window_get(window) != NULL)
        post_type(window, DWM_EVENT_CLOSE);
}

//
// Properties
//

void dwm_window_set_title(dwm_handle_t window, const char* title) {
    dwm_window_t* w = window_get(window);
    if (w == NULL)
        return;
    memset(w->title, 0, sizeof(w->title));
    if (title != NULL)
        strncpy(w->title, title, sizeof(w->title) - 1);
    invalidate_titlebar(w);
}

const char* dwm_window_get_title(dwm_handle_t window) {
    dwm_window_t* w = window_get(window);
    return w ? w->title : "";
}

void dwm_window_move(dwm_handle_t window, int32_t x, int32_t y) {
    dwm_window_t* w = window_get(window);
    if (w == NULL || (w->x == x && w->y == y))
        return;

    // Keep it reachable: frames stay within the coordinate range gfx takes
    if (x < -GFX_COORD_MAX / 2) x = -GFX_COORD_MAX / 2;
    if (y < -GFX_COORD_MAX / 2) y = -GFX_COORD_MAX / 2;
    if (x >  GFX_COORD_MAX / 2) x =  GFX_COORD_MAX / 2;
    if (y >  GFX_COORD_MAX / 2) y =  GFX_COORD_MAX / 2;

    if (w->visible)
        compositor_invalidate(window_frame(w));
    w->x = x;
    w->y = y;
    if (w->visible)
        compositor_invalidate(window_frame(w));

    dwm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DWM_EVENT_MOVED;
    ev.rect = window_frame(w);
    dwm_post(window, &ev);
}

void dwm_window_resize(dwm_handle_t window, int32_t width, int32_t height) {
    dwm_window_t* w = window_get(window);
    if (w == NULL)
        return;
    if (width  < w->min_width)  width  = w->min_width;
    if (height < w->min_height) height = w->min_height;
    if (width  > GFX_SIZE_MAX / 2) width  = GFX_SIZE_MAX / 2;
    if (height > GFX_SIZE_MAX / 2) height = GFX_SIZE_MAX / 2;
    if (w->width == width && w->height == height)
        return;

    if (w->visible)
        compositor_invalidate(window_frame(w));
    w->width  = width;
    w->height = height;
    if (w->visible)
        compositor_invalidate(window_frame(w));

    dwm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DWM_EVENT_RESIZED;
    ev.rect = gfx_rect(0, 0, width, height);
    dwm_post(window, &ev);
}

bool dwm_window_get_rect(dwm_handle_t window, gfx_rect_t* frame, gfx_rect_t* client) {
    dwm_window_t* w = window_get(window);
    if (w == NULL)
        return false;
    if (frame)  *frame  = window_frame(w);
    if (client) *client = window_client(w);
    return true;
}

void dwm_window_get_size(dwm_handle_t window, int32_t* width, int32_t* height) {
    dwm_window_t* w = window_get(window);
    if (width)  *width  = w ? w->width  : 0;
    if (height) *height = w ? w->height : 0;
}

void dwm_window_show(dwm_handle_t window, bool visible) {
    dwm_window_t* w = window_get(window);
    if (w == NULL)
        return;
    if (visible)
        w->minimized = false;       // Shown again, however it was hidden
    if (w->visible == visible)
        return;

    w->visible = visible;
    compositor_invalidate(window_frame(w));
    post_type(window, visible ? DWM_EVENT_SHOW : DWM_EVENT_HIDE);

    if (visible) {
        if (!(w->style & DWM_STYLE_NOFOCUS)) {
            dwm_window_raise(window);
            dwm_window_focus(window);
        }
    } else {
        input_window_gone(window);
        menu_window_gone(window);
        if (focus == window)
            focus_next(w);
    }
}

bool dwm_window_visible(dwm_handle_t window) {
    dwm_window_t* w = window_get(window);
    return w != NULL && w->visible;
}

void dwm_window_invalidate(dwm_handle_t window, const gfx_rect_t* rect) {
    dwm_window_t* w = window_get(window);
    if (w == NULL || !w->visible)
        return;
    gfx_rect_t client = window_client(w);
    if (rect == NULL) {
        compositor_invalidate(client);
        return;
    }
    // Client coordinates -> screen, cut to the client area
    gfx_rect_t r = gfx_rect(client.x + rect->x, client.y + rect->y, rect->w, rect->h);
    int32_t x0 = r.x > client.x ? r.x : client.x;
    int32_t y0 = r.y > client.y ? r.y : client.y;
    int32_t x1 = (r.x + r.w < client.x + client.w) ? r.x + r.w : client.x + client.w;
    int32_t y1 = (r.y + r.h < client.y + client.h) ? r.y + r.h : client.y + client.h;
    if (x1 > x0 && y1 > y0)
        compositor_invalidate(gfx_rect(x0, y0, x1 - x0, y1 - y0));
}

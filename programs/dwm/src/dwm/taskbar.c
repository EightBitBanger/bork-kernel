/*
 * Taskbar (taskbar.h): the strip, the Start button, a button per window,
 * the clock, and minimizing. The start menu itself is startmenu.c.
 *
 * The buttons follow the windows by looking, not by being told: once per
 * round taskbar_tick() walks the window list, keeps the buttons of windows
 * still there (in the order they first showed up), adds new ones at the
 * end, and repaints the strip only when what it shows changed (a title,
 * the focus, a window coming, going or being minimized).
 *
 * input.c gives every press on the strip to taskbar_press() and its
 * release to taskbar_release(): no window or icon ever sees them. A button
 * acts when the press and the release are on it.
 *
 * The tray at the right end holds the programs' tray icons (tray.c) and
 * the clock; it grows to the left as icons come. The clock is the wall
 * clock (dwm_clock_now, the kernel's RTC driver) as "3:07 PM", with the
 * date in a tooltip; on a machine without a clock chip it falls back to
 * the uptime, "up 1:23". It is repainted when the minute changes
 * (taskbar_next_timeout wakes the desktop for that).
 */

#include <stdio.h>          // snprintf
#include <stdlib.h>
#include <string.h>
#include <os/input.h>

#include "compositor.h"
#include "dirty.h"
#include "internal.h"
#include "taskbar.h"
#include "theme.h"
#include "timer.h"

#define BAR_PAD_Y           4       // Above and below the buttons
#define BAR_PAD_X           4       // Left of Start, right of the clock
#define BUTTON_GAP          4
#define BUTTON_PAD_X        8       // Text inside a button
#define START_IMAGE         "ui_start"  // The Start button's picture (<home>/sys/images)
#define WINDOW_BUTTON_MAX   180     // Widest window button
#define WINDOW_BUTTON_MIN   28
#define MAX_BUTTONS         64      // Windows with a button (more: left out)
#define TRAY_PAD_X          10
#define CLOCK_TIP_PAD_X     6       // As the tray icons' tooltips (tray.c)
#define CLOCK_TIP_PAD_Y     3
#define CLOCK_TIP_LIFT      4

#define SMALL_CACHE         24      // Shrunk images kept (small_image_draw)

typedef struct {
    dwm_handle_t window;
    gfx_rect_t   rect;              // Screen
} button_t;

static button_t buttons[MAX_BUTTONS];
static size_t   button_count;
static uint32_t shown_hash;         // What the strip last showed (taskbar_tick)
static uint64_t shown_minute = UINT64_MAX;
static bool     clock_hover;        // The pointer is on the clock: its date tooltip shows

// Item numbers for press / hover: START, WINDOW + index, TRAY + index
#define ITEM_NONE           -1
#define ITEM_START          0
#define ITEM_WINDOW         1
#define ITEM_TRAY           (ITEM_WINDOW + MAX_BUTTONS)

static int      pressed_item = ITEM_NONE;
static uint8_t  pressed_button;
static uint32_t pressed_tray;       // tray_places_serial() when a tray icon was pressed
static int      hover_item = ITEM_NONE;

//
// Geometry
//

int32_t dwm_taskbar_height(void) {
    int32_t h = dwm_text_height() + 2 * BAR_PAD_Y + 8;
    return h > DWM_TASKBAR_MIN_HEIGHT ? h : DWM_TASKBAR_MIN_HEIGHT;
}

gfx_rect_t taskbar_rect(void) {
    int32_t h = dwm_taskbar_height();
    return gfx_rect(0, compositor_height() - h, compositor_width(), h);
}

gfx_rect_t dwm_work_area(void) {
    int32_t h = compositor_height() - dwm_taskbar_height();
    return gfx_rect(0, 0, compositor_width(), h > 0 ? h : 0);
}

bool taskbar_contains(int32_t x, int32_t y) {
    gfx_rect_t r = taskbar_rect();
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static bool inside(gfx_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

// The Start button is its picture alone, a little wider than high; the
// word "Start" stands in while the image isn't installed
gfx_rect_t taskbar_start_rect(void) {
    gfx_rect_t bar = taskbar_rect();
    int32_t h = bar.h - 2 * BAR_PAD_Y - 1;
    int32_t w = (imagelib_get(START_IMAGE) != NULL) ? h + 2 * BUTTON_PAD_X
                                                    : dwm_text_width("Start", 5) + 2 * BUTTON_PAD_X;
    return gfx_rect(bar.x + BAR_PAD_X, bar.y + BAR_PAD_Y + 1, w, h);
}

// Where a click counts for Start: the button stretched down to the bottom
// edge of the screen and left to its left edge, so a pointer pushed into
// the corner still hits it. Drawn as taskbar_start_rect.
gfx_rect_t taskbar_start_hit_rect(void) {
    gfx_rect_t bar = taskbar_rect();
    gfx_rect_t r   = taskbar_start_rect();
    return gfx_rect(bar.x, r.y, r.x + r.w - bar.x, bar.y + bar.h - r.y);
}

// A window button's clickable place: its rect stretched down to the
// bottom edge of the screen (empty stays empty)
static gfx_rect_t button_hit_rect(gfx_rect_t r) {
    if (r.w <= 0 || r.h <= 0)
        return r;
    gfx_rect_t bar = taskbar_rect();
    return gfx_rect(r.x, r.y, r.w, bar.y + bar.h - r.y);
}

//
// Clock
//

// Milliseconds on the clock the taskbar shows: the wall clock, or the
// uptime without one
static uint64_t clock_source_ms(void) {
    uint64_t ms;
    return dwm_clock_ms(&ms) ? ms : dwm_time_ms();
}

// Which minute is showing (repaint when it changes)
static uint64_t clock_minute(void) {
    return clock_source_ms() / 60000u;
}

// "3:07 PM" from the wall clock, or "up 12:34" (hours and minutes since
// boot) without one
static void clock_text(char* buf, size_t size) {
    struct tm tm;
    if (dwm_clock_now(&tm)) {
        int hour = tm.tm_hour % 12;
        snprintf(buf, size, "%d:%02d %s", hour == 0 ? 12 : hour, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
        return;
    }
    uint32_t m = (uint32_t)(dwm_time_ms() / 60000u);
    snprintf(buf, size, "up %u:%02u", (unsigned)(m / 60u), (unsigned)(m % 60u));
}

// "Friday, November 6, 2026", or "" without a wall clock
static void clock_tip_text(char* buf, size_t size) {
    static const char* const days[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
    };
    static const char* const months[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    struct tm tm;
    if (size == 0)
        return;
    buf[0] = '\0';
    if (!dwm_clock_now(&tm) || tm.tm_wday < 0 || tm.tm_wday > 6 || tm.tm_mon < 0 || tm.tm_mon > 11)
        return;
    snprintf(buf, size, "%s, %s %d, %d", days[tm.tm_wday], months[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
}

// Room for the widest of either form
static int32_t clock_width(void) {
    int32_t wall   = dwm_text_width("12:59 PM", 8);
    int32_t uptime = dwm_text_width("up 000:00", 9);
    return (wall > uptime ? wall : uptime) + 2 * TRAY_PAD_X;
}

// The tray: the icons (tray_width), then the clock
static gfx_rect_t tray_rect(void) {
    gfx_rect_t bar = taskbar_rect();
    int32_t w = tray_width() + clock_width();
    int32_t most = bar.w / 2;                   // Leave the window buttons some room
    if (w > most && most > clock_width())
        w = most;
    return gfx_rect(bar.x + bar.w - BAR_PAD_X - w, bar.y + BAR_PAD_Y + 1, w, bar.h - 2 * BAR_PAD_Y - 1);
}

static gfx_rect_t tray_icons_rect(void) {
    gfx_rect_t r = tray_rect();
    int32_t w = r.w - clock_width();
    return gfx_rect(r.x, r.y, w > 0 ? w : 0, r.h);
}

static gfx_rect_t clock_rect(void) {
    gfx_rect_t r = tray_rect();
    int32_t w = clock_width();
    return gfx_rect(r.x + r.w - w, r.y, w, r.h);
}

// The date tooltip: above the clock, kept on the screen; empty when it isn't
// showing
static gfx_rect_t clock_tip_rect(void) {
    char text[48];
    if (!clock_hover)
        return gfx_rect(0, 0, 0, 0);
    clock_tip_text(text, sizeof(text));
    if (text[0] == '\0')
        return gfx_rect(0, 0, 0, 0);
    gfx_rect_t c = clock_rect();
    int32_t w = dwm_text_width(text, strlen(text)) + 2 * CLOCK_TIP_PAD_X;
    int32_t h = dwm_text_height() + 2 * CLOCK_TIP_PAD_Y;
    int32_t sw = compositor_width();
    int32_t x = c.x + c.w / 2 - w / 2;
    if (x + w > sw) x = sw - w;
    if (x < 0) x = 0;
    int32_t y = taskbar_rect().y - CLOCK_TIP_LIFT - h;
    if (y < 0) y = 0;
    return gfx_rect(x, y, w, h);
}

static void clock_set_hover(bool on) {
    if (on == clock_hover)
        return;
    compositor_invalidate(clock_tip_rect());    // Old place (empty when it was off)
    clock_hover = on;
    compositor_invalidate(clock_tip_rect());
}

static void clock_paint_tip(gfx_surface_t* s, gfx_rect_t area) {
    gfx_rect_t r = clock_tip_rect();
    if (rect_empty(r) || !rect_overlaps(area, r))
        return;
    char text[48];
    clock_tip_text(text, sizeof(text));
    const dwm_theme_t* t = dwm_theme();
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, t->menu.background | 0xFF000000u);
    gfx_draw_rect(s, r.x, r.y, r.w, r.h, t->menu.border);
    dwm_text_draw(s, r.x + CLOCK_TIP_PAD_X, r.y + CLOCK_TIP_PAD_Y, text, strlen(text), t->menu.text);
}

// Where each window button goes, between Start and the tray
static void place_buttons(void) {
    if (button_count == 0)
        return;
    gfx_rect_t start = taskbar_start_rect();
    gfx_rect_t tray  = tray_rect();
    int32_t left  = start.x + start.w + BUTTON_GAP * 2;
    int32_t right = tray.x - BUTTON_GAP * 2;
    int32_t room  = right - left;
    int32_t n     = (int32_t)button_count;
    int32_t w     = (room - (n - 1) * BUTTON_GAP) / n;
    if (w > WINDOW_BUTTON_MAX) w = WINDOW_BUTTON_MAX;
    if (w < WINDOW_BUTTON_MIN) w = WINDOW_BUTTON_MIN;
    for (size_t i = 0; i < button_count; i++) {
        int32_t x = left + (int32_t)i * (w + BUTTON_GAP);
        // Past the room: no place (empty), so it can't be hit or drawn
        buttons[i].rect = (x + w <= right) ? gfx_rect(x, start.y, w, start.h) : gfx_rect(0, 0, 0, 0);
    }
}

//
// Following the windows
//

// A window that gets a button: one with a titlebar that shows or is minimized
static bool has_button(const dwm_window_t* w) {
    return w->handle != 0 && !w->doomed && !w->destroying && window_title_height(w) > 0 &&
           (w->visible || w->minimized);
}

static uint32_t hash_step(uint32_t h, uint32_t v) {
    return (h ^ v) * 16777619u;
}

static uint32_t hash_text(uint32_t h, const char* s) {
    while (*s != '\0')
        h = hash_step(h, (uint8_t)*s++);
    return hash_step(h, 0);
}

static void sync_buttons(void) {
    // Keep the ones still wanted, in order
    size_t kept = 0;
    for (size_t i = 0; i < button_count; i++) {
        dwm_window_t* w = window_get(buttons[i].window);
        if (w != NULL && has_button(w))
            buttons[kept++] = buttons[i];
    }
    button_count = kept;

    // New windows at the end (the stacking order is close to the order of
    // creation for windows that just appeared)
    for (dwm_window_t* w = window_bottom(); w != NULL && button_count < MAX_BUTTONS; w = w->above) {
        if (!has_button(w))
            continue;
        bool known = false;
        for (size_t i = 0; i < button_count && !known; i++)
            known = (buttons[i].window == w->handle);
        if (!known) {
            buttons[button_count].window = w->handle;
            buttons[button_count].rect   = gfx_rect(0, 0, 0, 0);
            button_count++;
        }
    }
    // A window button gone (tray icons are numbered after every window
    // button slot, so they never count as one here: they used to, and a
    // right press on a tray icon was forgotten by the next tick, before
    // the button came up)
    if (pressed_item >= ITEM_WINDOW && pressed_item < ITEM_TRAY && (size_t)(pressed_item - ITEM_WINDOW) >= button_count)
        pressed_item = ITEM_NONE;
    if (hover_item >= ITEM_WINDOW && hover_item < ITEM_TRAY && (size_t)(hover_item - ITEM_WINDOW) >= button_count)
        hover_item = ITEM_NONE;

    // Tray icons came or went: a press on one is forgotten (its index may
    // be another icon's now), and the hover is the tray's to drop
    // (A new picture or tooltip keeps the press: the Wi-Fi icon changes
    // every few hundred ms while it scans.)
    if (pressed_item >= ITEM_TRAY && pressed_tray != tray_places_serial())
        pressed_item = ITEM_NONE;
    if (hover_item >= ITEM_TRAY && (size_t)(hover_item - ITEM_TRAY) >= tray_count())
        hover_item = ITEM_NONE;
}

void taskbar_tick(void) {
    sync_buttons();

    uint32_t h = 2166136261u;
    h = hash_step(h, (uint32_t)compositor_width());
    h = hash_step(h, (uint32_t)compositor_height());
    h = hash_step(h, (uint32_t)dwm_text_height());
    h = hash_step(h, dwm_window_get_focus());
    h = hash_step(h, dwm_start_is_open());
    h = hash_step(h, tray_serial());
    for (size_t i = 0; i < button_count; i++) {
        dwm_window_t* w = window_get(buttons[i].window);
        h = hash_step(h, buttons[i].window);
        h = hash_step(h, w->minimized);
        h = hash_text(h, w->title);
    }
    if (h != shown_hash) {
        shown_hash = h;
        place_buttons();
        tray_layout(tray_icons_rect());
        compositor_invalidate(taskbar_rect());
    }

    uint64_t minute = clock_minute();
    if (minute != shown_minute) {
        shown_minute = minute;
        compositor_invalidate(clock_rect());
        compositor_invalidate(clock_tip_rect());    // The date, past midnight
    }
}

// Until the clock's next minute begins
int32_t taskbar_next_timeout(void) {
    return (int32_t)(60000u - (uint32_t)(clock_source_ms() % 60000u));
}

//
// Minimizing
//

void dwm_window_minimize(dwm_handle_t window) {
    dwm_window_t* w = window_get(window);
    if (w == NULL || !w->visible || !window_can_minimize(w))
        return;
    dwm_window_show(window, false);     // Focus goes to the next window
    w->minimized = true;                // After: showing it clears this
}

bool dwm_window_minimized(dwm_handle_t window) {
    dwm_window_t* w = window_get(window);
    return w != NULL && w->minimized;
}

// The window in front: the focused one, or (none focused) the top visible one
static bool in_front(const dwm_window_t* w) {
    return w->visible && dwm_window_get_focus() == w->handle;
}

static void restore(dwm_handle_t window) {
    dwm_window_t* w = window_get(window);
    if (w == NULL)
        return;
    if (!w->visible)
        dwm_window_show(window, true);      // Raises and focuses it too
    dwm_window_raise(window);
    if (!(w->style & DWM_STYLE_NOFOCUS))
        dwm_window_focus(window);
}

//
// Mouse
//

static int item_at(int32_t x, int32_t y) {
    if (!taskbar_contains(x, y))
        return ITEM_NONE;
    if (inside(taskbar_start_hit_rect(), x, y))
        return ITEM_START;
    for (size_t i = 0; i < button_count; i++) {
        if (inside(button_hit_rect(buttons[i].rect), x, y))
            return ITEM_WINDOW + (int)i;
    }
    int icon = tray_icon_at(x, y);
    if (icon >= 0)
        return ITEM_TRAY + icon;
    return ITEM_NONE;
}

static gfx_rect_t item_rect(int item) {
    if (item == ITEM_START)
        return taskbar_start_rect();
    if (item >= ITEM_TRAY)
        return tray_icon_rect(item - ITEM_TRAY);
    if (item >= ITEM_WINDOW && (size_t)(item - ITEM_WINDOW) < button_count)
        return buttons[item - ITEM_WINDOW].rect;
    return gfx_rect(0, 0, 0, 0);
}

void taskbar_hover(int32_t x, int32_t y) {
    clock_set_hover(taskbar_contains(x, y) && inside(clock_rect(), x, y));
    int item = item_at(x, y);
    if (item == hover_item)
        return;
    compositor_invalidate(item_rect(hover_item));
    hover_item = item;
    compositor_invalidate(item_rect(hover_item));
    tray_hover(item >= ITEM_TRAY ? item - ITEM_TRAY : -1);     // The tooltip
}

void taskbar_press(int32_t x, int32_t y, uint8_t button) {
    int item = item_at(x, y);
    if (item == ITEM_START && button == INPUT_BTN_LEFT) {
        // Opens on the press, as a menu does; startmenu.c closes it again
        // when the button is pressed while it is open
        dwm_start_open();
        compositor_invalidate(item_rect(ITEM_START));
        pressed_item = ITEM_NONE;
        return;
    }
    if (item >= ITEM_TRAY && button == INPUT_BTN_LEFT && tray_icon_panel(item - ITEM_TRAY) &&
        pressed_item == ITEM_NONE) {
        // The tray panel opens on the press too, like Start; a press on the
        // icon while it is open closes it (startmenu.c sees that first).
        // The program hears of it, to fill the panel.
        dwm_start_open_tray(item - ITEM_TRAY);
        tray_click(item - ITEM_TRAY, button, x, y);
        return;
    }
    if (pressed_item != ITEM_NONE)
        return;                 // Another button while one is down
    pressed_item   = item;
    pressed_button = button;
    pressed_tray   = tray_places_serial();
    compositor_invalidate(item_rect(item));
}

void taskbar_release(int32_t x, int32_t y, uint8_t button) {
    if (pressed_item == ITEM_NONE || button != pressed_button)
        return;
    int item = pressed_item;
    pressed_item = ITEM_NONE;
    compositor_invalidate(item_rect(item));
    if (item_at(x, y) != item || item < ITEM_WINDOW)
        return;
    if (item >= ITEM_TRAY) {
        // To the program that owns the icon (any button)
        if (pressed_tray == tray_places_serial())
            tray_click(item - ITEM_TRAY, button, x, y);
        return;
    }

    dwm_handle_t window = buttons[item - ITEM_WINDOW].window;
    dwm_window_t* w = window_get(window);
    if (w == NULL)
        return;

    if (button == INPUT_BTN_LEFT) {
        if (in_front(w))
            dwm_window_minimize(window);    // Not NOMINIMIZE windows: they stay
        else
            restore(window);
    } else if (button == INPUT_BTN_RIGHT) {
        // The window menu, as for a right click on its frame. dwm_menu_show
        // takes client coordinates and needs the window showing.
        const dwm_menu_t* menu = dwm_get_window_menu();
        if (menu == NULL)
            return;
        if (!w->visible)
            restore(window);
        gfx_rect_t client = window_client(w);
        dwm_menu_show(window, menu, x - client.x, y - client.y);
    }
    taskbar_tick();
}

void taskbar_cancel(void) {
    if (pressed_item != ITEM_NONE)
        compositor_invalidate(item_rect(pressed_item));
    pressed_item = ITEM_NONE;
}

bool taskbar_pressing(void) {
    return pressed_item != ITEM_NONE;
}

//
// Small images (start menu lines)
//

typedef struct {
    const image_t* source;
    int32_t        size;
    int32_t        w, h;
    uint32_t*      pixels;          // w * h, straight alpha
} small_t;

static small_t small[SMALL_CACHE];
static size_t  small_next;

// Shrink `src` to fit size x size: each pixel the average of the source
// pixels it covers, see-through ones (SYS_GFX_SPRITE_KEY) counting as clear
static bool shrink(const image_t* src, int32_t size, small_t* out) {
    int32_t w = src->width, h = src->height;
    if (w <= 0 || h <= 0)
        return false;
    int32_t dw = size, dh = size;
    if (w >= h) dh = (h * size + w / 2) / w;
    else        dw = (w * size + h / 2) / h;
    if (w <= size && h <= size) { dw = w; dh = h; }    // Small already
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    uint32_t* px = (uint32_t*)malloc((size_t)dw * (size_t)dh * sizeof(uint32_t));
    if (px == NULL)
        return false;
    for (int32_t dy = 0; dy < dh; dy++) {
        int32_t y0 = dy * h / dh, y1 = (dy + 1) * h / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int32_t dx = 0; dx < dw; dx++) {
            int32_t x0 = dx * w / dw, x1 = (dx + 1) * w / dw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int32_t y = y0; y < y1; y++) {
                for (int32_t x = x0; x < x1; x++) {
                    uint32_t c = src->pixels[y * w + x];
                    n++;
                    if (c == SYS_GFX_SPRITE_KEY)
                        continue;
                    uint32_t ca = c >> 24;
                    a += ca;
                    r += ((c >> 16) & 0xFF) * ca;
                    g += ((c >> 8) & 0xFF) * ca;
                    b += (c & 0xFF) * ca;
                }
            }
            uint32_t out_px = 0;
            if (a > 0) {
                out_px = ((a / n) << 24) | ((r / a) << 16) | ((g / a) << 8) | (b / a);
            }
            px[dy * dw + dx] = out_px;
        }
    }
    out->source = src;
    out->size   = size;
    out->w      = dw;
    out->h      = dh;
    out->pixels = px;
    return true;
}

void small_image_draw(gfx_surface_t* s, const image_t* image, int32_t x, int32_t y, int32_t size) {
    if (image == NULL || size <= 0)
        return;
    small_t* hit = NULL;
    for (size_t i = 0; i < SMALL_CACHE && hit == NULL; i++) {
        if (small[i].source == image && small[i].size == size && small[i].pixels != NULL)
            hit = &small[i];
    }
    if (hit == NULL) {
        small_t* slot = &small[small_next];
        small_next = (small_next + 1) % SMALL_CACHE;
        free(slot->pixels);
        memset(slot, 0, sizeof(*slot));
        if (!shrink(image, size, slot))
            return;
        hit = slot;
    }
    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_draw_pixels(s, hit->pixels, hit->w, hit->h, 0, x + (size - hit->w) / 2, y + (size - hit->h) / 2);
    gfx_set_blend(s, old);
}

void taskbar_shutdown(void) {
    for (size_t i = 0; i < SMALL_CACHE; i++) {
        free(small[i].pixels);
        memset(&small[i], 0, sizeof(small[i]));
    }
    button_count = 0;
    pressed_item = hover_item = ITEM_NONE;
    tray_shutdown();
    shown_hash = 0;
    shown_minute = UINT64_MAX;
    clock_hover = false;
}

//
// Painting
//

static void draw_button(gfx_surface_t* s, gfx_rect_t r, bool down, bool hot, bool active) {
    const dwm_theme_t* t = dwm_theme();
    uint32_t fill = down ? t->menu.highlight : active ? t->frame.active_high : t->button.fill;
    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_fill_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, fill);
    gfx_set_blend(s, old);
    uint32_t border = (hot || active) ? t->button.border_active : t->button.border;
    gfx_draw_rect(s, r.x, r.y, r.w, r.h, border);
    if (down || active)     // Pressed in: a dark line along the top
        gfx_draw_hline(s, r.x + 1, r.y + 1, r.w - 2, t->frame.border);
}

static void draw_start(gfx_surface_t* s) {
    const dwm_theme_t* t = dwm_theme();
    gfx_rect_t r = taskbar_start_rect();
    bool open = dwm_start_is_open() && !dwm_start_is_tray();     // The tray panel leaves Start up
    draw_button(s, r, open, hover_item == ITEM_START, false);
    int32_t off = open ? 1 : 0;

    // The picture centered, at its own size when it fits, else shrunk
    const image_t* image = imagelib_get(START_IMAGE);
    if (image != NULL) {
        int32_t room = r.h - 4;
        if (image->width <= room && image->height <= room)
            image_draw(s, image, r.x + (r.w - image->width) / 2 + off, r.y + (r.h - image->height) / 2 + off);
        else
            small_image_draw(s, image, r.x + (r.w - room) / 2 + off, r.y + 2 + off, room);
        return;
    }
    int32_t lh = dwm_text_height();
    dwm_text_draw(s, r.x + BUTTON_PAD_X + off, r.y + (r.h - lh) / 2 + off, "Start", 5, t->frame.title_text);
}

static void draw_window_button(gfx_surface_t* s, size_t i) {
    const dwm_theme_t* t = dwm_theme();
    gfx_rect_t r = buttons[i].rect;
    if (r.w == 0)
        return;
    dwm_window_t* w = window_get(buttons[i].window);
    if (w == NULL)
        return;
    int item = ITEM_WINDOW + (int)i;
    bool down = (pressed_item == item);
    draw_button(s, r, down, hover_item == item, in_front(w));

    // The title, cut to fit; dimmed while minimized
    int32_t lh = dwm_text_height();
    int32_t room = r.w - 2 * BUTTON_PAD_X;
    if (room <= 0 || w->title[0] == '\0')
        return;
    char text[DWM_TITLE_MAX + 4];
    size_t len = dwm_text_ellipsize(w->title, strlen(w->title), room, "..", false, text, sizeof(text));
    uint32_t color = w->minimized ? t->client.text_muted : t->frame.title_text;
    int32_t off = down ? 1 : 0;
    dwm_text_draw(s, r.x + BUTTON_PAD_X + off, r.y + (r.h - lh) / 2 + off, text, len, color);
}

static void draw_tray(gfx_surface_t* s, gfx_rect_t area) {
    const dwm_theme_t* t = dwm_theme();
    gfx_rect_t r = tray_rect();
    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, 0x60000000);
    gfx_set_blend(s, old);
    gfx_draw_hline(s, r.x, r.y, r.w, t->frame.border);
    gfx_draw_vline(s, r.x, r.y, r.h, t->frame.border);
    tray_paint(s, area);

    r = clock_rect();
    char text[24];
    clock_text(text, sizeof(text));
    size_t len = strlen(text);
    int32_t lh = dwm_text_height();
    int32_t tw = dwm_text_width(text, len);
    dwm_text_draw(s, r.x + (r.w - tw) / 2, r.y + (r.h - lh) / 2, text, len, t->client.text_soft);
}

void taskbar_paint(gfx_surface_t* s, gfx_rect_t area) {
    gfx_rect_t bar = taskbar_rect();
    if (!rect_overlaps(area, bar)) {
        tray_paint_tip(s, area);
        clock_paint_tip(s, area);
        return;
    }
    const dwm_theme_t* t = dwm_theme();
    gfx_fill_gradient(s, bar.x, bar.y, bar.w, bar.h, t->taskbar.gradient_low, t->taskbar.gradient_high,
                      GFX_GRADIENT_VERTICAL);
    gfx_draw_hline(s, bar.x, bar.y, bar.w, t->button.border_active);

    draw_start(s);
    for (size_t i = 0; i < button_count; i++) {
        if (rect_overlaps(area, buttons[i].rect))
            draw_window_button(s, i);
    }
    if (rect_overlaps(area, tray_rect()))
        draw_tray(s, area);
    tray_paint_tip(s, area);        // Above the strip, over the windows
    clock_paint_tip(s, area);
}

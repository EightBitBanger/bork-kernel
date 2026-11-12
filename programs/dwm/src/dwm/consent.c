/*
 * The administrator prompt (consent.h).
 *
 * Not a window: the prompt lives here, is painted by the compositor after
 * everything else (but the cursor) and gets input from input.c before
 * anything else, so nothing a program can ask the DWM for (raise, focus,
 * move, topmost, pointer lock, a menu, a drag) can reach it.
 *
 * One request at a time, as the kernel hands them out. The text is laid out
 * once when the prompt opens; painting only draws it.
 *
 * The desktop's own launches (consent_ask_launch) use the same prompt: the
 * DWM is the agent, so it can't wait in the kernel for its own answer. A
 * kernel request that comes in while one of those is up is claimed (NEXT)
 * so the input handle stops reading ready, and shown right after.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/security.h>

#include "dwm.h"
#include "consent.h"
#include "compositor.h"
#include "dirty.h"
#include "internal.h"

// Fixed colors: a theme (or a program that could someday change one) must
// not be able to make the prompt look like anything else
#define C_DIM           0xB8000000u     // Over the rest of the screen
#define C_PANEL         0xFF15151Fu
#define C_EDGE          0xFFE0A000u     // Amber: border and header
#define C_HEAD_TEXT     0xFF141414u
#define C_TEXT          0xFFFFFFFFu
#define C_SOFT          0xFFC8C8D8u
#define C_LABEL         0xFF9090A8u
#define C_BUTTON        0xFF262636u
#define C_BUTTON_HOT    0xFF34344Au
#define C_BUTTON_EDGE   0xFF5A5A78u
#define C_BUTTON_DEAD   0xFF6A6A7Au

#define PANEL_W         480
#define PAD             14
#define EDGE            2
#define LABEL_W         78              // Label column ("Location")
#define LINE_GAP        3
#define PARA_GAP        10
#define BUTTON_W        104
#define BUTTON_GAP      12
#define MAX_LINES       32
#define BULLET_W        12              // Room for a bullet left of its text
#define BULLET_SIZE     4
#define LINE_MAX        160

#define CHECK_MS        200             // How often an open prompt asks whether it still waits

typedef enum { BTN_NONE = 0, BTN_ALLOW, BTN_CANCEL } button_t;

typedef struct {
    char     text[LINE_MAX];
    uint32_t color;
    int32_t  x, y;                      // Panel coordinates
    bool     bullet;                    // A bullet drawn left of the text
} line_t;

static struct {
    bool       registered;
    bool       open;
    bool       local;                   // The open prompt is consent_ask_launch's
    sec_elevate_request_t req;

    // consent_ask_launch() waiting for the prompt (one at a time)
    bool       ask_pending;
    sec_elevate_request_t ask;
    consent_done_fn ask_done;
    void*      ask_ctx;
    consent_done_fn done;               // The open local prompt's
    void*      done_ctx;

    uint64_t   shown_ms;
    uint64_t   next_check_ms;
    bool       armed_drawn;             // Repainted once armed (Allow no longer grey)

    gfx_rect_t panel;                   // Screen
    gfx_rect_t allow, cancel;           // Screen
    int32_t    head_h;
    line_t     lines[MAX_LINES];
    size_t     line_count;

    button_t   focus;                   // Keyboard
    button_t   hot;                     // Under the pointer
    button_t   pressed;                 // Left button went down on it (armed)
    uint8_t    buttons;
    bool       all_up;                  // Every button was up once since the prompt opened
} cs;

//
// Layout
//

static bool armed(void) {
    return cs.open && dwm_time_ms() - cs.shown_ms >= CONSENT_ARM_MS;
}

static void add_line(const char* text, size_t len, uint32_t color, int32_t x, int32_t* y) {
    if (cs.line_count == MAX_LINES)
        return;
    line_t* l = &cs.lines[cs.line_count++];
    if (len >= sizeof(l->text))
        len = sizeof(l->text) - 1;
    memcpy(l->text, text, len);
    l->text[len] = '\0';
    l->color = color;
    l->x = x;
    l->y = *y;
    *y += dwm_text_height() + LINE_GAP;
}

// `text` word wrapped into `width` at x, at most `max` lines ("..." when cut)
static void add_wrapped(const char* text, uint32_t color, int32_t x, int32_t width, size_t max, int32_t* y) {
    dwm_text_line_t at[6];
    if (max > 6)
        max = 6;
    size_t len = strlen(text);
    bool more = false;
    size_t n = dwm_text_wrap(text, len, width, at, max, &more);
    for (size_t i = 0; i < n; i++) {
        char buf[LINE_MAX];
        size_t l = dwm_text_ellipsize(text + at[i].start, at[i].len, width, "...",
                                      more && i == n - 1, buf, sizeof(buf));
        add_line(buf, l, color, x, y);
    }
}

// "Label   value", the value wrapped in the column right of the label
static void add_field(const char* label, const char* value, uint32_t color, size_t max, int32_t* y) {
    int32_t width = cs.panel.w - 2 * PAD;
    int32_t label_y = *y;
    add_wrapped(value, color, PAD + LABEL_W, width - LABEL_W, max, y);
    add_line(label, strlen(label), C_LABEL, PAD, &label_y);
}

static void layout(void) {
    int32_t sw = dwm_screen_width(), sh = dwm_screen_height();
    int32_t th = dwm_text_height();
    int32_t w = PANEL_W;
    if (w > sw - 16)
        w = sw - 16;

    cs.panel = gfx_rect(0, 0, w, 0);   // Width first: the wrapping needs it
    cs.line_count = 0;
    cs.head_h = th + 2 * 8;
    int32_t width = w - 2 * PAD;
    int32_t y = cs.head_h + PAD;

    // A launch (SYS_ELEVATE_LAUNCH): the program to be started, and who is
    // starting it. Otherwise the process asking for itself.
    bool launch = (cs.req.flags & SYS_ELEVATE_LAUNCH) != 0;
    add_wrapped(launch ? "A program is requesting elevated permissions to start."
                       : "A process is requesting elevated permissions.", C_TEXT, PAD, width, 3, &y);
    y += PARA_GAP;

    char pid[32 + SYS_ELEVATE_NAME_MAX];
    if (launch)
        snprintf(pid, sizeof(pid), "%s (process %ld)", cs.req.by[0] ? cs.req.by : "(no name)",
                 (long)cs.req.pid);
    else
        snprintf(pid, sizeof(pid), "%ld", (long)cs.req.pid);
    add_field("Program", cs.req.name[0] ? cs.req.name : "(no name)", C_TEXT, 1, &y);
    add_field("Location", cs.req.path[0] ? cs.req.path : "(unknown)", C_TEXT, 3, &y);
    add_field(launch ? "Started by" : "Process", pid, C_SOFT, 1, &y);

    // What it would get: one bit a line, by name, nothing more
    uint32_t requested = cs.req.requested ? cs.req.requested : SEC_ADMIN;
    int32_t label_y = y;
    bool first = true;
    for (uint32_t i = 0; i < 32; i++) {
        uint32_t bit = 1u << i;
        if (!(requested & bit))
            continue;
        const char* name = sec_bit_name(bit);
        char unknown[16];
        if (name == NULL) {
            snprintf(unknown, sizeof(unknown), "0x%08lx", (unsigned long)bit);
            name = unknown;
        }
        if (cs.line_count == MAX_LINES)
            break;
        add_line(name, strlen(name), C_TEXT, PAD + LABEL_W + BULLET_W, &y);
        cs.lines[cs.line_count - 1].bullet = true;
        first = false;
    }
    if (!first)
        add_line("Permissions", strlen("Permissions"), C_LABEL, PAD, &label_y);

    if (cs.req.waiting > 0) {
        y += PARA_GAP;
        char more[64];
        snprintf(more, sizeof(more), "%lu more request%s waiting after this one",
                 (unsigned long)cs.req.waiting, cs.req.waiting == 1 ? "" : "s");
        add_wrapped(more, C_LABEL, PAD, width, 1, &y);
    }
    y += PARA_GAP;

    int32_t bh = th + 12;
    int32_t h = y + bh + PAD;
    if (h > sh - 16)
        h = sh - 16;

    cs.panel.x = (sw - w) / 2;
    cs.panel.y = (sh - h) / 2;
    cs.panel.h = h;

    // Cancel on the right, where Enter's default sits in most dialogs
    int32_t by = cs.panel.y + h - PAD - bh;
    int32_t right = cs.panel.x + w - PAD;
    cs.cancel = gfx_rect(right - BUTTON_W, by, BUTTON_W, bh);
    cs.allow  = gfx_rect(right - 2 * BUTTON_W - BUTTON_GAP, by, BUTTON_W, bh);
}

static bool inside(gfx_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static button_t button_at(int32_t x, int32_t y) {
    if (inside(cs.allow, x, y))  return BTN_ALLOW;
    if (inside(cs.cancel, x, y)) return BTN_CANCEL;
    return BTN_NONE;
}

static void redraw_panel(void) {
    compositor_invalidate(cs.panel);
}

//
// Opening and closing
//

static void open_prompt(const sec_elevate_request_t* r) {
    // Whatever was going on ends here: the prompt owns the input now
    dwm_menu_close();
    dwm_start_close();
    input_cancel();             // Drags, captures, the pointer lock

    memset(&cs.lines, 0, sizeof(cs.lines));
    cs.req           = *r;
    cs.req.name[sizeof(cs.req.name) - 1]     = '\0';
    cs.req.path[sizeof(cs.req.path) - 1]     = '\0';
    cs.req.reason[sizeof(cs.req.reason) - 1] = '\0';
    cs.req.by[sizeof(cs.req.by) - 1]         = '\0';
    cs.open          = true;
    cs.shown_ms      = dwm_time_ms();
    cs.next_check_ms = cs.shown_ms + CHECK_MS;
    cs.armed_drawn   = false;
    cs.focus         = BTN_CANCEL;
    cs.hot           = BTN_NONE;
    cs.pressed       = BTN_NONE;
    cs.all_up        = false;
    layout();
    compositor_invalidate_all();
}

static void close_prompt(void) {
    cs.open = false;
    cs.local = false;
    compositor_invalidate_all();
}

static void answer(bool allow) {
    if (!cs.open)
        return;

    // The desktop's own launch: the answer goes back to it, not the kernel
    if (cs.local) {
        consent_done_fn done = cs.done;
        void* ctx = cs.done_ctx;
        cs.done = NULL;
        cs.done_ctx = NULL;
        close_prompt();
        if (done != NULL)
            done(allow, ctx);
        return;
    }

    if (sec_agent_answer(cs.req.id, allow) < 0) {
        if (allow && errno == EAGAIN)
            return;                     // Too soon for the kernel: stays up
        if (errno != ESRCH) {
            // Something is wrong with the agent itself: never leave it
            // looking as if it worked
            close_prompt();
            dwm_notifyf(DWM_NOTIFY_ERROR, "Admin request failed", "%s: %s", cs.req.name, strerror(errno));
            return;
        }
        // ESRCH: it was over already (the program gave up a moment ago)
        close_prompt();
        dwm_notifyf(DWM_NOTIFY_INFO, "Admin request withdrawn", "%s stopped waiting for an answer",
                    cs.req.name);
        return;
    }

    close_prompt();
}

// Open the waiting consent_ask_launch() request
static void open_local(void) {
    sec_elevate_request_t r = cs.ask;
    consent_done_fn done = cs.ask_done;
    void* ctx = cs.ask_ctx;
    cs.ask_pending = false;
    cs.ask_done = NULL;
    cs.ask_ctx = NULL;

    open_prompt(&r);
    cs.local    = true;
    cs.done     = done;
    cs.done_ctx = ctx;
}

bool consent_ask_launch(const char* path, uint32_t requested, consent_done_fn done, void* ctx) {
    if (!cs.registered || cs.ask_pending || (cs.open && cs.local) || path == NULL || path[0] == '\0') {
        errno = cs.registered ? EBUSY : EAGAIN;
        return false;
    }

    sec_elevate_request_t* r = &cs.ask;
    memset(r, 0, sizeof(*r));
    r->pid   = getpid();
    r->flags = SYS_ELEVATE_LAUNCH;
    r->requested = requested;
    strlcpy(r->path, path, sizeof(r->path));
    const char* base = strrchr(path, '/');
    strlcpy(r->name, (base != NULL && base[1] != '\0') ? base + 1 : path, sizeof(r->name));
    strlcpy(r->by, "desktop", sizeof(r->by));
    cs.ask_done    = done;
    cs.ask_ctx     = ctx;
    cs.ask_pending = true;

    // A kernel prompt that is up stays up: this one comes after it
    if (!cs.open)
        open_local();
    return true;
}

//
// Main loop
//

void consent_init(void) {
    memset(&cs, 0, sizeof(cs));
    if (!sec_has(SEC_ADMIN | SEC_INPUT | SEC_GFX_SCREEN))
        return;
    if (sec_agent_register() < 0) {
        dwm_notifyf(DWM_NOTIFY_ERROR, "No admin prompts", "Programs can't ask for admin rights: %s",
                    strerror(errno));
        return;
    }
    cs.registered = true;
}

void consent_shutdown(void) {
    if (cs.open) {
        if (cs.local) {
            consent_done_fn done = cs.done;
            void* ctx = cs.done_ctx;
            cs.done = NULL;
            if (done != NULL)
                done(false, ctx);
        } else
            sec_agent_answer(cs.req.id, false);
        cs.open = false;
        cs.local = false;
    }
    if (cs.ask_pending) {
        cs.ask_pending = false;
        if (cs.ask_done != NULL)
            cs.ask_done(false, cs.ask_ctx);
    }
}

void consent_tick(void) {
    if (!cs.registered)
        return;

    uint64_t now = dwm_time_ms();
    if (cs.open && cs.local) {
        // A kernel request waiting behind ours: take it off the queue (the
        // kernel hands it out again once we ask), so our input handle
        // stops reading ready while it waits
        sec_elevate_request_t r;
        sec_agent_next(&r);
        if (!cs.armed_drawn && armed()) {
            cs.armed_drawn = true;
            redraw_panel();
        }
        return;
    }
    if (cs.open) {
        if (now >= cs.next_check_ms) {
            cs.next_check_ms = now + CHECK_MS;
            if (sec_agent_waiting(cs.req.id) == 0) {
                close_prompt();
                dwm_notifyf(DWM_NOTIFY_INFO, "Admin request withdrawn",
                            "%s stopped waiting for an answer", cs.req.name);
            }
        }
        if (cs.open && !cs.armed_drawn && armed()) {
            cs.armed_drawn = true;
            redraw_panel();
        }
        return;
    }

    // The kernel's first (it may have been waiting behind a launch of ours)
    sec_elevate_request_t r;
    if (sec_agent_next(&r) == 1)
        open_prompt(&r);
    else if (cs.ask_pending)
        open_local();
}

int32_t consent_next_timeout(void) {
    if (!cs.open)
        return cs.ask_pending ? 0 : -1;
    uint64_t now = dwm_time_ms();
    uint64_t due = cs.next_check_ms;
    if (!cs.armed_drawn && cs.shown_ms + CONSENT_ARM_MS < due)
        due = cs.shown_ms + CONSENT_ARM_MS;
    return (due <= now) ? 0 : (int32_t)(due - now);
}

bool consent_active(void) {
    return cs.open;
}

//
// Input
//

void consent_mouse(int32_t x, int32_t y, uint8_t buttons) {
    if (!cs.open)
        return;

    button_t hot = button_at(x, y);
    if (hot != cs.hot) {
        cs.hot = hot;
        redraw_panel();
    }

    // A button already down when the prompt came up counts for nothing,
    // and neither does anything until every button has been up once
    if (!cs.all_up) {
        cs.buttons = buttons;
        if (buttons == 0)
            cs.all_up = true;
        return;
    }

    uint8_t pressed  = (uint8_t)(buttons & ~cs.buttons);
    uint8_t released = (uint8_t)(cs.buttons & ~buttons);
    cs.buttons = buttons;

    // Cancel works at once; Allow only once armed, for the whole click
    if (pressed & INPUT_BTN_LEFT) {
        cs.pressed = (hot == BTN_CANCEL || armed()) ? hot : BTN_NONE;
        redraw_panel();
    }
    if (released & INPUT_BTN_LEFT) {
        button_t was = cs.pressed;
        cs.pressed = BTN_NONE;
        redraw_panel();
        if (was == BTN_CANCEL && hot == BTN_CANCEL)
            answer(false);
        else if (was == BTN_ALLOW && hot == BTN_ALLOW && armed())
            answer(true);
    }
}

void consent_key(uint16_t key, uint8_t mods) {
    (void)mods;
    if (!cs.open)
        return;

    if (key == KEY_ESCAPE) {
        answer(false);
        return;
    }
    // Nothing else until armed: keys already on their way (typing in a
    // program, a held Enter) must not move the focus or press a button
    if (!armed())
        return;

    switch (key) {
        case KEY_TAB:
        case KEY_LEFT:
        case KEY_RIGHT:
            cs.focus = (cs.focus == BTN_ALLOW) ? BTN_CANCEL : BTN_ALLOW;
            redraw_panel();
            break;
        case KEY_ENTER:
        case ' ':
            answer(cs.focus == BTN_ALLOW);
            break;
        default:
            break;
    }
}

//
// Painting
//

static void paint_button(gfx_surface_t* s, gfx_rect_t r, const char* label, button_t which, bool live) {
    bool hot = live && (cs.hot == which || cs.pressed == which);
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, hot ? C_BUTTON_HOT : C_BUTTON);
    gfx_draw_rect(s, r.x, r.y, r.w, r.h, C_BUTTON_EDGE);
    if (cs.focus == which && live)
        gfx_draw_rect(s, r.x - 2, r.y - 2, r.w + 4, r.h + 4, C_EDGE);

    size_t len = strlen(label);
    int32_t tw = dwm_text_width(label, len);
    dwm_text_draw(s, r.x + (r.w - tw) / 2, r.y + (r.h - dwm_text_height()) / 2, label, len,
                  live ? C_TEXT : C_BUTTON_DEAD);
}

void consent_paint(gfx_surface_t* s, gfx_rect_t area) {
    if (!cs.open)
        return;

    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_fill_rect(s, area.x, area.y, area.w, area.h, C_DIM);
    gfx_set_blend(s, old);

    gfx_rect_t p = cs.panel;
    if (!rect_overlaps(area, gfx_rect(p.x - EDGE, p.y - EDGE, p.w + 2 * EDGE, p.h + 2 * EDGE)))
        return;

    gfx_fill_rect(s, p.x - EDGE, p.y - EDGE, p.w + 2 * EDGE, p.h + 2 * EDGE, C_EDGE);
    gfx_fill_rect(s, p.x, p.y + cs.head_h, p.w, p.h - cs.head_h, C_PANEL);

    const char* head = "Administrator permission";
    dwm_text_draw(s, p.x + PAD, p.y + (cs.head_h - dwm_text_height()) / 2, head, strlen(head), C_HEAD_TEXT);

    // Clipped to the panel: a layout taller than the screen is cut, not spilled
    gfx_rect_t body = rect_intersect(area, gfx_rect(p.x, p.y + cs.head_h, p.w, p.h - cs.head_h));
    if (!rect_empty(body)) {
        gfx_save(s);
        gfx_set_clip(s, &body);
        for (size_t i = 0; i < cs.line_count; i++) {
            const line_t* l = &cs.lines[i];
            if (l->bullet) {
                int32_t th = dwm_text_height();
                gfx_fill_rect(s, p.x + l->x - BULLET_W + 2, p.y + l->y + (th - BULLET_SIZE) / 2,
                              BULLET_SIZE, BULLET_SIZE, l->color);
            }
            dwm_text_draw(s, p.x + l->x, p.y + l->y, l->text, strlen(l->text), l->color);
        }
        gfx_restore(s);
    }

    bool live = armed();
    paint_button(s, cs.allow, "Allow", BTN_ALLOW, live);
    paint_button(s, cs.cancel, "Cancel", BTN_CANCEL, true);
}

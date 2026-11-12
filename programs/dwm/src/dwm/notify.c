/*
 * Desktop notifications (notify.h).
 *
 * Each notification is an ordinary DWM window (topmost, no frame, never
 * focused) whose procedure is nt_proc below. The main loop (dwm.c) calls
 * notify_tick() every round: it times notifications out and slides them one
 * step toward where they belong, and notify_next_timeout() keeps the loop
 * waking up while anything is moving or about to expire.
 *
 * entries[] is ordered bottom to top: [0] sits in the corner, new ones are
 * appended and land on top of the stack.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <os/gfx.h>

#include "dwm.h"
#include "internal.h"

// Layout (client coordinates)
#define NT_WIDTH            240
#define NT_MARGIN           6       // Gap to the screen's right and bottom edges
#define NT_GAP              6       // Between stacked notifications
#define NT_PAD              8
#define NT_ACCENT_W         3       // Colored strip on the left edge
#define NT_TEXT_X           (NT_ACCENT_W + NT_PAD)
#define NT_TITLE_GAP        6       // Under the title
#define NT_LINE_GAP         3       // Between message lines
#define NT_CLOSE_GAP        6       // Between a sticky title and its "x"
#define NT_LINE_MAX         127     // Bytes per line, at most
#define NT_MAX_LINES        4

#define NT_MAX_VISIBLE      5       // Showing at once (the oldest is pushed out)
#define NT_MAX_ENTRIES      12      // Showing + still sliding out
#define NT_MAX_PER_OWNER    3       // Showing at once per program
#define NT_PENDING_MAX      8       // Kept until dwm_init() has finished

#define NT_FRAME_MS         16      // Slide step interval
#define NT_SLIDE_DIV        4       // Each step covers 1/4 of what is left

#define NT_STYLE            (DWM_STYLE_TOPMOST | DWM_STYLE_NOBORDER | DWM_STYLE_NOFOCUS)

typedef struct {
    dwm_handle_t      handle;
    dwm_notify_kind_t kind;
    uint32_t          owner;            // 0: the DWM itself, else the program's pid

    char              title[NT_LINE_MAX + 1];
    char              lines[NT_MAX_LINES][NT_LINE_MAX + 1];
    uint8_t           line_count;
    int32_t           height;

    int32_t           x, y;             // Where the window is now
    int32_t           target_x, target_y;

    uint64_t          expires_ms;       // 0: sticky
    bool              dismissing;       // Sliding out, no longer in the stack
} nt_entry_t;

typedef struct {
    dwm_notify_kind_t kind;
    uint32_t          timeout_ms;
    uint32_t          owner;
    char              title[DWM_NOTIFY_TITLE_MAX];
    char              text[DWM_NOTIFY_TEXT_MAX];
} nt_pending_t;

static nt_entry_t   entries[NT_MAX_ENTRIES];
static size_t       entry_count;
static bool         ready;
static uint64_t     last_step;
static int32_t      font_h = 8;

static nt_pending_t pending[NT_PENDING_MAX];
static size_t       pending_count;

static int find(dwm_handle_t handle) {
    if (handle == 0)
        return -1;
    for (size_t i = 0; i < entry_count; i++)
        if (entries[i].handle == handle)
            return (int)i;
    return -1;
}

static void remove_at(size_t index) {
    if (index >= entry_count)
        return;
    memmove(&entries[index], &entries[index + 1], (entry_count - index - 1) * sizeof(nt_entry_t));
    entry_count--;
}

static int32_t line_height(void) {
    return font_h + NT_LINE_GAP;
}

// Room for text on a line, in pixels
static int32_t text_width(void) {
    return NT_WIDTH - NT_TEXT_X - NT_PAD;
}

// What a sticky title gives up to its "x"
static int32_t close_width(void) {
    return dwm_text_width("x", 1) + NT_CLOSE_GAP;
}

//
// Text
//

// Word-wrap `src` into lines at most `width` pixels wide. Honours '\n',
// hard-breaks words longer than a line, and ends the last line with "..."
// when the text did not fit in NT_MAX_LINES. Tabs show as spaces.
static uint8_t wrap(const char* src, int32_t width, char lines[NT_MAX_LINES][NT_LINE_MAX + 1]) {
    size_t len = strnlen(src, DWM_NOTIFY_TEXT_MAX);
    dwm_text_line_t at[NT_MAX_LINES];
    bool more;
    size_t count = dwm_text_wrap(src, len, width, at, NT_MAX_LINES, &more);

    for (size_t i = 0; i < count; i++) {
        bool last_cut = more && i == count - 1;
        dwm_text_ellipsize(src + at[i].start, at[i].len, width, "...", last_cut,
                           lines[i], NT_LINE_MAX + 1);
        for (char* p = lines[i]; *p != '\0'; p++)
            if (*p == '\t') *p = ' ';
    }
    return (uint8_t)count;
}

static void build(nt_entry_t* e, dwm_notify_kind_t kind, bool sticky, const char* title, const char* message) {
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    int32_t width = text_width();

    // Title: one line, "..." when cut; a sticky one leaves room for the "x"
    int32_t title_width = sticky ? width - close_width() : width;
    dwm_text_ellipsize(title, strnlen(title, DWM_NOTIFY_TITLE_MAX - 1), title_width, "...", false,
                       e->title, sizeof(e->title));
    for (char* p = e->title; *p != '\0'; p++)
        if (*p == '\n' || *p == '\t') *p = ' ';

    e->line_count = wrap(message, width, e->lines);

    int32_t h = NT_PAD * 2 + font_h;
    if (e->line_count > 0)
        h += NT_TITLE_GAP + e->line_count * line_height() - NT_LINE_GAP;
    e->height = h;
}

static bool same_text(const nt_entry_t* a, const nt_entry_t* b) {
    if (a->kind != b->kind || a->line_count != b->line_count || strcmp(a->title, b->title) != 0)
        return false;
    for (uint8_t l = 0; l < a->line_count; l++)
        if (strcmp(a->lines[l], b->lines[l]) != 0)
            return false;
    return true;
}

//
// Stack layout and movement
//

// Give every notification still in the stack its place, bottom up
static void layout(void) {
    int32_t x      = dwm_screen_width() - NT_WIDTH - NT_MARGIN;
    int32_t bottom = dwm_work_area().h - NT_MARGIN;     // Above the taskbar

    for (size_t i = 0; i < entry_count; i++) {
        nt_entry_t* e = &entries[i];
        if (e->dismissing)
            continue;
        e->target_x = x;
        e->target_y = bottom - e->height;
        bottom = e->target_y - NT_GAP;
    }
}

// Out of the stack: it slides off to the right, those above close the gap
static void dismiss(nt_entry_t* e) {
    if (e->dismissing)
        return;
    e->dismissing = true;
    e->target_x = dwm_screen_width();
    layout();
}

static int32_t slide(int32_t from, int32_t to) {
    int32_t delta = to - from;
    if (delta == 0)
        return from;
    int32_t step = delta / NT_SLIDE_DIV;
    if (step == 0)
        step = (delta > 0) ? 1 : -1;
    return from + step;
}

static bool moving(const nt_entry_t* e) {
    return e->x != e->target_x || e->y != e->target_y;
}

//
// The window procedure
//

static void paint(const nt_entry_t* e, gfx_surface_t* s) {
    const dwm_theme_t* t = dwm_theme();
    bool error = (e->kind == DWM_NOTIFY_ERROR);

    uint32_t background = error ? t->client.background_danger : t->client.background;
    uint32_t accent     = error ? t->client.text_danger       : t->client.accent;
    uint32_t border     = error ? t->client.text_danger       : t->button.border;
    uint32_t title      = error ? t->client.text              : t->client.text_value;
    uint32_t text       = t->client.text_soft;

    gfx_fill_rect(s, 0, 0, NT_WIDTH, e->height, background);
    gfx_fill_rect(s, 0, 0, NT_ACCENT_W, e->height, accent);
    gfx_draw_rect(s, 0, 0, NT_WIDTH, e->height, border);

    dwm_text_draw(s, NT_TEXT_X, NT_PAD, e->title, strlen(e->title), title);
    if (e->expires_ms == 0)     // Sticky: click to close
        dwm_text_draw(s, NT_WIDTH - NT_PAD - dwm_text_width("x", 1), NT_PAD, "x", 1, title);

    int32_t y = NT_PAD + font_h + NT_TITLE_GAP;
    for (uint8_t i = 0; i < e->line_count; i++) {
        dwm_text_draw(s, NT_TEXT_X, y, e->lines[i], strlen(e->lines[i]), text);
        y += line_height();
    }
}

static bool nt_proc(dwm_handle_t window, const dwm_event_t* ev, void* user) {
    (void)user;
    int index = find(window);

    switch (ev->type) {
        case DWM_EVENT_PAINT:
            if (index >= 0)
                paint(&entries[index], ev->surface);
            return true;

        case DWM_EVENT_MOUSE_DOWN:          // Any click dismisses
            if (index >= 0)
                dismiss(&entries[index]);
            return true;

        case DWM_EVENT_CONTEXT_MENU:        // No window menu on a notification
            return true;

        case DWM_EVENT_DESTROY:             // Slid out, or destroyed by someone else
            if (index >= 0) {
                remove_at((size_t)index);
                layout();
            }
            return true;
    }
    return false;
}

//
// Showing
//

static void keep_for_later(dwm_notify_kind_t kind, const char* title, const char* message,
                           uint32_t timeout_ms, uint32_t owner) {
    if (pending_count == NT_PENDING_MAX)
        return;
    nt_pending_t* p = &pending[pending_count++];
    p->kind       = kind;
    p->timeout_ms = timeout_ms;
    p->owner      = owner;
    strlcpy(p->title, title, sizeof(p->title));
    strlcpy(p->text, message, sizeof(p->text));
}

dwm_handle_t notify_show(dwm_notify_kind_t kind, const char* title, const char* message,
                         uint32_t timeout_ms, uint32_t owner) {
    if (title == NULL)   title = "";
    if (message == NULL) message = "";
    if (kind != DWM_NOTIFY_ERROR) kind = DWM_NOTIFY_INFO;

    if (!ready) {
        keep_for_later(kind, title, message, timeout_ms, owner);
        return 0;
    }

    bool sticky = (timeout_ms == DWM_NOTIFY_STICKY);
    uint64_t now = dwm_time_ms();

    nt_entry_t built;
    build(&built, kind, sticky, title, message);
    built.owner = owner;

    // Already showing: that one covers it, with a fresh timeout
    for (size_t i = 0; i < entry_count; i++) {
        nt_entry_t* e = &entries[i];
        if (!e->dismissing && same_text(e, &built)) {
            if (e->expires_ms != 0)
                e->expires_ms = sticky ? 0 : now + timeout_ms;
            return e->handle;
        }
    }

    // A program gets a few at a time: its oldest makes room
    if (owner != 0) {
        while (1) {
            nt_entry_t* oldest = NULL;
            size_t mine = 0;
            for (size_t i = 0; i < entry_count; i++) {
                if (entries[i].dismissing || entries[i].owner != owner)
                    continue;
                if (oldest == NULL) oldest = &entries[i];
                mine++;
            }
            if (mine < NT_MAX_PER_OWNER)
                break;
            dismiss(oldest);
        }
    }

    // Out of slots (many still sliding out): drop one outright, one already
    // sliding out first, then a timed one, then the oldest
    if (entry_count == NT_MAX_ENTRIES) {
        size_t victim = NT_MAX_ENTRIES;
        for (size_t i = 0; i < entry_count && victim == NT_MAX_ENTRIES; i++)
            if (entries[i].dismissing) victim = i;
        for (size_t i = 0; i < entry_count && victim == NT_MAX_ENTRIES; i++)
            if (entries[i].expires_ms != 0) victim = i;
        if (victim == NT_MAX_ENTRIES)
            victim = 0;
        dwm_handle_t gone = entries[victim].handle;
        remove_at(victim);          // First: its DESTROY then finds nothing
        layout();
        dwm_window_destroy(gone);
    }

    // Make room: push the oldest out until the new one fits, timed ones
    // first, so a sticky error is only pushed out when all showing are sticky
    int32_t avail = dwm_work_area().h - NT_MARGIN * 2;
    while (1) {
        nt_entry_t* oldest = NULL;
        nt_entry_t* oldest_timed = NULL;
        size_t active = 0;
        int32_t used = 0;
        for (size_t i = 0; i < entry_count; i++) {
            nt_entry_t* e = &entries[i];
            if (e->dismissing) continue;
            if (oldest == NULL) oldest = e;
            if (oldest_timed == NULL && e->expires_ms != 0) oldest_timed = e;
            active++;
            used += e->height + NT_GAP;
        }
        if (oldest == NULL)
            break;
        if (active < NT_MAX_VISIBLE && used + built.height <= avail)
            break;
        dismiss(oldest_timed != NULL ? oldest_timed : oldest);
    }

    // Top of the stack; the window starts just off the right edge and the
    // tick slides it in
    size_t index = entry_count++;
    entries[index] = built;
    layout();

    nt_entry_t* e = &entries[index];
    e->x = dwm_screen_width();
    e->y = (e->target_y < 0) ? 0 : e->target_y;
    e->expires_ms = sticky ? 0 : now + timeout_ms;

    dwm_window_desc_t desc = {
        .title      = e->title,
        .x          = e->x,
        .y          = e->y,
        .width      = NT_WIDTH,
        .height     = e->height,
        .min_width  = NT_WIDTH,
        .min_height = e->height,
        .style      = NT_STYLE,
        .proc       = nt_proc,
    };
    dwm_handle_t handle = dwm_window_create(&desc);
    if (handle == 0) {
        remove_at(entry_count - 1);
        layout();
        return 0;
    }
    entries[index].handle = handle;
    return handle;
}

//
// Main loop hooks (internal.h)
//

void notify_start(void) {
    font_h = dwm_text_height();     // dwm_init() has picked the font by now
    ready = true;

    size_t n = pending_count;
    pending_count = 0;
    for (size_t i = 0; i < n; i++)
        notify_show(pending[i].kind, pending[i].title, pending[i].text, pending[i].timeout_ms, pending[i].owner);
}

void notify_stop(void) {
    ready = false;
    entry_count = 0;
    pending_count = 0;
}

void notify_tick(void) {
    if (entry_count == 0)
        return;

    uint64_t now = dwm_time_ms();
    bool step = (now - last_step >= NT_FRAME_MS);
    if (step)
        last_step = now;

    dwm_handle_t done[NT_MAX_ENTRIES];
    size_t done_count = 0;

    for (size_t i = 0; i < entry_count; ) {
        nt_entry_t* e = &entries[i];

        // Destroyed without our procedure seeing it (shutdown, say)
        if (!dwm_window_exists(e->handle)) {
            remove_at(i);
            layout();
            continue;
        }

        if (!e->dismissing && e->expires_ms != 0 && now >= e->expires_ms)
            dismiss(e);

        if (step && moving(e)) {
            e->x = slide(e->x, e->target_x);
            e->y = slide(e->y, e->target_y);
            dwm_window_move(e->handle, e->x, e->y);
        }

        // Fully off screen: gone. DESTROY removes the entry.
        if (e->dismissing && !moving(e))
            done[done_count++] = e->handle;
        i++;
    }

    for (size_t i = 0; i < done_count; i++)
        dwm_window_destroy(done[i]);
}

int32_t notify_next_timeout(void) {
    if (entry_count == 0)
        return -1;

    uint64_t now = dwm_time_ms();
    int64_t best = -1;
    for (size_t i = 0; i < entry_count; i++) {
        const nt_entry_t* e = &entries[i];
        int64_t left = -1;
        if (moving(e) || (e->dismissing))
            left = (int64_t)(last_step + NT_FRAME_MS) - (int64_t)now;
        else if (e->expires_ms != 0)
            left = (int64_t)e->expires_ms - (int64_t)now;
        else
            continue;
        if (left < 0) left = 0;
        if (best < 0 || left < best)
            best = left;
    }
    return (int32_t)best;
}

//
// Public
//

dwm_handle_t dwm_notify(const char* title, const char* message, uint32_t timeout_ms) {
    return notify_show(DWM_NOTIFY_INFO, title, message, timeout_ms, 0);
}

dwm_handle_t dwm_notify_ex(dwm_notify_kind_t kind, const char* title, const char* message, uint32_t timeout_ms) {
    return notify_show(kind, title, message, timeout_ms, 0);
}

dwm_handle_t dwm_notifyf(dwm_notify_kind_t kind, const char* title, const char* fmt, ...) {
    char text[DWM_NOTIFY_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    uint32_t ms = (kind == DWM_NOTIFY_ERROR) ? DWM_NOTIFY_LONG_MS : DWM_NOTIFY_DEFAULT_MS;
    return notify_show(kind, title, text, ms, 0);
}

void dwm_notify_dismiss(dwm_handle_t notification) {
    int index = find(notification);
    if (index >= 0)
        dismiss(&entries[index]);
}

void dwm_notify_dismiss_all(void) {
    for (size_t i = 0; i < entry_count; i++)
        dismiss(&entries[i]);
}

size_t dwm_notify_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < entry_count; i++)
        n += !entries[i].dismissing;
    return n;
}

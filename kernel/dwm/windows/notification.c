#include <kernel/dwm/windows/notification.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/console/display.h>
#include <kernel/malloc.h>
#include <kernel/util/list.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>

// ==========================================
// Layout (client coordinates, 6x8 font)
// ==========================================
#define NT_WIDTH        240
#define NT_MARGIN       6       // Gap to the right screen edge and the taskbar
#define NT_GAP          6       // Gap between stacked notifications
#define NT_PAD          8
#define NT_ACCENT_W     3       // Colored strip on the left edge
#define NT_FONT_W       6
#define NT_FONT_H       8
#define NT_LINE_H       11
#define NT_TITLE_GAP    6       // Title baseline to first message line
#define NT_TEXT_X       (NT_ACCENT_W + NT_PAD)
#define NT_COLS         ((NT_WIDTH - NT_TEXT_X - NT_PAD) / NT_FONT_W)
#define NT_MAX_LINES    4
#define NT_MESSAGE_MAX  256

#define NT_MAX_VISIBLE  5       // Showing at once (oldest is pushed out)
#define NT_SLIDE_DIV    4       // Slide speed: 1/4 of the remaining distance per frame
#define NT_MAX_ENTRIES  12      // Showing + still sliding out

// Borderless, no titlebar, no close box, drawn above normal windows
#define NT_STYLE        (DWM_WSTYLE_TOPMOST | DWM_WSTYLE_NOBORDERS | DWM_WSTYLE_NOCLOSEBOX)

// ==========================================
// State (DWM lock)
// ==========================================
//
// nt_entries is ordered bottom to top: [0] sits on the taskbar, new
// notifications are appended and land on top of the stack.

struct NtEntry {
    WindowHandle handle;

    char     title[NT_COLS + 1];
    char     lines[NT_MAX_LINES][NT_COLS + 1];
    uint8_t  line_count;
    uint16_t height;

    int      x, y;                  // Where the window is now
    int      target_x, target_y;    // Where it is sliding to

    uint64_t expires_ms;            // 0 = until clicked
    bool     dismissing;            // Sliding out, no longer part of the stack
    bool     close_sent;
};

static struct NtEntry nt_entries[NT_MAX_ENTRIES];
static uint32_t       nt_count = 0;

static int nt_index(WindowHandle handle) {
    if (handle == 0) return -1;
    for (uint32_t i = 0; i < nt_count; i++) {
        if (nt_entries[i].handle == handle) return (int)i;
    }
    return -1;
}

static void nt_remove_at(uint32_t index) {
    if (index >= nt_count) return;
    memmove(&nt_entries[index], &nt_entries[index + 1],
            (nt_count - index - 1) * sizeof(struct NtEntry));
    nt_count--;
}

// ==========================================
// Text
// ==========================================

// Word-wrap `src` into lines of at most NT_COLS characters. Honours '\n',
// hard-breaks words longer than a line, and ends the last line with "..."
// when the text did not fit in NT_MAX_LINES.
static uint8_t nt_wrap(const char* src, char lines[NT_MAX_LINES][NT_COLS + 1]) {
    size_t  len   = strnlen(src, NT_MESSAGE_MAX);
    size_t  pos   = 0;
    uint8_t count = 0;

    while (pos < len && count < NT_MAX_LINES) {
        while (pos < len && src[pos] == ' ') pos++;
        if (pos >= len) break;

        size_t remaining = len - pos;
        size_t take;
        size_t next;

        size_t nl = 0;
        while (nl < remaining && nl <= NT_COLS && src[pos + nl] != '\n') nl++;
        bool has_newline = (nl < remaining && nl <= NT_COLS && src[pos + nl] == '\n');

        if (has_newline) {
            take = nl;
            next = pos + nl + 1;
        } else if (remaining <= NT_COLS) {
            take = remaining;
            next = len;
        } else {
            size_t brk = NT_COLS;
            while (brk > 0 && src[pos + brk] != ' ') brk--;
            if (brk == 0) brk = NT_COLS;    // One word longer than a line
            take = brk;
            next = pos + brk;
        }

        while (take > 0 && src[pos + take - 1] == ' ') take--;

        memcpy(lines[count], &src[pos], take);
        lines[count][take] = '\0';
        count++;
        pos = next;
    }

    // Skip trailing whitespace before deciding whether text was cut
    while (pos < len && (src[pos] == ' ' || src[pos] == '\n')) pos++;

    if (pos < len && count > 0) {
        char*  last = lines[count - 1];
        size_t n    = strlen(last);
        if (n + 3 > NT_COLS) n = NT_COLS - 3;
        memcpy(&last[n], "...", 4);
    }

    return count;
}

static void nt_build(struct NtEntry* e, const char* title, const char* message) {
    memset(e, 0, sizeof(struct NtEntry));

    // Title: one line, newlines flattened, "..." when cut
    size_t title_len = strnlen(title, DWM_MAX_TITLE_LEN);
    size_t n = (title_len > NT_COLS) ? NT_COLS : title_len;
    memcpy(e->title, title, n);
    e->title[n] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (e->title[i] == '\n') e->title[i] = ' ';
    }
    if (title_len > NT_COLS) memcpy(&e->title[NT_COLS - 3], "...", 4);

    e->line_count = nt_wrap(message, e->lines);

    int h = (NT_PAD * 2) + NT_FONT_H;
    if (e->line_count > 0)
        h += NT_TITLE_GAP + (e->line_count * NT_LINE_H) - (NT_LINE_H - NT_FONT_H);
    e->height = (uint16_t)h;
}

// ==========================================
// Stack layout and movement
// ==========================================

// Give every notification that is still in the stack its slot, bottom up
static void nt_layout(void) {
    int x      = display_get_width() - NT_WIDTH - NT_MARGIN;
    int bottom = display_get_height() - taskbar.height - NT_MARGIN;

    for (uint32_t i = 0; i < nt_count; i++) {
        struct NtEntry* e = &nt_entries[i];
        if (e->dismissing) continue;

        e->target_x = x;
        e->target_y = bottom - e->height;
        bottom = e->target_y - NT_GAP;
    }
}

// Take it out of the stack: it slides off to the right while the ones
// above it slide down into its place
static void nt_dismiss(struct NtEntry* e) {
    if (e->dismissing) return;
    e->dismissing = true;
    e->target_x = display_get_width();
    nt_layout();
}

static void nt_move(struct WindowObject* window, struct NtEntry* e, int x, int y) {
    e->x = x;
    e->y = y;
    dwm_window_place(window, x, y);
}

// A click focused the notification (dwm_handle_window_clicks moves the
// clicked window to the tail). Hand focus back to the window that had it,
// which is the closest normal window below it in the z-order.
static void nt_release_focus(struct WindowObject* window) {
    if (workspace.window_tail == NULL || workspace.window_tail->data != window) return;

    for (struct list_node* node = workspace.window_tail->prev; node != NULL; node = node->prev) {
        struct WindowObject* w = (struct WindowObject*)node->data;
        if (w->parent != NULL) continue;
        if (w->id == taskbar.window) continue;
        if (nt_index(w->id) >= 0) continue;

        dwm_set_focus(w);
        return;
    }

    dwm_set_desktop_focus();
}

// ==========================================
// Drawing and events (DWM thread, DWM lock held)
// ==========================================

static void nt_draw(struct WindowObject* window, const struct NtEntry* e) {
    int w = window->w;
    int h = window->h;

    dwm_draw_rect_filled(0, 0, w, h, theme.client.background);
    dwm_draw_rect_filled(0, 0, NT_ACCENT_W, h, theme.client.accent);
    dwm_draw_rect(0, 0, w, h, theme.button.border);

    dwm_draw_text(NT_TEXT_X, NT_PAD, e->title, theme.client.text_value);

    int y = NT_PAD + NT_FONT_H + NT_TITLE_GAP;
    for (uint8_t i = 0; i < e->line_count; i++) {
        dwm_draw_text(NT_TEXT_X, y, e->lines[i], theme.client.text_soft);
        y += NT_LINE_H;
    }
}

static void callback_notification_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam) {
    (void)wparam;
    int index = nt_index(handle);

    switch (event) {
        case DWM_EVENT_REDRAW: {
            struct WindowObject* window = dwm_get_window_by_id(handle);
            if (window != NULL && index >= 0) nt_draw(window, &nt_entries[index]);
            break;
        }

        // Any click dismisses
        case DWM_EVENT_MOUSE: {
            if (index < 0) break;
            if (!(lparam & (DWM_STATE_MOUSE_BTN_LEFT | DWM_STATE_MOUSE_BTN_RIGHT))) break;

            struct WindowObject* window = dwm_get_window_by_id(handle);
            if (window != NULL) nt_release_focus(window);
            nt_dismiss(&nt_entries[index]);
            break;
        }

        // Gone (slid out, or closed by someone else): the rest close the gap
        case DWM_EVENT_DESTROY:
            if (index >= 0) {
                nt_remove_at((uint32_t)index);
                nt_layout();
            }
            break;

        default:
            break;
    }
}

// Once per frame from dwm_update: timeouts, sliding, closing what slid out
void dwm_notification_tick(void) {
    if (nt_count == 0) return;

    uint64_t now = timer_get_ms();

    for (uint32_t i = 0; i < nt_count; ) {
        struct NtEntry* e = &nt_entries[i];

        // Destroyed without a DESTROY event (dwm_destroy_window directly)
        struct WindowObject* window = dwm_get_window_by_id(e->handle);
        if (window == NULL) {
            nt_remove_at(i);
            nt_layout();
            continue;
        }

        if (!e->dismissing && e->expires_ms != 0 && now >= e->expires_ms)
            nt_dismiss(e);

        int nx = dwm_slide_step(e->x, e->target_x, NT_SLIDE_DIV);
        int ny = dwm_slide_step(e->y, e->target_y, NT_SLIDE_DIV);
        if (nx != e->x || ny != e->y) nt_move(window, e, nx, ny);

        // Fully off screen: close it. DESTROY removes the entry next frame.
        if (e->dismissing && !e->close_sent && e->x == e->target_x) {
            e->close_sent = true;
            dwm_window_send_event(e->handle, DWM_EVENT_CLOSE);
        }

        i++;
    }
}

// ==========================================
// Public API
// ==========================================

WindowHandle dwm_notify(const char* title, const char* message, uint32_t timeout_ms) {
    DWM_GUARD();
    if (title == NULL)   title = "";
    if (message == NULL) message = "";

    // Out of slots (lots still sliding out): drop the oldest outright
    if (nt_count >= NT_MAX_ENTRIES) {
        WindowHandle oldest = nt_entries[0].handle;
        nt_remove_at(0);
        if (oldest != 0) dwm_destroy_window(oldest);
    }

    struct NtEntry built;
    nt_build(&built, title, message);

    // Make room: push the oldest out until the new one fits
    int avail = display_get_height() - taskbar.height - (NT_MARGIN * 2);
    while (1) {
        struct NtEntry* oldest = NULL;
        uint32_t active = 0;
        int used = 0;

        for (uint32_t i = 0; i < nt_count; i++) {
            struct NtEntry* e = &nt_entries[i];
            if (e->dismissing) continue;
            if (oldest == NULL) oldest = e;
            active++;
            used += e->height + NT_GAP;
        }

        if (oldest == NULL) break;
        if (active < NT_MAX_VISIBLE && used + built.height <= avail) break;
        nt_dismiss(oldest);
    }

    // Join the top of the stack, then create the window just off the right
    // edge of the screen; the tick slides it in
    struct NtEntry* e = &nt_entries[nt_count++];
    *e = built;
    nt_layout();

    int start_x = display_get_width();
    int start_y = (e->target_y < 0) ? 0 : e->target_y;

    WindowClass wclass;
    memset(&wclass, 0, sizeof(WindowClass));
    wclass.x          = (uint16_t)start_x;
    wclass.y          = (uint16_t)start_y;
    wclass.width      = NT_WIDTH;
    wclass.height     = e->height;
    wclass.max_width  = NT_WIDTH;
    wclass.max_height = e->height;
    strncpy(wclass.title, e->title, DWM_MAX_TITLE_LEN - 1);

    // dwm_allocate_window focuses the new window; remember who had focus
    struct WindowObject* prev_focus = dwm_get_focused_window();
    bool desktop_focus = workspace.desktop_focused;

    struct WindowObject* window = dwm_allocate_window(wclass, NT_STYLE, callback_notification_handler);
    if (window == NULL) {
        nt_remove_at(nt_count - 1);
        nt_layout();
        return 0;
    }

    e->handle     = window->id;
    e->x          = start_x;
    e->y          = start_y;
    e->expires_ms = (timeout_ms != 0) ? timer_get_ms() + timeout_ms : 0;

    // Give focus straight back. This happens before the next frame checks
    // focus, so nobody sees FOCUS_LOST / FOCUS_GAINED for it.
    if (desktop_focus)          dwm_set_desktop_focus();
    else if (prev_focus != NULL) dwm_set_focus(prev_focus);

    return e->handle;
}

struct NtPost {
    char     title[DWM_MAX_TITLE_LEN];
    char     message[NT_MESSAGE_MAX + 1];
    uint32_t timeout_ms;
};

static void nt_invoke(void* arg) {
    struct NtPost* p = (struct NtPost*)arg;
    dwm_notify(p->title, p->message, p->timeout_ms);
    free(p);
}

bool dwm_notify_post(const char* title, const char* message, uint32_t timeout_ms) {
    struct NtPost* p = (struct NtPost*)malloc(sizeof(struct NtPost));
    if (p == NULL) return false;

    memset(p, 0, sizeof(struct NtPost));
    if (title != NULL)   strncpy(p->title, title, sizeof(p->title) - 1);
    if (message != NULL) strncpy(p->message, message, sizeof(p->message) - 1);
    p->timeout_ms = timeout_ms;

    // dwm_invoke does not call fn on failure, so we still own p
    if (!dwm_invoke(nt_invoke, p)) {
        free(p);
        return false;
    }
    return true;
}

void dwm_notify_dismiss(WindowHandle handle) {
    DWM_GUARD();
    int index = nt_index(handle);
    if (index >= 0) nt_dismiss(&nt_entries[index]);
}

void dwm_notify_dismiss_all(void) {
    DWM_GUARD();
    for (uint32_t i = 0; i < nt_count; i++) nt_dismiss(&nt_entries[i]);
}

uint32_t dwm_notify_count(void) {
    DWM_GUARD();
    uint32_t count = 0;
    for (uint32_t i = 0; i < nt_count; i++) {
        if (!nt_entries[i].dismissing) count++;
    }
    return count;
}

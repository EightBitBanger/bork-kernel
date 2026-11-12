/*
 * Window server (server.h): client connections over the "sys.dwm" port,
 * and the window procedure every client window runs.
 *
 * Tray icons (tray.h) belong to the connection: the client_t is their
 * owner, so drop_client() takes them away with the windows when the
 * program hangs up, exits or crashes (the kernel closes its pipe then).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <os/font.h>
#include <os/gfx.h>
#include <os/ipc.h>
#include <os/security.h>
#include <os/wm_proto.h>

#include "dirty.h"
#include "compositor.h"
#include "internal.h"
#include "server.h"
#include "theme.h"
#include "text.h"
#include "timer.h"
#include "../resources/fontlib.h"

_Static_assert(WM_STYLE_TOPMOST == DWM_STYLE_TOPMOST && WM_STYLE_NOBORDER == DWM_STYLE_NOBORDER &&
               WM_STYLE_NOTITLEBAR == DWM_STYLE_NOTITLEBAR && WM_STYLE_NOCLOSEBOX == DWM_STYLE_NOCLOSEBOX &&
               WM_STYLE_RESIZABLE == DWM_STYLE_RESIZABLE && WM_STYLE_HIDDEN == DWM_STYLE_HIDDEN &&
               WM_STYLE_NOFOCUS == DWM_STYLE_NOFOCUS && WM_STYLE_NOMINIMIZE == DWM_STYLE_NOMINIMIZE,
               "WM_STYLE_* must match DWM_STYLE_*");
_Static_assert(WM_POS_DEFAULT == DWM_POS_DEFAULT, "WM_POS_DEFAULT must match DWM_POS_DEFAULT");
_Static_assert(WM_TITLE_MAX == DWM_TITLE_MAX, "WM_TITLE_MAX must match DWM_TITLE_MAX");
_Static_assert((WM_STYLE_CONTEXT_MENU & (DWM_STYLE_TOPMOST | DWM_STYLE_NOBORDER | DWM_STYLE_NOTITLEBAR |
               DWM_STYLE_NOCLOSEBOX | DWM_STYLE_RESIZABLE | DWM_STYLE_HIDDEN | DWM_STYLE_NOFOCUS |
               DWM_STYLE_NOMINIMIZE)) == 0,
               "WM_STYLE_CONTEXT_MENU is the protocol's own bit");
_Static_assert(WM_MENU_SEPARATOR == DWM_MENU_SEPARATOR && WM_MENU_DISABLED == DWM_MENU_DISABLED &&
               WM_MENU_CHECKED == DWM_MENU_CHECKED, "WM_MENU_* must match DWM_MENU_*");
_Static_assert(WM_MENU_MAX_ITEMS <= DWM_MENU_MAX_ITEMS && WM_MENU_LABEL_MAX <= DWM_MENU_LABEL_MAX,
               "a protocol menu must fit a DWM menu");
_Static_assert(sizeof(wm_theme_t) == sizeof(dwm_theme_t), "wm_theme_t mirrors dwm_theme_t");
_Static_assert(WM_NOTIFY_INFO == DWM_NOTIFY_INFO && WM_NOTIFY_ERROR == DWM_NOTIFY_ERROR &&
               WM_NOTIFY_STICKY == DWM_NOTIFY_STICKY, "WM_NOTIFY_* must match DWM_NOTIFY_*");
_Static_assert(WM_NOTIFY_TITLE_MAX <= DWM_NOTIFY_TITLE_MAX && WM_NOTIFY_TEXT_MAX <= DWM_NOTIFY_TEXT_MAX,
               "a protocol notification must fit a DWM one");
_Static_assert(sizeof(wm_clip_info_t) == sizeof(dwm_clip_info_t) && WM_CLIP_TEXT == DWM_CLIP_TEXT &&
               WM_CLIP_IMAGE == DWM_CLIP_IMAGE && WM_CLIP_BINARY == DWM_CLIP_BINARY &&
               WM_CLIP_FILES == DWM_CLIP_FILES && WM_CLIP_CUT == DWM_CLIP_CUT &&
               WM_CLIP_MAX == DWM_CLIP_MAX && WM_CLIP_FORMAT_MAX == DWM_CLIP_FORMAT_MAX,
               "wm_clip_info_t mirrors dwm_clip_info_t");
_Static_assert(WM_TRAY_ICON_PX == DWM_TRAY_ICON_PX && WM_TRAY_ICON_MAX == DWM_TRAY_SOURCE_MAX &&
               WM_TRAY_TIP_MAX == DWM_TRAY_TIP_MAX && WM_TRAY_PER_PROCESS == DWM_TRAY_PER_PROCESS &&
               WM_TRAY_MAX == DWM_TRAY_MAX && WM_TRAY_MENU_MS == DWM_TRAY_MENU_MS &&
               WM_MENU_MAX_ITEMS <= DWM_TRAY_MENU_MAX, "WM_TRAY_* must match DWM_TRAY_*");
_Static_assert(WM_TRAY_PANEL_ROWS_MAX == DWM_TRAY_PANEL_ROWS_MAX && WM_TRAY_PANEL_HEAD_MAX == DWM_TRAY_PANEL_HEAD_MAX &&
               WM_TRAY_PANEL_TEXT_MAX == DWM_TRAY_PANEL_TEXT_MAX && WM_TRAY_PANEL_SIDE_MAX == DWM_TRAY_PANEL_SIDE_MAX &&
               WM_PANEL_ROW_LOCK == DWM_PANEL_ROW_LOCK && WM_PANEL_ROW_MUTED == DWM_PANEL_ROW_MUTED &&
               WM_PANEL_ROW_NOTE == DWM_PANEL_ROW_NOTE && WM_PANEL_NO_LEVEL == DWM_PANEL_NO_LEVEL &&
               WM_PANEL_ROW_CLICK == DWM_PANEL_ROW_CLICK && WM_PANEL_ROW_ACTIVE == DWM_PANEL_ROW_ACTIVE &&
               WM_PANEL_ACCEPT == DWM_PANEL_ACCEPT && WM_PANEL_CANCEL == DWM_PANEL_CANCEL &&
               WM_PANEL_REFRESH == DWM_PANEL_REFRESH && WM_PANEL_DISCONNECT == DWM_PANEL_DISCONNECT &&
               WM_PANEL_BUSY == DWM_PANEL_BUSY && WM_PANEL_HAS_REFRESH == DWM_PANEL_HAS_REFRESH &&
               WM_PANEL_HAS_DISCONNECT == DWM_PANEL_HAS_DISCONNECT &&
               WM_TRAY_TEXT_MAX == DWM_TRAY_TEXT_MAX,
               "WM_TRAY_PANEL_* must match DWM_TRAY_PANEL_*");

#define OUTBOX_MAX          128     // Messages waiting for a client's pipe
#define REQUESTS_PER_ROUND  64      // So one busy client can't stall the others
#define BUFFER_MAX_DIM      SYS_GFX_MAX_DIM     // What one PIXELS command can draw

typedef struct {
    bool         used;
    int          pipe;
    uint32_t     pid;               // Who connected (for messages)
    bool         closing;           // Being torn down: send it nothing more
    size_t       out_count;
    uint32_t     dropped;           // Events lost because the outbox was full
    wm_message_t outbox[OUTBOX_MAX];
    int          attach[OUTBOX_MAX];// Handle sent with that message (closed once sent), 0 = none
} client_t;

typedef struct {
    bool         used;
    dwm_handle_t window;
    client_t*    owner;
    bool         silent;            // Destroyed on the client's request: no DESTROYED
    bool         menus;             // WM_STYLE_CONTEXT_MENU: the client opens its own menus
    int          shm;               // Attached buffer, 0 = none
    uint32_t*    pixels;
    int32_t      buf_w, buf_h;
} remote_t;

static int      port = -1;
static wm_theme_t sent_theme;       // The colors clients were last told about
static uint32_t sent_clip;          // The clipboard serial clients were last told about
static bool     font_changed;       // The app font changed: tell everyone this round
static bool     fs_changed;         // A client said files changed: tell everyone this round
static bool     settings_changed;   // A client said the desktop settings changed: tell the desktop
static client_t* drop_reader;       // Who got the last drop (WM_REQ_DROP_GET) ...
static uint32_t  drop_serial;       // ... and which one

// Tables of slots that grow (doubling) up to SERVER_MAX_CLIENTS and
// SERVER_MAX_WINDOWS. Each slot is allocated with the table and stays put:
// windows and clients point at each other.
static client_t** clients;
static size_t     client_slots;
static remote_t** remotes;
static size_t     remote_slots;

// One more slot in each table, doubling it up to its maximum. False when
// full or out of memory (the table is unchanged then).
#define GROW_SLOTS(name, type, table, count, max)                           \
    static bool name(void) {                                                \
        if (count >= (max))                                                 \
            return false;                                                   \
        size_t grow = count ? count * 2 : 4;                                \
        if (grow > (max))                                                   \
            grow = (max);                                                   \
        type** t = (type**)realloc(table, grow * sizeof(type*));            \
        if (t == NULL)                                                      \
            return false;                                                   \
        table = t;                                                          \
        size_t n = count;                                                   \
        while (n < grow && (t[n] = (type*)calloc(1, sizeof(type))) != NULL) \
            n++;                        /* Keep the ones that worked */     \
        if (n == count)                                                     \
            return false;                                                   \
        count = n;                                                          \
        return true;                                                        \
    }

GROW_SLOTS(grow_clients, client_t, clients, client_slots, SERVER_MAX_CLIENTS)
GROW_SLOTS(grow_remotes, remote_t, remotes, remote_slots, SERVER_MAX_WINDOWS)

//
// Outboxes
//

static bool droppable(const wm_message_t* m) {
    return m->kind == WM_MSG_EVENT && m->event.type != WM_EVENT_DROP;    // Files would go nowhere
}

// Take message `i` out of the outbox (closing a handle it would have carried)
static void outbox_remove(client_t* c, size_t i) {
    if (c->attach[i] > 0)
        ipc_close(c->attach[i]);
    memmove(&c->outbox[i], &c->outbox[i + 1], (c->out_count - i - 1) * sizeof(wm_message_t));
    memmove(&c->attach[i], &c->attach[i + 1], (c->out_count - i - 1) * sizeof(int));
    c->out_count--;
}

// Queue a message for a client, optionally with a handle to send along (the
// outbox owns it from here: it is closed once sent, or if the message goes).
// Mouse moves, moves and resizes of one window replace the one queued just
// before (wheel turns add to it); when full, the oldest mouse move goes (then the oldest event of any
// kind), never a reply.
static void outbox_push_attach(client_t* c, const wm_message_t* m, int attach) {
    if (c->closing) {
        if (attach > 0)
            ipc_close(attach);
        return;
    }

    if (m->kind == WM_MSG_EVENT && c->out_count > 0) {
        wm_message_t* last = &c->outbox[c->out_count - 1];
        uint32_t t = m->event.type;
        if (last->kind == WM_MSG_EVENT && last->event.type == t && last->event.window == m->event.window &&
            (t == WM_EVENT_MOUSE_MOVE || t == WM_EVENT_MOVED || t == WM_EVENT_RESIZED ||
             t == WM_EVENT_FS_CHANGED || t == WM_EVENT_DRAG_OVER) &&
            last->event.buttons == m->event.buttons) {
            *last = *m;
            return;
        }
        // Wheel turns in a row add up (same keys held), so a slow reader
        // still scrolls the whole way
        if (last->kind == WM_MSG_EVENT && t == WM_EVENT_MOUSE_WHEEL && last->event.type == t &&
            last->event.window == m->event.window && last->event.mods == m->event.mods &&
            last->event.buttons == m->event.buttons) {
            int32_t sum = (int32_t)last->event.wheel + m->event.wheel;
            int16_t wheel = (int16_t)(sum < -32768 ? -32768 : sum > 32767 ? 32767 : sum);
            *last = *m;
            last->event.wheel = wheel;
            return;
        }
    }

    if (c->out_count == OUTBOX_MAX) {
        size_t victim = OUTBOX_MAX;
        for (size_t i = 0; i < c->out_count && victim == OUTBOX_MAX; i++)
            if (c->outbox[i].kind == WM_MSG_EVENT && c->outbox[i].event.type == WM_EVENT_MOUSE_MOVE)
                victim = i;
        for (size_t i = 0; i < c->out_count && victim == OUTBOX_MAX; i++)
            if (droppable(&c->outbox[i]))
                victim = i;
        if (victim == OUTBOX_MAX && droppable(m)) {
            c->dropped++;
            if (attach > 0)
                ipc_close(attach);
            return;
        }
        if (victim == OUTBOX_MAX)
            victim = 0;         // All replies: the client is not reading at all
        outbox_remove(c, victim);
        c->dropped++;
    }
    c->attach[c->out_count] = attach;
    c->outbox[c->out_count++] = *m;
}

static void outbox_push(client_t* c, const wm_message_t* m) {
    outbox_push_attach(c, m, 0);
}

static void reply_with(client_t* c, uint32_t seq, int status, dwm_handle_t window, const wm_theme_t* theme) {
    wm_message_t m;
    memset(&m, 0, sizeof(m));
    m.kind   = WM_MSG_REPLY;
    m.seq    = seq;
    m.status = status;
    m.window = window;
    if (theme != NULL)
        m.theme = *theme;
    outbox_push(c, &m);
}

static void reply(client_t* c, uint32_t seq, int status, dwm_handle_t window) {
    reply_with(c, seq, status, window, NULL);
}

static void current_theme(wm_theme_t* out) {
    memcpy(out, dwm_theme(), sizeof(*out));
}

static void flush_client(client_t* c) {
    size_t sent = 0;
    while (sent < c->out_count) {
        if (ipc_send(c->pipe, &c->outbox[sent], sizeof(wm_message_t), c->attach[sent]) < 0)
            break;      // EAGAIN: the rest waits; EPIPE: poll notices the hangup
        if (c->attach[sent] > 0)
            ipc_close(c->attach[sent]);     // The client has its own handle now
        c->attach[sent] = 0;
        sent++;
    }
    if (sent > 0) {
        memmove(c->outbox, c->outbox + sent, (c->out_count - sent) * sizeof(wm_message_t));
        memmove(c->attach, c->attach + sent, (c->out_count - sent) * sizeof(int));
        c->out_count -= sent;
        // What was sent doesn't stay behind (a prompt's answer may be a password)
        volatile uint8_t* v = (volatile uint8_t*)&c->outbox[c->out_count];
        for (size_t i = 0; i < sent * sizeof(wm_message_t); i++)
            v[i] = 0;
    }
}

//
// Client windows
//

static remote_t* remote_of(client_t* c, dwm_handle_t window) {
    for (size_t i = 0; i < remote_slots; i++) {
        if (remotes[i]->used && remotes[i]->window == window && remotes[i]->owner == c)
            return remotes[i];
    }
    return NULL;
}

static void detach_buffer(remote_t* r) {
    if (r->shm > 0)
        ipc_close(r->shm);      // Unmaps it; the memory goes with the client's handle
    r->shm = 0;
    r->pixels = NULL;
    r->buf_w = r->buf_h = 0;
}

static void forward(remote_t* r, const dwm_event_t* ev, uint32_t type) {
    wm_message_t m;
    memset(&m, 0, sizeof(m));
    m.kind = WM_MSG_EVENT;
    m.event.type    = type;
    m.event.window  = r->window;
    m.event.x       = ev->x;
    m.event.y       = ev->y;
    m.event.rect_x  = ev->rect.x;
    m.event.rect_y  = ev->rect.y;
    m.event.rect_w  = ev->rect.w;
    m.event.rect_h  = ev->rect.h;
    m.event.time_ms = ev->time_ms;
    m.event.key     = ev->key;
    m.event.buttons = ev->buttons;
    m.event.button  = ev->button;
    m.event.clicks  = ev->clicks;
    m.event.mods    = ev->mods;
    m.event.wheel   = ev->wheel;
    m.event.param   = ev->param[0];
    outbox_push(r->owner, &m);
}

static void paint(remote_t* r, const dwm_event_t* ev) {
    if (r->pixels == NULL)
        return;     // Nothing attached yet: the theme's client background shows
    gfx_rect_t a = rect_intersect(ev->rect, gfx_rect(0, 0, r->buf_w, r->buf_h));
    if (rect_empty(a))
        return;
    // Origin is the client's top-left and the clip the dirty part (compositor.c).
    // Runs at once, so the kernel reads the shared buffer before this returns.
    gfx_draw_pixels(ev->surface, r->pixels + (size_t)a.y * (size_t)r->buf_w + (size_t)a.x,
                    a.w, a.h, r->buf_w, a.x, a.y);
}

static bool remote_proc(dwm_handle_t window, const dwm_event_t* ev, void* user) {
    remote_t* r = (remote_t*)user;
    (void)window;

    switch (ev->type) {
        case DWM_EVENT_PAINT:        paint(r, ev);                               return true;
        case DWM_EVENT_CLOSE:        forward(r, ev, WM_EVENT_CLOSE);             return true;
        case DWM_EVENT_MOUSE_MOVE:   forward(r, ev, WM_EVENT_MOUSE_MOVE);        return true;
        case DWM_EVENT_MOUSE_DOWN:   forward(r, ev, WM_EVENT_MOUSE_DOWN);        return true;
        case DWM_EVENT_MOUSE_UP:     forward(r, ev, WM_EVENT_MOUSE_UP);          return true;
        case DWM_EVENT_MOUSE_ENTER:  forward(r, ev, WM_EVENT_MOUSE_ENTER);       return true;
        case DWM_EVENT_MOUSE_LEAVE:  forward(r, ev, WM_EVENT_MOUSE_LEAVE);       return true;
        case DWM_EVENT_MOUSE_WHEEL:  forward(r, ev, WM_EVENT_MOUSE_WHEEL);       return true;
        case DWM_EVENT_DRAG_OVER:    forward(r, ev, WM_EVENT_DRAG_OVER);         return true;
        case DWM_EVENT_KEY:          forward(r, ev, WM_EVENT_KEY);               return true;
        case DWM_EVENT_FOCUS_GAINED: forward(r, ev, WM_EVENT_FOCUS_GAINED);      return true;
        case DWM_EVENT_FOCUS_LOST:   forward(r, ev, WM_EVENT_FOCUS_LOST);        return true;
        case DWM_EVENT_MOVED:        forward(r, ev, WM_EVENT_MOVED);             return true;
        case DWM_EVENT_RESIZED:      forward(r, ev, WM_EVENT_RESIZED);           return true;
        case DWM_EVENT_SHOW:         forward(r, ev, WM_EVENT_SHOW);              return true;
        case DWM_EVENT_HIDE:         forward(r, ev, WM_EVENT_HIDE);              return true;
        case DWM_EVENT_MENU_COMMAND: forward(r, ev, WM_EVENT_MENU_COMMAND);      return true;

        case DWM_EVENT_DROP: {
            // Files let go over the window: the client fetches the paths (DROP_GET)
            drop_reader = r->owner;
            drop_serial = ev->param[1];
            dwm_event_t e = *ev;
            e.param[0] = ev->param[1];          // WM_EVENT_DROP's param: the serial
            forward(r, &e, WM_EVENT_DROP);
            return true;
        }

        case DWM_EVENT_CONTEXT_MENU:
            // A client with menus of its own gets client-area right clicks;
            // the frame (and every other client) keeps the default window menu
            if (!r->menus || ev->param[0] != DWM_CONTEXT_CLIENT)
                return false;
            forward(r, ev, WM_EVENT_CONTEXT_MENU);
            return true;

        case DWM_EVENT_DESTROY:
            // The one place a client window's record goes, however it ends
            if (!r->silent)
                forward(r, ev, WM_EVENT_DESTROYED);
            detach_buffer(r);
            memset(r, 0, sizeof(*r));
            return true;
    }
    return false;
}

//
// Requests
//

static int do_create(client_t* c, const wm_request_t* q, dwm_handle_t* out) {
    if (q->w <= 0 || q->h <= 0 || q->w > GFX_SIZE_MAX / 2 || q->h > GFX_SIZE_MAX / 2)
        return EINVAL;
    if (q->style & ~WM_STYLE_VALID)
        return EINVAL;

    remote_t* r = NULL;
    for (size_t i = 0; i < remote_slots && r == NULL; i++)
        if (!remotes[i]->used)
            r = remotes[i];
    if (r == NULL) {
        size_t at = remote_slots;
        if (!grow_remotes())
            return remote_slots >= SERVER_MAX_WINDOWS ? EMFILE : ENOMEM;
        r = remotes[at];
    }

    memset(r, 0, sizeof(*r));
    r->used  = true;
    r->owner = c;

    char title[WM_TITLE_MAX];
    memcpy(title, q->title, sizeof(title));
    title[sizeof(title) - 1] = '\0';

    dwm_window_desc_t desc = {
        .title = title, .x = q->x, .y = q->y, .width = q->w, .height = q->h,
        .min_width = q->min_w, .min_height = q->min_h, .style = q->style & ~WM_STYLE_CONTEXT_MENU,
        .proc = remote_proc, .user = r,
    };
    dwm_handle_t win = dwm_window_create(&desc);
    if (win == 0) {
        memset(r, 0, sizeof(*r));
        return ENOMEM;
    }
    r->window = win;
    r->menus  = (q->style & WM_STYLE_CONTEXT_MENU) != 0;
    *out = win;
    return 0;
}

static int do_attach(remote_t* r, const wm_request_t* q, int shm) {
    if (shm <= 0)
        return EINVAL;

    ipc_info_t info;
    if (ipc_info(shm, &info) < 0 || info.type != IPC_TYPE_SHM)
        return EINVAL;
    if (q->w <= 0 || q->h <= 0 || q->w > BUFFER_MAX_DIM || q->h > BUFFER_MAX_DIM)
        return EINVAL;
    if ((uint64_t)q->w * (uint64_t)q->h * 4u > info.size)
        return EINVAL;      // Smaller than it claims: painting would read past it

    uint32_t* pixels = (uint32_t*)shm_map(shm);
    if (pixels == NULL)
        return errno ? errno : ENOMEM;

    detach_buffer(r);
    r->shm    = shm;
    r->pixels = pixels;
    r->buf_w  = q->w;
    r->buf_h  = q->h;
    dwm_window_invalidate(r->window, NULL);
    return 0;
}

// The app font for WM_REQ_FONT: its file in a fresh shm block per program
// (like the clipboard), which the program copies and closes. ENOENT when
// there is none, and the program keeps the built-in font.
static void reply_font(client_t* c, uint32_t seq) {
    wm_message_t m;
    memset(&m, 0, sizeof(m));
    m.kind = WM_MSG_REPLY;
    m.seq  = seq;

    int32_t px;
    font_t* font = dwm_text_app_font(&px);
    size_t size = 0;
    const void* bytes = (font != NULL) ? font_bytes(font, &size) : NULL;
    int attach = 0;
    if (bytes == NULL || size == 0 || size > SHM_MAX) {
        m.status = ENOENT;
    } else {
        attach = shm_create(size);
        void* p = (attach > 0) ? shm_map(attach) : NULL;
        if (p == NULL) {
            if (attach > 0)
                ipc_close(attach);
            attach = 0;
            m.status = ENOMEM;
        } else {
            memcpy(p, bytes, size);
            shm_unmap(attach);
            const char* name = fontlib_name_of(font);
            m.font.px   = px;
            m.font.size = (uint32_t)size;
            strlcpy(m.font.name, name != NULL ? name : "", sizeof(m.font.name));
        }
    }
    outbox_push_attach(c, &m, attach);
}

// Carry out one request. `shm` is the handle that came with it (0: none);
// it is closed here unless it was taken.
static void handle_request(client_t* c, const wm_request_t* q, int shm) {
    int status = 0;
    dwm_handle_t window = q->window;
    bool answer = q->seq != 0;

    if (q->type == WM_REQ_CREATE) {
        window = 0;
        status = do_create(c, q, &window);
        answer = true;
    } else if (q->type == WM_REQ_THEME) {
        if (shm > 0)
            ipc_close(shm);
        wm_theme_t theme;
        current_theme(&theme);
        reply_with(c, q->seq, 0, 0, &theme);
        return;
    } else if (q->type == WM_REQ_FONT) {
        if (shm > 0)
            ipc_close(shm);
        if (q->seq != 0)
            reply_font(c, q->seq);
        return;
    } else if (q->type == WM_REQ_SETTINGS_CHANGED) {
        if (shm > 0)
            ipc_close(shm);
        settings_changed = true;        // Many in one round go out as one
        if (q->seq != 0)
            reply(c, q->seq, 0, 0);
        return;
    } else {
        remote_t* r = remote_of(c, q->window);
        if (r == NULL) {
            status = EBADF;
        } else {
            switch (q->type) {
                case WM_REQ_DESTROY:
                    r->silent = true;
                    dwm_window_destroy(r->window);
                    break;
                case WM_REQ_ATTACH:
                    status = do_attach(r, q, shm);
                    if (status == 0)
                        shm = 0;        // Kept
                    break;
                case WM_REQ_COMMIT: {
                    gfx_rect_t a = rect_intersect(gfx_rect(q->x, q->y, q->w, q->h),
                                                  gfx_rect(0, 0, r->buf_w, r->buf_h));
                    if (!rect_empty(a))
                        dwm_window_invalidate(r->window, &a);
                    break;
                }
                case WM_REQ_SET_TITLE: {
                    char title[WM_TITLE_MAX];
                    memcpy(title, q->title, sizeof(title));
                    title[sizeof(title) - 1] = '\0';
                    dwm_window_set_title(r->window, title);
                    break;
                }
                case WM_REQ_MOVE:   dwm_window_move(r->window, q->x, q->y);          break;
                case WM_REQ_RESIZE: dwm_window_resize(r->window, q->w, q->h);        break;
                case WM_REQ_SHOW:   dwm_window_show(r->window, q->style != 0);       break;
                case WM_REQ_FOCUS:
                    dwm_window_raise(r->window);
                    dwm_window_focus(r->window);
                    break;
                case WM_REQ_POINTER_LOCK:
                    if (!input_lock_pointer(r->window, q->style != 0))
                        status = EPERM;
                    break;
                default:
                    status = EINVAL;
                    break;
            }
        }
    }

    if (shm > 0)
        ipc_close(shm);
    if (answer)
        reply(c, q->seq, status, window);
}

// WM_REQ_MENU's submenus (wm_proto.h wm_menu_tree_t), built into dwm
// menus. Static: dwm_menu_show copies all of it, so it may be reused at
// the next request.
#define MENU_NODES  (WM_MENU_MAX_ITEMS + WM_MENU_TREE_MAX)

static char             menu_labels[MENU_NODES][WM_MENU_LABEL_MAX];
static dwm_menu_item_t  menu_top[WM_MENU_MAX_ITEMS];
static dwm_menu_item_t  menu_grouped[WM_MENU_TREE_MAX];     // Each submenu's items side by side
static dwm_menu_t       menu_subs[WM_MENU_TREE_MAX];

static void menu_fill(dwm_menu_item_t* it, char* label, uint32_t id, uint32_t flags, const char* text) {
    memset(it, 0, sizeof(*it));
    memcpy(label, text, WM_MENU_LABEL_MAX);
    label[WM_MENU_LABEL_MAX - 1] = '\0';
    it->label = label;
    it->id    = id;
    it->flags = flags & (WM_MENU_SEPARATOR | WM_MENU_DISABLED | WM_MENU_CHECKED);
}

// The request's items in menu_top, their submenus from `shm` (0: none).
// False when the block is missing or breaks the rules.
static bool menu_build(const wm_menu_request_t* q, int shm) {
    bool any = false;
    for (uint32_t i = 0; i < q->count; i++) {
        menu_fill(&menu_top[i], menu_labels[i], q->items[i].id, q->items[i].flags, q->items[i].label);
        any |= (q->items[i].flags & WM_MENU_SUBMENU) != 0;
    }
    if (!any)
        return true;
    if (shm <= 0)
        return false;

    ipc_info_t info;
    if (ipc_info(shm, &info) < 0 || info.type != IPC_TYPE_SHM || info.size < sizeof(wm_menu_tree_t))
        return false;
    const wm_menu_tree_t* tree = (const wm_menu_tree_t*)shm_map(shm);
    if (tree == NULL)
        return false;
    uint32_t n = tree->count;
    if (n == 0 || n > WM_MENU_TREE_MAX ||
        sizeof(wm_menu_tree_t) + (uint64_t)n * sizeof(wm_menu_tree_entry_t) > info.size)
        return false;

    // Copied out first: the client could change the block while we read it
    static wm_menu_tree_entry_t e[WM_MENU_TREE_MAX];
    memcpy(e, tree->items, n * sizeof(e[0]));
    uint32_t total = q->count + n;
    uint32_t flags_of[MENU_NODES];
    for (uint32_t i = 0; i < q->count; i++)
        flags_of[i] = q->items[i].flags;
    for (uint32_t j = 0; j < n; j++) {
        uint32_t self = q->count + j;
        if (e[j].parent >= self || !(flags_of[e[j].parent] & WM_MENU_SUBMENU))
            return false;
        flags_of[self] = e[j].flags;
    }

    // Where each item ends up: the request's in menu_top, a submenu's in
    // menu_grouped. A parent always comes before its items, so it has its
    // place when its submenu is laid out.
    dwm_menu_item_t* node[MENU_NODES];
    for (uint32_t i = 0; i < q->count; i++)
        node[i] = &menu_top[i];
    uint32_t placed = 0, subs = 0;
    for (uint32_t p = 0; p < total; p++) {
        if (!(flags_of[p] & WM_MENU_SUBMENU))
            continue;
        uint32_t start = placed;
        for (uint32_t j = 0; j < n; j++) {
            if (e[j].parent != p)
                continue;
            if (placed - start == WM_MENU_MAX_ITEMS)
                return false;
            node[q->count + j] = &menu_grouped[placed];
            menu_fill(&menu_grouped[placed], menu_labels[q->count + j], e[j].id, e[j].flags, e[j].label);
            placed++;
        }
        // An empty one still counts: its item shows dimmed
        menu_subs[subs] = (dwm_menu_t){ &menu_grouped[start], placed - start, NULL };
        node[p]->submenu = &menu_subs[subs++];
    }
    return true;
}

// WM_REQ_MENU: the dwm's own context menu, choices come back as MENU_COMMAND
static void handle_menu(client_t* c, const wm_menu_request_t* q, int shm) {
    int status = 0;
    remote_t* r = remote_of(c, q->window);
    if (r == NULL) {
        status = EBADF;
    } else if (q->count == 0 || q->count > WM_MENU_MAX_ITEMS || !menu_build(q, shm)) {
        status = EINVAL;
    } else {
        dwm_menu_t menu = { menu_top, q->count, NULL };
        if (!dwm_menu_show(r->window, &menu, q->x, q->y))
            status = EINVAL;
    }
    if (shm > 0)
        ipc_close(shm);         // Unmaps it
    if (q->seq != 0)
        reply(c, q->seq, status, q->window);
}

//
// Tray icons
//

static uint32_t sender_pid(const client_t* c) {
    return c->pid != 0 ? c->pid : 0xFFFFFFFFu;     // Never 0: that is the DWM
}

// The picture of a WM_REQ_TRAY_ADD / MODIFY, mapped from the attached block
// after checking it holds what the request says. NULL: EINVAL.
static const uint32_t* tray_picture(const wm_tray_request_t* q, int shm) {
    if (shm <= 0 || q->icon_w <= 0 || q->icon_h <= 0 ||
        q->icon_w > WM_TRAY_ICON_MAX || q->icon_h > WM_TRAY_ICON_MAX)
        return NULL;
    ipc_info_t info;
    if (ipc_info(shm, &info) < 0 || info.type != IPC_TYPE_SHM)
        return NULL;
    if ((uint64_t)q->icon_w * (uint64_t)q->icon_h * 4u > info.size)
        return NULL;        // Smaller than it claims: reading it would run past
    return (const uint32_t*)shm_map(shm);
}

// WM_REQ_TRAY_ADD / MODIFY / REMOVE. Owns `shm` (closed here: the tray
// keeps its own copy of the picture).
static void handle_tray(client_t* c, wm_tray_request_t* q, int shm) {
    int status = 0;
    const uint32_t* pixels = NULL;
    const char* tip = NULL;

    q->tip[sizeof(q->tip) - 1] = '\0';
    if (q->flags & ~(uint32_t)(WM_TRAY_ICON | WM_TRAY_TIP | WM_TRAY_PANEL | WM_TRAY_SECRET | WM_TRAY_CONNECT) ||
        ((q->flags & WM_TRAY_CONNECT) && q->type != WM_REQ_TRAY_PROMPT)) {
        status = EINVAL;
    } else if (q->type == WM_REQ_TRAY_PROMPT) {
        status = tray_panel_prompt(c, sender_pid(c), q->id, q->tip, (q->flags & WM_TRAY_SECRET) != 0,
                                   (q->flags & WM_TRAY_CONNECT) != 0);
    } else if (q->type == WM_REQ_TRAY_PROMPT_TEXT) {
        // The answer rides in the reply, then is gone from here
        wm_message_t m;
        memset(&m, 0, sizeof(m));
        m.kind   = WM_MSG_REPLY;
        m.seq    = q->seq;
        m.status = tray_panel_take_text(c, sender_pid(c), q->id, m.text, sizeof(m.text));
        if (shm > 0)
            ipc_close(shm);
        if (q->seq != 0)
            outbox_push(c, &m);
        volatile char* v = m.text;
        for (size_t i = 0; i < sizeof(m.text); i++)
            v[i] = 0;
        return;
    } else if (q->type == WM_REQ_TRAY_REMOVE) {
        status = tray_remove(c, sender_pid(c), q->id);
    } else {
        if (q->flags & WM_TRAY_ICON) {
            pixels = tray_picture(q, shm);
            if (pixels == NULL)
                status = EINVAL;
        }
        if (q->flags & WM_TRAY_TIP)
            tip = q->tip;
        if (status == 0 && q->type == WM_REQ_TRAY_ADD)
            status = (pixels == NULL) ? EINVAL
                                      : tray_add(c, sender_pid(c), q->id, pixels, q->icon_w, q->icon_h, tip);
        else if (status == 0)
            status = tray_modify(c, sender_pid(c), q->id, pixels, q->icon_w, q->icon_h, tip);
        if (status == 0 && (q->flags & WM_TRAY_PANEL))
            status = tray_set_panel(c, sender_pid(c), q->id);
    }

    if (shm > 0)
        ipc_close(shm);     // Unmaps it too
    if (q->seq != 0)
        reply(c, q->seq, status, 0);
}

// WM_REQ_TRAY_PANEL: a piece of what an icon's tray panel lists
static void handle_tray_panel(client_t* c, wm_tray_panel_request_t* q, int shm) {
    if (shm > 0)
        ipc_close(shm);
    int status;
    if (q->count > WM_TRAY_PANEL_ROWS ||
        (q->flags & ~(uint32_t)(WM_PANEL_BUSY | WM_PANEL_HAS_REFRESH | WM_PANEL_HAS_DISCONNECT))) {
        status = EINVAL;
    } else {
        q->title[sizeof(q->title) - 1]   = '\0';
        q->status[sizeof(q->status) - 1] = '\0';
        dwm_tray_row_t rows[WM_TRAY_PANEL_ROWS];
        for (uint32_t i = 0; i < q->count; i++) {
            memcpy(rows[i].text, q->rows[i].text, sizeof(rows[i].text));
            memcpy(rows[i].side, q->rows[i].side, sizeof(rows[i].side));
            rows[i].text[sizeof(rows[i].text) - 1] = '\0';
            rows[i].side[sizeof(rows[i].side) - 1] = '\0';
            rows[i].level = q->rows[i].level;
            rows[i].flags = q->rows[i].flags;
        }
        status = tray_panel_rows(c, sender_pid(c), q->id, q->title, q->status, q->flags,
                                 q->first, q->total, rows, q->count);
    }
    if (q->seq != 0)
        reply(c, q->seq, status, 0);
}

// WM_REQ_TRAY_MENU: a menu above one of the client's tray icons, right
// after a click on it (tray_menu says when), or, with x WM_TRAY_MENU_KEEP,
// one to keep and open here on every right click (tray_set_menu; no items:
// none kept)
static void handle_tray_menu(client_t* c, const wm_menu_request_t* q, int shm) {
    if (shm > 0)
        ipc_close(shm);

    bool keep = q->x == WM_TRAY_MENU_KEEP;
    int status;
    if ((q->count == 0 && !keep) || q->count > WM_MENU_MAX_ITEMS) {
        status = EINVAL;
    } else {
        char labels[WM_MENU_MAX_ITEMS][WM_MENU_LABEL_MAX];
        dwm_menu_item_t items[WM_MENU_MAX_ITEMS];
        memset(items, 0, sizeof(items));
        for (uint32_t i = 0; i < q->count; i++) {
            memcpy(labels[i], q->items[i].label, WM_MENU_LABEL_MAX);
            labels[i][WM_MENU_LABEL_MAX - 1] = '\0';
            items[i].label = labels[i];
            items[i].id    = q->items[i].id;
            items[i].flags = q->items[i].flags & (WM_MENU_SEPARATOR | WM_MENU_DISABLED | WM_MENU_CHECKED);
        }
        status = keep ? tray_set_menu(c, sender_pid(c), q->window, items, q->count)
                      : tray_menu(c, sender_pid(c), q->window, items, q->count);
    }
    if (q->seq != 0)
        reply(c, q->seq, status, 0);
}

// From the tray: a click on one of a client's icons ...
static void tray_clicked(void* owner, uint32_t id, const dwm_tray_click_t* click) {
    client_t* c = (client_t*)owner;
    if (!c->used || c->closing)
        return;
    wm_message_t m;
    memset(&m, 0, sizeof(m));
    m.kind = WM_MSG_EVENT;
    m.event.type    = WM_EVENT_TRAY;
    m.event.tray    = id;
    m.event.param   = id;
    m.event.x       = click->x;
    m.event.y       = click->y;
    m.event.rect_x  = click->rect.x;
    m.event.rect_y  = click->rect.y;
    m.event.rect_w  = click->rect.w;
    m.event.rect_h  = click->rect.h;
    m.event.button  = click->button;
    m.event.clicks  = click->clicks;
    m.event.time_ms = (uint32_t)dwm_time_ms();
    outbox_push(c, &m);
}

// ... something in its tray panel ...
static void tray_panel_used(void* owner, uint32_t id, uint32_t param) {
    client_t* c = (client_t*)owner;
    if (!c->used || c->closing)
        return;
    wm_message_t m;
    memset(&m, 0, sizeof(m));
    m.kind = WM_MSG_EVENT;
    m.event.type    = WM_EVENT_TRAY_PANEL;
    m.event.tray    = id;
    m.event.param   = param;
    m.event.time_ms = (uint32_t)dwm_time_ms();
    outbox_push(c, &m);
}

// ... and an item chosen from its tray menu
static void tray_command(void* owner, uint32_t id, uint32_t command) {
    client_t* c = (client_t*)owner;
    if (!c->used || c->closing)
        return;
    wm_message_t m;
    memset(&m, 0, sizeof(m));
    m.kind = WM_MSG_EVENT;
    m.event.type    = WM_EVENT_TRAY_COMMAND;
    m.event.tray    = id;
    m.event.param   = command;
    m.event.time_ms = (uint32_t)dwm_time_ms();
    outbox_push(c, &m);
}

// WM_REQ_NOTIFY: a notification for the program (at most a few at a time)
static void handle_notify(client_t* c, wm_notify_request_t* q) {
    q->title[sizeof(q->title) - 1] = '\0';
    q->text[sizeof(q->text) - 1]   = '\0';

    int status = 0;
    if (q->kind != WM_NOTIFY_INFO && q->kind != WM_NOTIFY_ERROR) {
        status = EINVAL;
    } else {
        uint32_t ms = q->timeout_ms;
        if (ms > WM_NOTIFY_MAX_MS)
            ms = WM_NOTIFY_MAX_MS;
        uint32_t owner = c->pid != 0 ? c->pid : 0xFFFFFFFFu;    // Never 0: that is the DWM
        if (notify_show((dwm_notify_kind_t)q->kind, q->title, q->text, ms, owner) == 0)
            status = ENOMEM;
    }
    if (q->seq != 0)
        reply(c, q->seq, status, 0);
}

// WM_REQ_CLIP_*: the system clipboard (clipboard.h), and WM_REQ_DRAG /
// WM_REQ_DROP_GET, which carry files the same way (drag.h). Owns `shm`.
static void handle_clip(client_t* c, wm_clip_request_t* q, int shm) {
    wm_message_t m;
    memset(&m, 0, sizeof(m));
    m.kind = WM_MSG_REPLY;
    m.seq  = q->seq;
    int status = 0, attach = 0;

    switch (q->type) {
        case WM_REQ_CLIP_SET: {
            dwm_clip_info_t info;
            memcpy(&info, &q->info, sizeof(info));
            info.format[DWM_CLIP_FORMAT_MAX - 1] = '\0';
            const void* bytes = NULL;
            if (info.size > 0) {
                // The data rides in the attached block; map it just long enough to copy
                bytes = (shm > 0 && shm_size(shm) >= info.size) ? shm_map(shm) : NULL;
                if (bytes == NULL)
                    status = EINVAL;
            }
            if (status == 0 && !clip_set_from(&info, bytes, c->pid != 0 ? c->pid : 0xFFFFFFFFu))
                status = errno ? errno : ENOMEM;
            break;
        }
        case WM_REQ_CLIP_GET: {
            dwm_clip_info_t info;
            if (!dwm_clip_info(&info)) {
                status = ENOENT;
                break;
            }
            memcpy(&m.clip, &info, sizeof(info));
            attach = clip_share();
            if (attach < 0) {
                status = ENOMEM;
                attach = 0;
            }
            break;
        }
        case WM_REQ_CLIP_INFO: {
            dwm_clip_info_t info;
            dwm_clip_info(&info);           // type NONE when empty: not an error
            memcpy(&m.clip, &info, sizeof(info));
            break;
        }
        case WM_REQ_CLIP_CLEAR:
            dwm_clip_clear(q->info.serial);
            break;
        case WM_REQ_DRAG: {
            // Only from the window the left button last went down in
            dwm_handle_t source = input_press_window();
            if (source == DWM_DESKTOP || remote_of(c, source) == NULL) {
                status = EPERM;
                break;
            }
            dwm_clip_info_t info;
            memcpy(&info, &q->info, sizeof(info));
            const void* bytes = (info.size > 0 && shm > 0 && shm_size(shm) >= info.size) ? shm_map(shm) : NULL;
            if (bytes == NULL)
                status = EINVAL;
            else if (!drag_set_from(&info, bytes, c->pid != 0 ? c->pid : 0xFFFFFFFFu))
                status = errno ? errno : ENOMEM;
            else if (!input_start_file_drag())
                status = EPERM;             // That press ended a while ago
            break;
        }
        case WM_REQ_DROP_GET: {
            dwm_clip_info_t info;
            if (drop_reader != c || q->info.serial != drop_serial ||
                dwm_drop_data(drop_serial, &info) == NULL) {
                status = ENOENT;
                break;
            }
            memcpy(&m.clip, &info, sizeof(info));
            attach = drag_share(drop_serial);
            if (attach < 0) {
                status = ENOMEM;
                attach = 0;
            }
            break;
        }
        default:
            status = EINVAL;
            break;
    }

    if (shm > 0)
        ipc_close(shm);                     // Unmaps it too: the DWM keeps its own copy
    m.status = status;
    if (q->seq != 0)
        outbox_push_attach(c, &m, attach);
    else if (attach > 0)
        ipc_close(attach);
}

//
// Connections
//

static void drop_client(client_t* c, const char* why) {
    if (why != NULL)
        dwm_notifyf(DWM_NOTIFY_ERROR, "Program dropped", "pid %u: %s", (unsigned)c->pid, why);
    c->closing = true;
    if (drop_reader == c)
        drop_reader = NULL;
    tray_remove_owner(c);                   // Its tray icons go with it
    for (size_t i = 0; i < c->out_count; i++)
        if (c->attach[i] > 0)
            ipc_close(c->attach[i]);        // Handles that never went out
    for (size_t i = 0; i < remote_slots; i++) {
        if (remotes[i]->used && remotes[i]->owner == c)
            dwm_window_destroy(remotes[i]->window);
    }
    ipc_close(c->pipe);
    memset(c, 0, sizeof(*c));
}

static void accept_clients(void) {
    int pipe;
    while ((pipe = ipc_accept(port)) > 0) {
        client_t* c = NULL;
        for (size_t i = 0; i < client_slots && c == NULL; i++)
            if (!clients[i]->used)
                c = clients[i];
        if (c == NULL) {
            size_t at = client_slots;
            if (grow_clients())
                c = clients[at];
        }
        if (c == NULL) {
            ipc_close(pipe);    // The client sees EPIPE before its welcome
            continue;
        }
        memset(c, 0, sizeof(*c));
        c->used = true;
        c->pipe = pipe;

        wm_message_t m;
        memset(&m, 0, sizeof(m));
        m.kind     = WM_MSG_WELCOME;
        m.version  = WM_PROTOCOL_VERSION;
        m.screen_w = compositor_width();
        m.screen_h = compositor_height();
        outbox_push(c, &m);
    }
}

// Where a window's drops go, for the drag plate (drag.h)
static void handle_drop_folder(client_t* c, wm_folder_request_t* q) {
    remote_t* r = remote_of(c, q->window);
    if (r != NULL) {
        q->path[sizeof(q->path) - 1] = '\0';
        dwm_set_drop_folder(r->window, q->path);
    }
    if (q->seq != 0)
        reply(c, q->seq, r != NULL ? 0 : EBADF, 0);
}

static void read_client(client_t* c) {
    union {
        wm_request_t        q;
        wm_menu_request_t   menu;
        wm_notify_request_t notify;
        wm_clip_request_t   clip;
        wm_path_request_t   fs;
        wm_folder_request_t folder;
        wm_tray_request_t   tray;
        wm_tray_panel_request_t panel;
        uint8_t           raw[IPC_MSG_MAX];
    } buf;

    for (int n = 0; n < REQUESTS_PER_ROUND && c->used; n++) {
        ipc_msg_info_t info;
        memset(&info, 0, sizeof(info));
        ssize_t len = ipc_recv(c->pipe, buf.raw, sizeof(buf.raw), &info);
        if (len < 0) {
            if (errno == EPIPE)
                drop_client(c, NULL);           // Hung up (or exited): normal
            else if (errno == EMFILE)
                drop_client(c, "too many handles");
            else if (errno != EAGAIN)
                drop_client(c, "connection lost");
            return;
        }
        c->pid = info.sender;
        if ((size_t)len == sizeof(wm_path_request_t) && buf.fs.type == WM_REQ_FS_CHANGED) {
            if (info.handle > 0)
                ipc_close(info.handle);
            fs_changed = true;          // Many in one round go out as one
            if (buf.fs.seq != 0)
                reply(c, buf.fs.seq, 0, 0);
            continue;
        }
        if ((size_t)len == sizeof(wm_folder_request_t) && buf.folder.type == WM_REQ_DROP_FOLDER) {
            if (info.handle > 0)
                ipc_close(info.handle);
            handle_drop_folder(c, &buf.folder);
            continue;
        }
        if ((size_t)len == sizeof(wm_clip_request_t) &&
            ((buf.clip.type >= WM_REQ_CLIP_SET && buf.clip.type <= WM_REQ_CLIP_CLEAR) ||
             buf.clip.type == WM_REQ_DRAG || buf.clip.type == WM_REQ_DROP_GET)) {
            handle_clip(c, &buf.clip, info.handle);
            continue;
        }
        if ((size_t)len == sizeof(wm_notify_request_t) && buf.notify.type == WM_REQ_NOTIFY) {
            if (info.handle > 0)
                ipc_close(info.handle);     // Nothing rides along with a notification
            handle_notify(c, &buf.notify);
            continue;
        }
        if ((size_t)len == sizeof(wm_menu_request_t) && buf.menu.type == WM_REQ_MENU) {
            handle_menu(c, &buf.menu, info.handle);
            continue;
        }
        if ((size_t)len == sizeof(wm_menu_request_t) && buf.menu.type == WM_REQ_TRAY_MENU) {
            handle_tray_menu(c, &buf.menu, info.handle);
            continue;
        }
        if ((size_t)len == sizeof(wm_tray_request_t) &&
            ((buf.tray.type >= WM_REQ_TRAY_ADD && buf.tray.type <= WM_REQ_TRAY_REMOVE) ||
             buf.tray.type == WM_REQ_TRAY_PROMPT || buf.tray.type == WM_REQ_TRAY_PROMPT_TEXT)) {
            handle_tray(c, &buf.tray, info.handle);
            continue;
        }
        if ((size_t)len == sizeof(wm_tray_panel_request_t) && buf.panel.type == WM_REQ_TRAY_PANEL) {
            handle_tray_panel(c, &buf.panel, info.handle);
            continue;
        }
        if ((size_t)len != sizeof(wm_request_t)) {
            if (info.handle > 0)
                ipc_close(info.handle);
            continue;       // Not a request: ignored
        }
        handle_request(c, &buf.q, info.handle);
    }
}

//
// Public
//

bool server_init(void) {
    if (!sec_has(SEC_IPC)) {
        dwm_notify_ex(DWM_NOTIFY_ERROR, "No window server", "Missing IPC clearance", DWM_NOTIFY_STICKY);
        return false;
    }
    if (port >= 0)
        return true;
    int p = ipc_port_create(WM_PORT_NAME);
    if (p < 0) {
        int err = errno;
        dwm_notifyf(DWM_NOTIFY_ERROR, "No window server", "%s: %s", WM_PORT_NAME, strerror(err));
        errno = err;
        return false;
    }
    port = p;
    tray_set_handlers(tray_clicked, tray_command);
    tray_set_panel_handler(tray_panel_used);
    current_theme(&sent_theme);
    sent_clip = dwm_clip_serial();
    return true;
}

size_t server_wait_items(ipc_wait_item_t* items, size_t max) {
    size_t n = 0;
    if (port < 0 || max == 0)
        return 0;
    items[n].handle = port;
    items[n].events = IPC_READABLE;
    items[n].ready  = 0;
    n++;
    for (size_t i = 0; i < client_slots && n < max; i++) {
        if (!clients[i]->used)
            continue;
        items[n].handle = clients[i]->pipe;
        items[n].events = IPC_READABLE | (clients[i]->out_count ? IPC_WRITABLE : 0);
        items[n].ready  = 0;
        n++;
    }
    return n;
}

void server_poll(void) {
    if (port < 0)
        return;
    accept_clients();
    for (size_t i = 0; i < client_slots; i++) {
        if (clients[i]->used)
            read_client(clients[i]);
    }
    server_flush();
}

// After dwm_theme_set(): tell every client once, so its windows can repaint
static void announce_theme(void) {
    wm_theme_t now;
    current_theme(&now);
    if (memcmp(&now, &sent_theme, sizeof(now)) == 0)
        return;
    sent_theme = now;

    wm_message_t m;
    memset(&m, 0, sizeof(m));
    m.kind = WM_MSG_EVENT;
    m.event.type = WM_EVENT_THEME_CHANGED;
    for (size_t i = 0; i < client_slots; i++) {
        if (clients[i]->used)
            outbox_push(clients[i], &m);
    }
}

// After the clipboard changed (here or in the DWM itself): tell every client once
static void announce_clip(void) {
    uint32_t now = dwm_clip_serial();
    if (now == sent_clip)
        return;
    sent_clip = now;

    wm_message_t m;
    memset(&m, 0, sizeof(m));
    m.kind = WM_MSG_EVENT;
    m.event.type  = WM_EVENT_CLIP_CHANGED;
    m.event.param = now;
    for (size_t i = 0; i < client_slots; i++) {
        if (clients[i]->used)
            outbox_push(clients[i], &m);
    }
}

void server_font_changed(void) {
    font_changed = true;
}

// The app font changed: every client once (as a theme change, which makes
// wm.h fetch the font again and the program repaint)
static void announce_font(void) {
    if (!font_changed)
        return;
    font_changed = false;

    wm_message_t m;
    memset(&m, 0, sizeof(m));
    m.kind = WM_MSG_EVENT;
    m.event.type = WM_EVENT_THEME_CHANGED;
    for (size_t i = 0; i < client_slots; i++) {
        if (clients[i]->used)
            outbox_push(clients[i], &m);
    }
}

void dwm_fs_changed(void) {
    fs_changed = true;
}

// Files changed: every client once, and the desktop procedure
static void announce_fs(void) {
    if (!fs_changed)
        return;
    fs_changed = false;

    wm_message_t m;
    memset(&m, 0, sizeof(m));
    m.kind = WM_MSG_EVENT;
    m.event.type = WM_EVENT_FS_CHANGED;
    for (size_t i = 0; i < client_slots; i++) {
        if (clients[i]->used)
            outbox_push(clients[i], &m);
    }
    dwm_post_simple(DWM_DESKTOP, DWM_EVENT_FS_CHANGED, 0, 0);
}

// Desktop settings changed: the desktop procedure reads them again
static void announce_settings(void) {
    if (!settings_changed)
        return;
    settings_changed = false;
    dwm_post_simple(DWM_DESKTOP, DWM_EVENT_SETTINGS_CHANGED, 0, 0);
}

void server_flush(void) {
    if (port >= 0) {
        announce_theme();
        announce_font();
        announce_clip();
        announce_fs();
        announce_settings();
    }
    for (size_t i = 0; i < client_slots; i++) {
        if (clients[i]->used && clients[i]->out_count > 0)
            flush_client(clients[i]);
    }
}

void server_shutdown(void) {
    for (size_t i = 0; i < client_slots; i++) {
        if (!clients[i]->used)
            continue;
        // Tell it its windows are going, then close
        for (size_t w = 0; w < remote_slots; w++) {
            if (remotes[w]->used && remotes[w]->owner == clients[i])
                dwm_window_destroy(remotes[w]->window);      // Queues DESTROYED
        }
        flush_client(clients[i]);
        drop_client(clients[i], NULL);
    }
    if (port >= 0)
        ipc_close(port);
    port = -1;
    for (size_t i = 0; i < client_slots; i++)
        free(clients[i]);
    for (size_t i = 0; i < remote_slots; i++)
        free(remotes[i]);
    free(clients);
    free(remotes);
    clients = NULL;
    remotes = NULL;
    client_slots = remote_slots = 0;
}


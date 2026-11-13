/*
 * audiosrv: the volume icon on the taskbar, as a small service.
 *
 * The desktop starts it (<home>/sys/services/audiosrv) like the Wi-Fi icon.
 * The icon is a speaker with one to three waves for the level, or a cross
 * while muted (grey with no sound device). Pointing at it says the level.
 *
 * A left click opens a small panel above the icon: an upright slider for
 * the master level and one beside it for each program playing through the
 * mixer (below), each with its level and a Mute box. Click or drag a
 * slider; the wheel, Up / Down, Page Up / Down, Home and End move the
 * chosen one (Left / Right or Tab choose), M or Space mutes it. Moving a
 * slider unmutes it. The codec's name is in the icon's tooltip. Esc, Enter,
 * another click on the icon or clicking anywhere else closes it. A right
 * click has Mute / Unmute for the master.
 *
 * The mixer (mixer.c): the service holds /dev/audio0 while anything plays
 * and adds up the programs' streams in user space, each scaled by its own
 * slider. libc's audio_open(0) connects to it by itself; without the
 * service a program gets the device directly, as before.
 *
 * The master level is the codec's own amplifier (os/audio.h,
 * audio_set_volume on /dev/audioctl0), so it changes whatever plays,
 * without touching the samples. The icon looks again every second, so a
 * level set elsewhere shows too. A program's level scales its samples in
 * the mixer.
 *
 * The master level is kept in the registry, hkey_user/control/audio
 * (volume, mute), and set again when the service starts, so it survives a
 * restart; each program's under .../audio/apps, by the name it shows.
 *
 * The microphone: while a program records (/dev/mic0, os/audio.h mic_*),
 * a red microphone icon sits beside the speaker, and pointing at it names
 * the program, as the kernel knows it. It looks twice a second and keeps
 * the icon at least MIC_HOLD_MS, so even a short recording shows.
 *
 * One at a time: a second copy finds the "audiosrv" port taken and stops.
 *
 * Permissions: file read and write (the registry and /dev/audio0 need
 * write), gfx and ipc. Anything more it was started with is dropped at once.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/audio.h>
#include <os/input.h>
#include <os/ipc.h>
#include <os/os.h>
#include <os/registry.h>
#include <os/security.h>
#include <os/wm.h>
#include "mixer.h"

#define ICON_ID             1
#define MIC_ICON_ID         2
#define MIC_POLL_MS         500         // Who records, looked at this often
#define MIC_HOLD_MS         3000        // The microphone icon stays at least this long
#define ICON_PX             16
#define INSTANCE_PORT       "audiosrv"
#define REG_KEY             "hkey_user/control/audio"

#define POLL_MS             1000        // The device's level read this often
#define RETRY_MS            5000        // No device yet: look again this often
#define SAVE_DELAY_MS       1000        // The registry written this long after the last change
#define FOCUS_GRACE_MS      300         // A focus loss this soon after showing is ignored

#define STEP                5           // Wheel notch / arrow key
#define PAGE                20          // Page Up / Down

// Panel layout
#define PAD                 10
#define TRACK_LEN           140         // Track length in pixels, 0% at the bottom
#define THUMB_W             19
#define THUMB_H             9
#define TRACK_W             4
#define BOX                 13          // The Mute box

#define STYLE_POPUP (WM_STYLE_NOBORDER | WM_STYLE_TOPMOST | WM_STYLE_NOMINIMIZE | WM_STYLE_HIDDEN)
#define SEC_KEEP    (SEC_FILE | SEC_GFX | SEC_IPC)

enum { CMD_MUTE = 1 };

#define COLS_MAX            (1 + MIXER_STREAMS)
#define COL_GAP             6
#define NAME_CELLS          10          // Column width in characters for the name

// One slider: the master (id 0) or a program's stream
typedef struct {
    uint32_t id;
    char     name[32];
    int      level;
    bool     mute;
    bool     playing;
} column_t;

typedef struct {
    int32_t cw, lh;
    int32_t w, h;
    int32_t cols;
    int32_t col_w;
    int32_t head_h;
    int32_t track_y, slider_y, slider_h;   // track_y: 100%; slider_*: the band that takes clicks
    int32_t level_y, level_h;
    int32_t mute_y, mute_h, mute_w;
    int32_t note_y, note_h;                // A line under it all while the device is busy
} layout_t;

static struct {
    int         instance;
    int         fd;                 // /dev/audioctl0, -1 while there is none
    audio_info_t info;
    bool        have_info;
    int         volume;             // 0..100
    bool        mute;
    uint64_t    poll_at;
    uint64_t    save_at;            // 0: nothing to save

    // The icon as last sent
    int         icon_state;         // -1: not sent yet
    char        tip[64];
    bool        menu_kept;          // The desktop opens the right-click menu itself
    bool        menu_mute;          // What it was sent with

    // The microphone icon
    bool        mic_shown;
    uint32_t    mic_starts;         // audio_capture_t.starts last seen
    bool        mic_have_starts;
    uint64_t    mic_poll_at;
    uint64_t    mic_until;          // Shown at least until then
    char        mic_tip[64];

    // The panel
    wm_window_t popup;
    bool        open;
    bool        drag;
    int         drag_col;
    int         sel;                // The column the keys move
    column_t    col[COLS_MAX];
    int         ncol;
    bool        mixer_on;
    uint64_t    shown_at;
    int32_t     anchor_x, anchor_y, anchor_w, anchor_h;
    int32_t     x, y;
    layout_t    lay;
    wm_theme_t  theme;
    bool        have_theme;
} as;

static uint64_t now(void) {
    return os_time_ms();
}

//
// The device
//

static bool device_open(void) {
    if (as.fd >= 0)
        return true;
    as.fd = audio_ctl_open(0);
    if (as.fd < 0)
        return false;
    as.have_info = audio_info(as.fd, &as.info) == 0;
    return true;
}

static void device_close(void) {
    if (as.fd >= 0)
        audio_close(as.fd);
    as.fd = -1;
    as.have_info = false;
}

// The device's level into as.volume / as.mute. False without a device.
static bool device_read(void) {
    if (!device_open())
        return false;
    audio_volume_t v;
    if (audio_get_volume(as.fd, &v) < 0) {
        device_close();         // Gone (or never right): open again later
        return false;
    }
    as.volume = (v.left > v.right) ? v.left : v.right;
    as.mute   = v.mute != 0;
    return true;
}

static void device_write(void) {
    if (!device_open())
        return;
    if (audio_set_volume(as.fd, as.volume, as.volume, as.mute) < 0)
        device_close();
}

//
// The registry
//

static void settings_load(void) {
    char text[16];
    bool changed = false;
    if (os_reg_get_string(REG_KEY, "volume", text, sizeof(text)) == 0) {
        int v = atoi(text);
        if (v >= 0 && v <= 100) {
            as.volume = v;
            changed = true;
        }
    }
    if (os_reg_get_string(REG_KEY, "mute", text, sizeof(text)) == 0) {
        as.mute = atoi(text) != 0;
        changed = true;
    }
    if (changed)
        device_write();
}

static void settings_save(void) {
    char text[16];
    os_reg_mkkey(REG_KEY);          // EEXIST after the first time
    snprintf(text, sizeof(text), "%d", as.volume);
    os_reg_set_string(REG_KEY, "volume", text);
    os_reg_set_string(REG_KEY, "mute", as.mute ? "1" : "0");
}

//
// The icon
//

static uint32_t opaque(uint32_t c) {
    return c | 0xFF000000u;
}

// a over b, `alpha` of 255
static uint32_t mix(uint32_t a, uint32_t b, uint32_t alpha) {
    uint32_t out = 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
        out |= ((ca * alpha + cb * (255 - alpha)) / 255) << shift;
    }
    return out;
}

// 0: no device, 1: muted (or 0 %), 2..4: one to three waves
static int icon_state(void) {
    if (as.fd < 0)
        return 0;
    if (as.mute || as.volume == 0)
        return 1;
    if (as.volume <= 33)
        return 2;
    if (as.volume <= 66)
        return 3;
    return 4;
}

static void put(uint32_t* pic, int x, int y, uint32_t c) {
    if (x >= 0 && x < ICON_PX && y >= 0 && y < ICON_PX)
        pic[y * ICON_PX + x] = c;
}

static void draw_icon(uint32_t* pic, int state) {
    memset(pic, 0, ICON_PX * ICON_PX * sizeof(uint32_t));
    uint32_t c = (state == 0) ? 0xFF808080u : 0xFFE8E8E8u;

    // The speaker: a box and a cone opening to the right
    for (int y = 6; y <= 9; y++)
        for (int x = 1; x <= 3; x++)
            put(pic, x, y, c);
    for (int x = 4; x <= 7; x++) {
        int spread = x - 3;             // 1..4
        for (int y = 6 - spread; y <= 9 + spread; y++)
            put(pic, x, y, c);
    }

    if (state == 1) {
        // A cross where the waves go
        uint32_t red = 0xFFE04848u;
        for (int i = 0; i < 5; i++) {
            put(pic, 10 + i, 5 + i, red);
            put(pic, 14 - i, 5 + i, red);
            put(pic, 10 + i, 6 + i, red);
            put(pic, 14 - i, 6 + i, red);
        }
        return;
    }

    // Waves: arcs around the cone's mouth, one per level step
    int waves = (state >= 2) ? state - 1 : 0;
    static const int radius[3] = { 3, 5, 7 };
    for (int w = 0; w < waves; w++) {
        int r = radius[w];
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = 1; dx <= r; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 >= (r - 1) * (r - 1) + 1 && d2 <= r * r + 1 && dx * 2 >= (dy < 0 ? -dy : dy))
                    put(pic, 8 + dx, 8 + dy, c);
            }
        }
    }
}

static void menu_send(void);

// Icon, tooltip and kept menu after a change
static void icon_update(void) {
    int state = icon_state();
    if (state != as.icon_state) {
        static uint32_t pic[ICON_PX * ICON_PX];
        draw_icon(pic, state);
        if (wm_tray_set_icon(ICON_ID, pic, ICON_PX, ICON_PX) == 0)
            as.icon_state = state;
    }
    char tip[64];
    if (as.fd < 0)
        snprintf(tip, sizeof(tip), "No sound device");
    else if (as.mute)
        snprintf(tip, sizeof(tip), "Volume: %d%% (muted)", as.volume);
    else
        snprintf(tip, sizeof(tip), "Volume: %d%%", as.volume);
    if (as.fd >= 0 && as.have_info && as.info.codec[0] != '\0') {
        size_t n = strlen(tip);
        snprintf(tip + n, sizeof(tip) - n, " - %.*s", (int)(sizeof(tip) - n - 4), as.info.codec);
    }
    if (strcmp(tip, as.tip) != 0 && wm_tray_set_tip(ICON_ID, tip) == 0)
        strcpy(as.tip, tip);
    if (as.menu_kept && as.menu_mute != as.mute)
        menu_send();
}

//
// The microphone icon
//

static void draw_mic(uint32_t* pic) {
    memset(pic, 0, ICON_PX * ICON_PX * sizeof(uint32_t));
    uint32_t red = 0xFFE04848u, dark = 0xFF902020u;
    // The head: a capsule
    for (int y = 1; y <= 9; y++) {
        for (int x = 5; x <= 10; x++) {
            bool corner = (y == 1 || y == 9) && (x == 5 || x == 10);
            if (!corner)
                put(pic, x, y, red);
        }
    }
    for (int y = 3; y <= 7; y += 2)             // Grille lines
        for (int x = 6; x <= 9; x++)
            put(pic, x, y, dark);
    // The holder: a U round the lower half
    for (int y = 6; y <= 10; y++) {
        put(pic, 3, y, red);
        put(pic, 12, y, red);
    }
    for (int x = 4; x <= 11; x++)
        put(pic, x, 11 + (x == 4 || x == 11 ? 0 : 1), red);
    put(pic, 4, 11, red);
    put(pic, 11, 11, red);
    // Stem and foot
    put(pic, 7, 13, red);
    put(pic, 8, 13, red);
    for (int x = 5; x <= 10; x++)
        put(pic, x, 14, red);
}

static const char* mic_source(int source) {
    switch (source) {
        case AUDIO_IN_MIC:          return "microphone jack";
        case AUDIO_IN_INTERNAL_MIC: return "built-in microphone";
        case AUDIO_IN_LINE:         return "line in";
        default:                    return NULL;
    }
}

// Who records, from the control device; the icon shown or taken away
static void mic_check(void) {
    audio_capture_t k;
    bool ok = as.fd >= 0 && audio_capture_status(as.fd, &k) == 0;
    uint64_t t = now();
    bool active = false;
    if (ok) {
        bool ran = as.mic_have_starts && k.starts != as.mic_starts;
        as.mic_starts = k.starts;
        as.mic_have_starts = true;
        active = k.state == AUDIO_CAPTURE_RUNNING;
        if (active || ran)
            as.mic_until = t + MIC_HOLD_MS;
    }
    bool show = active || (as.mic_until != 0 && t < as.mic_until);

    if (show) {
        char tip[64];
        if (ok && k.state != AUDIO_CAPTURE_IDLE && k.program[0] != '\0') {
            const char* src = mic_source(k.source);
            if (src != NULL)
                snprintf(tip, sizeof(tip), "Microphone in use by %.15s (%s)", k.program, src);
            else
                snprintf(tip, sizeof(tip), "Microphone in use by %.15s", k.program);
        } else if (as.mic_tip[0] != '\0') {
            snprintf(tip, sizeof(tip), "%s", as.mic_tip);        // Just ended: keep the name
        } else {
            snprintf(tip, sizeof(tip), "Microphone in use");
        }
        if (!as.mic_shown) {
            static uint32_t pic[ICON_PX * ICON_PX];
            draw_mic(pic);
            if (wm_tray_add(MIC_ICON_ID, pic, ICON_PX, ICON_PX, tip) == 0) {
                as.mic_shown = true;
                snprintf(as.mic_tip, sizeof(as.mic_tip), "%s", tip);
            }
        } else if (strcmp(tip, as.mic_tip) != 0 && wm_tray_set_tip(MIC_ICON_ID, tip) == 0) {
            snprintf(as.mic_tip, sizeof(as.mic_tip), "%s", tip);
        }
    } else if (as.mic_shown) {
        wm_tray_remove(MIC_ICON_ID);
        as.mic_shown = false;
        as.mic_tip[0] = '\0';
        as.mic_until = 0;
    }
}

//
// The right-click menu
//

static size_t menu_items(wm_menu_item_t* items) {
    items[0].label = as.mute ? "Unmute" : "Mute";
    items[0].id    = CMD_MUTE;
    items[0].flags = (as.fd < 0) ? WM_MENU_DISABLED : 0;
    return 1;
}

static void menu_send(void) {
    wm_menu_item_t items[1];
    size_t n = menu_items(items);
    as.menu_kept = wm_tray_set_menu(ICON_ID, items, n) == 0;
    as.menu_mute = as.mute;
}

//
// The panel
//

static void theme_load(void) {
    as.have_theme = wm_get_theme(&as.theme) == 0;
    if (!as.have_theme) {
        memset(&as.theme, 0, sizeof(as.theme));
        as.theme.menu.background = 0xFF222222u;
        as.theme.menu.border     = 0xFF444444u;
        as.theme.menu.highlight  = 0xFF777777u;
        as.theme.menu.text       = 0xFFE0E0E0u;
        as.theme.client.accent   = 0xFF04C004u;
    }
}

// The top of the taskbar, from the icon's rectangle (the panel sits on it)
static int32_t taskbar_top(int32_t screen_h) {
    int32_t middle = as.anchor_y + as.anchor_h / 2;
    int32_t top = (screen_h > middle) ? 2 * middle - screen_h : as.anchor_y;
    if (top > as.anchor_y) top = as.anchor_y;
    return top > 0 ? top : screen_h;
}

// The columns as they are now: the master, then the mixer's streams
static void columns_load(void) {
    column_t* c = &as.col[0];
    memset(c, 0, sizeof(*c));
    c->id = 0;
    snprintf(c->name, sizeof(c->name), "Master");
    c->level = as.volume;
    c->mute  = as.mute;
    as.ncol = 1;
    if (as.mixer_on) {
        mixer_stream_info_t st[MIXER_STREAMS];
        size_t n = mixer_streams(st, MIXER_STREAMS);
        for (size_t i = 0; i < n && as.ncol < COLS_MAX; i++) {
            c = &as.col[as.ncol++];
            c->id = st[i].id;
            snprintf(c->name, sizeof(c->name), "%s", st[i].name);
            c->level   = st[i].level;
            c->mute    = st[i].mute;
            c->playing = st[i].playing;
        }
    }
    if (as.sel >= as.ncol)
        as.sel = as.ncol - 1;
    if (as.drag && as.drag_col >= as.ncol)
        as.drag = false;
}

static bool measure(layout_t* l, int cols) {
    gfx_surface_t* probe = gfx_surface_create(1, 1);
    if (probe == NULL)
        return false;
    memset(l, 0, sizeof(*l));
    l->cw = gfx_char_width(probe);
    l->lh = gfx_line_height(probe);
    gfx_surface_destroy(probe);

    l->cols   = cols;
    l->mute_w = BOX + 6 + 4 * l->cw;
    l->col_w  = NAME_CELLS * l->cw;
    if (l->mute_w > l->col_w) l->col_w = l->mute_w;
    if (THUMB_W > l->col_w)   l->col_w = THUMB_W;
    l->w        = 2 * PAD + cols * l->col_w + (cols - 1) * COL_GAP;
    l->head_h   = l->lh + 12;
    l->slider_y = l->head_h + 2;
    l->track_y  = l->slider_y + 4 + THUMB_H / 2;
    l->slider_h = TRACK_LEN + THUMB_H + 8;
    l->level_y  = l->slider_y + l->slider_h;
    l->level_h  = l->lh + 4;
    l->mute_y   = l->level_y + l->level_h;
    l->mute_h   = (l->lh > BOX ? l->lh : BOX) + 8;
    l->note_y   = l->mute_y + l->mute_h;
    l->note_h   = 0;
    l->h        = l->note_y + PAD / 2;
    return true;
}

static int32_t col_x(const layout_t* l, int i) {
    return PAD + i * (l->col_w + COL_GAP);
}

// The column under x (the nearest one in a gap)
static int col_at(const layout_t* l, int32_t x) {
    int i = (int)((x - PAD + COL_GAP / 2) / (l->col_w + COL_GAP));
    if (i < 0) i = 0;
    if (i >= l->cols) i = l->cols - 1;
    return i;
}

static void place(int32_t* x, int32_t* y) {
    int32_t sw, sh;
    wm_screen_size(&sw, &sh);
    // Under the icon's middle if it fits, else against the right edge
    *x = as.anchor_x + as.anchor_w / 2 - as.lay.w / 2;
    if (*x + as.lay.w > sw) *x = sw - as.lay.w;
    if (*x < 0) *x = 0;
    *y = taskbar_top(sh) - as.lay.h;
    if (*y < 0) *y = 0;
}

// The name cut to fit the column, ".." at the end when it doesn't
static void fit_name(const layout_t* l, const char* name, char* out, size_t size) {
    size_t max = (size_t)(l->col_w / (l->cw > 0 ? l->cw : 8));
    if (max >= size) max = size - 1;
    size_t n = strlen(name);
    if (n <= max) {
        snprintf(out, size, "%s", name);
        return;
    }
    if (max < 3) max = 3;
    snprintf(out, size, "%.*s..", (int)(max - 2), name);
}

static void draw_column(gfx_surface_t* s, int i) {
    const layout_t* l = &as.lay;
    const column_t* c = &as.col[i];
    uint32_t bg     = opaque(as.theme.menu.background);
    uint32_t text   = opaque(as.theme.menu.text);
    uint32_t border = opaque(as.theme.menu.border);
    uint32_t muted  = mix(text, bg, 140);
    uint32_t dim    = mix(text, bg, 60);
    uint32_t accent = opaque(as.theme.client.accent);
    bool live = (i == 0) ? as.fd >= 0 : true;
    int32_t x0 = col_x(l, i);
    int32_t cx = x0 + l->col_w / 2;

    // The name, underlined in the accent colour for the one the keys move
    char name[40];
    fit_name(l, c->name, name, sizeof(name));
    int32_t nw = gfx_text_width(s, name);
    gfx_draw_text(s, cx - nw / 2, gfx_text_y(s, 0, l->head_h), name, (i > 0 && !c->playing) ? muted : text);
    if (i == as.sel && as.ncol > 1)
        gfx_fill_rect(s, cx - nw / 2, l->head_h - 4, nw, 2, accent);

    // The slider, upright: the filled part from the bottom in the accent
    // colour (grey while muted)
    int32_t fill = TRACK_LEN * c->level / 100;
    int32_t bottom = l->track_y + TRACK_LEN;
    gfx_fill_rect(s, cx - TRACK_W / 2, l->track_y, TRACK_W, TRACK_LEN, live ? mix(text, bg, 50) : dim);
    if (live)
        gfx_fill_rect(s, cx - TRACK_W / 2, bottom - fill, TRACK_W, fill, c->mute ? muted : accent);
    int32_t ty = bottom - fill - THUMB_H / 2;
    bool dragging = as.drag && as.drag_col == i;
    gfx_fill_rect(s, cx - THUMB_W / 2, ty, THUMB_W, THUMB_H, !live ? dim : dragging ? accent : text);
    gfx_draw_rect(s, cx - THUMB_W / 2, ty, THUMB_W, THUMB_H, border);

    // The level under it
    char level[16];
    if (!live)
        snprintf(level, sizeof(level), "--");
    else if (c->mute)
        snprintf(level, sizeof(level), "muted");
    else
        snprintf(level, sizeof(level), "%d%%", c->level);
    gfx_draw_text(s, cx - gfx_text_width(s, level) / 2, gfx_text_y(s, l->level_y, l->level_h),
                  level, (c->mute || !live) ? muted : text);

    // The Mute box
    int32_t bx = x0 + (l->col_w - l->mute_w) / 2;
    int32_t by = l->mute_y + (l->mute_h - BOX) / 2;
    gfx_draw_rect(s, bx, by, BOX, BOX, live ? text : dim);
    if (c->mute) {
        for (int k = 0; k < 2; k++) {
            gfx_draw_line(s, bx + 3, by + 6 + k, bx + 5, by + 8 + k, accent);
            gfx_draw_line(s, bx + 5, by + 8 + k, bx + 9, by + 3 + k, accent);
        }
    }
    gfx_draw_text(s, bx + BOX + 6, gfx_text_y(s, l->mute_y, l->mute_h), "Mute", live ? text : dim);
}

static void draw_panel(gfx_surface_t* s) {
    const layout_t* l = &as.lay;
    uint32_t bg     = opaque(as.theme.menu.background);
    uint32_t border = opaque(as.theme.menu.border);

    gfx_clear(s, bg);
    gfx_draw_rect(s, 0, 0, l->w, l->h, border);
    gfx_draw_hline(s, 1, l->head_h, l->w - 2, border);
    int n = as.ncol < l->cols ? as.ncol : l->cols;
    for (int i = 0; i < n; i++)
        draw_column(s, i);
    // The master apart from the programs
    if (n > 1) {
        int32_t x = col_x(l, 1) - COL_GAP / 2 - 1;
        gfx_fill_rect(s, x, l->head_h + 1, 1, l->h - l->head_h - 2, border);
    }
}

static void popup_draw(void) {
    if (!as.open || as.popup == 0)
        return;
    gfx_surface_t* s = wm_window_surface(as.popup);
    if (s == NULL)
        return;
    int32_t w, h;
    wm_window_get_size(as.popup, &w, &h);
    if (w != as.lay.w || h != as.lay.h)
        return;                         // A resize on its way: drawn on RESIZED
    draw_panel(s);
    wm_window_commit(as.popup, NULL);
}

static void popup_failed(const char* what) {
    char text[96];
    snprintf(text, sizeof(text), "%s: %s", what, strerror(errno));
    wm_notify_ex(WM_NOTIFY_ERROR, "Volume", text, WM_NOTIFY_DEFAULT_MS);
}

// The columns again; the window made to fit when their number changed
static void popup_refresh(void) {
    columns_load();
    if (!as.open || as.popup == 0)
        return;
    if (as.ncol != as.lay.cols) {
        layout_t l;
        if (!measure(&l, as.ncol))
            return;
        as.lay = l;
        int32_t x, y;
        place(&x, &y);
        wm_window_resize(as.popup, as.lay.w, as.lay.h);
        if (x != as.x || y != as.y)
            wm_window_move(as.popup, x, y);
        as.x = x;
        as.y = y;
    }
    popup_draw();
}

static void popup_open(const wm_event_t* ev) {
    as.anchor_x = ev->rect_x;
    as.anchor_y = ev->rect_y;
    as.anchor_w = ev->rect_w;
    as.anchor_h = ev->rect_h;
    if (!as.have_theme)
        theme_load();
    device_read();
    icon_update();
    columns_load();
    as.sel = 0;

    layout_t l;
    if (!measure(&l, as.ncol)) {
        popup_failed("no surface to measure the font on");
        return;
    }
    bool resized = as.popup != 0 && (l.w != as.lay.w || l.h != as.lay.h);
    as.lay = l;
    int32_t x, y;
    place(&x, &y);
    if (as.popup == 0) {
        wm_window_desc_t d = { .title = "Volume", .x = x, .y = y,
                               .width = as.lay.w, .height = as.lay.h, .style = STYLE_POPUP };
        as.popup = wm_window_create(&d);
        if (as.popup == 0) {
            popup_failed("no window");
            return;
        }
    } else {
        if (resized)
            wm_window_resize(as.popup, as.lay.w, as.lay.h);
        if (x != as.x || y != as.y)
            wm_window_move(as.popup, x, y);
    }
    as.x = x;
    as.y = y;
    as.open = true;
    popup_draw();
    if (wm_window_show(as.popup, true) < 0)     // Raised and focused: clicking elsewhere closes it
        popup_failed("can't show the window");
    as.shown_at = now();
}

static void popup_close(void) {
    if (!as.open)
        return;
    as.open = false;
    as.drag = false;
    if (as.popup != 0)
        wm_window_show(as.popup, false);
}

// The master's level or mute from the panel: to the device, the icon, the
// panel, and the registry a moment later
static void set_level(int volume, bool mute) {
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    if (volume == as.volume && mute == as.mute)
        return;
    as.volume = volume;
    as.mute   = mute;
    as.col[0].level = volume;
    as.col[0].mute  = mute;
    device_write();
    icon_update();
    popup_draw();
    as.save_at = now() + SAVE_DELAY_MS;
}

// Any column's: the master as above, a program's to the mixer
static void set_col(int i, int level, bool mute) {
    if (i <= 0 || i >= as.ncol) {
        set_level(level, mute);
        return;
    }
    if (level < 0)   level = 0;
    if (level > 100) level = 100;
    column_t* c = &as.col[i];
    if (level == c->level && mute == c->mute)
        return;
    c->level = level;
    c->mute  = mute;
    mixer_set_level(c->id, level, mute);
    popup_draw();
}

// Moving the level unmutes, as on other desktops
static void move_col(int i, int level) {
    set_col(i, level, false);
}

static void toggle_mute(int i) {
    if (i < 0 || i >= as.ncol)
        return;
    set_col(i, as.col[i].level, !as.col[i].mute);
}

static int level_at(int32_t y) {
    const layout_t* l = &as.lay;
    int32_t pos = l->track_y + TRACK_LEN - y;
    if (pos < 0) pos = 0;
    if (pos > TRACK_LEN) pos = TRACK_LEN;
    return (int)((pos * 100 + TRACK_LEN / 2) / TRACK_LEN);
}

static void popup_press(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return;
    const layout_t* l = &as.lay;
    int i = col_at(l, ev->x);
    if (i >= as.ncol)
        return;
    if (i == 0 && as.fd < 0)
        return;
    bool changed = as.sel != i;
    as.sel = i;
    if (ev->y >= l->slider_y && ev->y < l->slider_y + l->slider_h) {
        as.drag = true;
        as.drag_col = i;
        move_col(i, level_at(ev->y));
        popup_draw();
    } else if (ev->y >= l->mute_y && ev->y < l->mute_y + l->mute_h) {
        toggle_mute(i);
    } else if (changed) {
        popup_draw();
    }
}

static void popup_key(uint16_t key) {
    int i = as.sel;
    int v = (i >= 0 && i < as.ncol) ? as.col[i].level : as.volume;
    switch (key) {
        case KEY_ESCAPE:
        case KEY_ENTER:     popup_close();                  return;
        case KEY_DOWN:      move_col(i, v - STEP);          return;
        case KEY_UP:        move_col(i, v + STEP);          return;
        case KEY_PAGE_DOWN: move_col(i, v - PAGE);          return;
        case KEY_PAGE_UP:   move_col(i, v + PAGE);          return;
        case KEY_HOME:      move_col(i, 0);                 return;
        case KEY_END:       move_col(i, 100);               return;
        case KEY_LEFT:
            if (as.ncol == 1) { move_col(0, v - STEP); return; }
            as.sel = (as.sel + as.ncol - 1) % as.ncol;
            popup_draw();
            return;
        case KEY_RIGHT:
        case KEY_TAB:
            if (as.ncol == 1) { if (key == KEY_RIGHT) move_col(0, v + STEP); return; }
            as.sel = (as.sel + 1) % as.ncol;
            popup_draw();
            return;
        case 'm': case 'M': case ' ':
            toggle_mute(i);
            return;
        default: return;
    }
}

static void left_click(const wm_event_t* ev) {
    if (as.open) {
        popup_close();
        return;
    }
    popup_open(ev);
}

static bool handle(const wm_event_t* ev) {
    switch (ev->type) {
        case WM_EVENT_TRAY:
            if (ev->tray != ICON_ID)
                break;
            if (ev->button == INPUT_BTN_LEFT) {
                left_click(ev);
            } else if (ev->button == INPUT_BTN_RIGHT && !as.menu_kept) {
                // An older desktop: the menu goes up in answer to the click
                wm_menu_item_t items[1];
                size_t n = menu_items(items);
                wm_tray_menu(ev->tray, items, n);
            }
            break;
        case WM_EVENT_TRAY_COMMAND:
            if (ev->tray == ICON_ID && ev->param == CMD_MUTE && as.fd >= 0) {
                set_level(as.volume, !as.mute);
                columns_load();
                popup_draw();
            }
            break;
        case WM_EVENT_THEME_CHANGED:
            // The font may have changed too: the panel is made again, at
            // its new size, the next time it opens
            as.have_theme = false;
            popup_close();
            if (as.popup != 0)
                wm_window_destroy(as.popup);
            as.popup = 0;
            break;
        default:
            if (ev->window == 0 || ev->window != as.popup)
                break;
            switch (ev->type) {
                case WM_EVENT_FOCUS_LOST:
                    if (now() - as.shown_at >= FOCUS_GRACE_MS)
                        popup_close();
                    break;
                case WM_EVENT_CLOSE:       popup_close();        break;
                case WM_EVENT_KEY:         popup_key(ev->key);   break;
                case WM_EVENT_MOUSE_DOWN:  popup_press(ev);      break;
                case WM_EVENT_MOUSE_MOVE:
                    if (as.drag)
                        move_col(as.drag_col, level_at(ev->y));
                    break;
                case WM_EVENT_MOUSE_UP:
                    if (as.drag) {
                        as.drag = false;
                        popup_draw();
                    }
                    break;
                case WM_EVENT_MOUSE_WHEEL:
                    // + toward the user: down, quieter
                    if (ev->wheel != 0 && as.sel >= 0 && as.sel < as.ncol &&
                        (as.sel > 0 || as.fd >= 0))
                        move_col(as.sel, as.col[as.sel].level - ev->wheel * STEP);
                    break;
                case WM_EVENT_RESIZED:
                    popup_draw();
                    break;
                case WM_EVENT_DESTROYED:
                    as.popup = 0;
                    as.open  = false;
                    break;
                default: break;
            }
            break;
    }
    return true;
}

// Due work: the registry, and a look at the device's level
static void tick(void) {
    uint64_t t = now();
    if (as.save_at != 0 && t >= as.save_at && !as.drag) {
        as.save_at = 0;
        settings_save();
    }
    if (t >= as.poll_at) {
        bool had = as.fd >= 0;
        int v = as.volume;
        bool m = as.mute;
        // Not while dragging: the panel's level wins
        if (!as.drag)
            device_read();
        as.poll_at = t + (as.fd >= 0 ? POLL_MS : RETRY_MS);
        if (had != (as.fd >= 0) || v != as.volume || m != as.mute) {
            icon_update();
            columns_load();
            popup_draw();
        }
    }
    if (t >= as.mic_poll_at) {
        as.mic_poll_at = t + MIC_POLL_MS;
        mic_check();
    }
}

static int32_t timeout(void) {
    uint64_t due = as.poll_at;
    if (as.mic_poll_at < due)
        due = as.mic_poll_at;
    if (as.save_at != 0 && as.save_at < due)
        due = as.save_at;
    uint64_t t = now();
    return due <= t ? 0 : (int32_t)(due - t);
}

int main(void) {
    as.fd = -1;
    as.icon_state = -1;
    as.volume = 80;

    // Only what a volume icon needs; admin and the rest go now
    sec_mask_t extra = sec_get() & ~(sec_mask_t)SEC_KEEP;
    if (extra != 0)
        sec_drop(extra);

    if (wm_connect(10000) < 0)
        return 1;           // No desktop: nowhere to show anything

    int port = ipc_port_create(INSTANCE_PORT);
    if (port < 0) {
        int err = errno;
        if (err == EEXIST)
            wm_notify_ex(WM_NOTIFY_INFO, "Volume", "The volume service is already running", WM_NOTIFY_DEFAULT_MS);
        wm_disconnect();
        return err == EEXIST ? 0 : 1;
    }
    as.instance = port;

    device_read();
    settings_load();        // The level from last time, back on the device

    static uint32_t pic[ICON_PX * ICON_PX];
    as.icon_state = icon_state();
    draw_icon(pic, as.icon_state);
    if (wm_tray_add(ICON_ID, pic, ICON_PX, ICON_PX, "Volume") < 0) {
        int err = errno;
        wm_notify_ex(WM_NOTIFY_ERROR, "Volume icon", err == EMFILE || err == ENOSPC ? "The tray is full" : strerror(err),
                     WM_NOTIFY_DEFAULT_MS);
        ipc_close(as.instance);
        wm_disconnect();
        return 1;
    }
    strcpy(as.tip, "Volume");
    menu_send();
    icon_update();
    as.poll_at = now() + (as.fd >= 0 ? POLL_MS : RETRY_MS);

    // The mixer: off if its port is taken (an older copy), the rest works
    as.mixer_on = mixer_init();

    wm_event_t ev;
    bool running = true;
    while (running) {
        // The desktop and the mixer's handles together
        ipc_wait_item_t items[2 + MIXER_STREAMS];
        size_t n = 0;
        items[n].handle = wm_handle();
        items[n].events = IPC_READABLE;
        items[n].ready  = 0;
        n++;
        n += mixer_wait_items(items + n, sizeof(items) / sizeof(items[0]) - n);
        int32_t wait = timeout();
        int32_t mw = mixer_timeout();
        if (mw >= 0 && mw < wait)
            wait = mw;
        ipc_wait(items, n, wait);

        for (int k = 0; k < 64 && running; k++) {
            int r = wm_wait_event(&ev, 0);
            if (r < 0) {
                running = false;        // The desktop went away
                break;
            }
            if (r == 0)
                break;
            running = handle(&ev);
        }
        if (mixer_tick())
            popup_refresh();
        tick();
    }
    mixer_shutdown();

    if (as.save_at != 0)
        settings_save();
    if (as.popup != 0)
        wm_window_destroy(as.popup);
    if (as.mic_shown)
        wm_tray_remove(MIC_ICON_ID);
    wm_tray_remove(ICON_ID);
    device_close();
    ipc_close(as.instance);
    wm_disconnect();
    return 0;
}

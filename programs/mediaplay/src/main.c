/*
 * mediaplay: a media player for MP3 and WAV files.
 *
 *     mediaplay [file ...]
 *
 * One window: the title, artist and album (from the file's ID3 tags, else
 * its name), what the file is, a position bar to click or drag, the time,
 * and Previous / Play-Pause / Stop / Next / Open buttons. Opened with one
 * file (from the explorer, the Open dialog or a drop) it plays that file
 * and Next / Previous go through the other MP3 and WAV files in the same
 * folder, by name; with several it plays those in turn. At the end of the
 * list it stops, unless Repeat (right click) is on.
 *
 * Keys: Space play / pause, S stop, Left / Right 5 s back / on (Ctrl: 30
 * s), Home to the start, Page Up / Page Down (or P / N) previous / next,
 * Ctrl+O open, R repeat. Files dropped on the window replace the list.
 *
 * Decoding: minimp3 (track.c) for MPEG audio layer 1, 2 and 3; PCM WAV as
 * it is. Sound goes to /dev/audio0 through os/audio.h as 16-bit stereo,
 * at the file's rate when the codec takes it, else resampled (linear) to
 * 48 or 44.1 kHz. The device is held only while playing or paused, so
 * other programs (play, games) can have it otherwise. Volume and mute are
 * the taskbar's volume icon (audiosrv).
 *
 * Needs SEC_GFX, SEC_IPC, SEC_FILE_READ, and SEC_FILE_WRITE for the sound
 * device (the default clearance has them all).
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <os/audio.h>
#include <os/filedlg.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/os.h>
#include <os/wm.h>
#include <os/wm_app.h>     // Not on the desktop: says so and exits before main()
#include "track.h"

#ifndef APP_NAME
#define APP_NAME        "mediaplay"
#endif

#define WIN_W           420
#define WIN_H           164         // Made to fit the font's line height at the start
#define WIN_MIN_W       330
#define WIN_MIN_H       150

#define PAD             10
#define BAR_H           8
#define BAR_HIT         7           // Pixels above and below the bar that still grab it
#define BTN_W           40
#define BTN_H           28
#define BTN_GAP         6
#define OPEN_W          64

#define PLAYLIST_MAX    512
#define PEND_FRAMES     8192        // Output of one decode, resampled (1152 * 6 and some)
#define SEEK_STEP       5.0
#define SEEK_STEP_BIG   30.0
#define TICK_MS         20          // How often the queue is topped up while playing

enum {
    CMD_OPEN = 1,
    CMD_PLAY,
    CMD_STOP,
    CMD_PREV,
    CMD_NEXT,
    CMD_REPEAT,
};

// Buttons, left to right
enum {
    BTN_PREV = 0,
    BTN_PLAY,
    BTN_STOP,
    BTN_NEXT,
    BTN_OPEN,
    BTN_COUNT,
};

typedef enum {
    P_STOPPED,
    P_PLAYING,
    P_PAUSED,
} play_state_t;

//
// State
//

static struct {
    char*   paths[PLAYLIST_MAX];
    size_t  count;
    size_t  index;              // The one loaded
    bool    repeat;
} list;

static struct {
    track_t      t;
    bool         loaded;
    play_state_t state;

    int          fd;            // /dev/audio0, -1 while stopped
    audio_info_t info;
    uint32_t     out_rate;

    // Linear resampling, when the codec can't take the file's rate
    bool         resample;
    double       step;          // Input frames per output frame
    double       rpos;          // Where the next output frame is, in the input (-1: the last one kept)
    int16_t      prev[2];

    int16_t*     dec;           // TRACK_MAX_FRAMES stereo frames from the decoder
    int16_t*     pend;          // PEND_FRAMES stereo frames waiting for room in the device
    size_t       pend_len, pend_pos;
    bool         flushing;      // Decoded to the end; waiting for the queue to play out

    double       base;          // Position (s) where the stream was last (re)started
    uint64_t     written_base;  // The device's written_frames then
    audio_status_t status;      // As of the last tick
} pl;

static wm_window_t  main_win;
static wm_theme_t   theme;
static bool         running = true;
static bool         dirty;
static filedlg_t*   dlg;
static char         message[160];       // An error or note for the info line ("" none)
static bool         message_bad;

static int          pressed_btn = -1;   // Held down with the mouse
static bool         dragging;           // The position bar
static double       drag_pos;
static int          shown_second = -1;  // What the window shows, to repaint only on a change
static int32_t      shown_bar = -1;

//
// Small things
//

static void default_theme(void) {
    memset(&theme, 0, sizeof(theme));
    theme.client.background     = GFX_RGB(236, 236, 236);
    theme.client.text           = GFX_RGB(20, 20, 20);
    theme.client.text_soft      = GFX_RGB(80, 80, 80);
    theme.client.text_danger    = GFX_RGB(190, 30, 30);
    theme.client.accent         = GFX_RGB(40, 100, 200);
    theme.client.divider_soft   = GFX_RGB(200, 200, 200);
    theme.button.fill           = GFX_RGB(222, 222, 222);
    theme.button.border         = GFX_RGB(140, 140, 140);
    theme.button.border_active  = GFX_RGB(40, 100, 200);
    theme.button.text           = GFX_RGB(20, 20, 20);
    theme.button.fill_pressed   = GFX_RGB(190, 190, 190);
    theme.button.border_pressed = GFX_RGB(90, 90, 90);
    theme.edit.background       = GFX_COLOR_WHITE;
    theme.edit.border           = GFX_RGB(140, 140, 140);
}

static void load_theme(void) {
    if (wm_get_theme(&theme) < 0)
        default_theme();
}

static void set_message(bool bad, const char* text) {
    strlcpy(message, text != NULL ? text : "", sizeof(message));
    message_bad = bad;
    dirty = true;
}

static const char* base_name(const char* path) {
    const char* s = strrchr(path, '/');
    return s != NULL ? s + 1 : path;
}

static bool is_audio_name(const char* name) {
    const char* dot = strrchr(name, '.');
    if (dot == NULL || dot == name)
        return false;
    return !strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".wav") ||
           !strcasecmp(dot, ".mp2") || !strcasecmp(dot, ".mpga");
}

// "3:07", "1:02:45"
static void format_time(double seconds, char* out, size_t size) {
    if (seconds < 0)
        seconds = 0;
    unsigned s = (unsigned)seconds;
    if (s >= 3600)
        snprintf(out, size, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, size, "%u:%02u", s / 60, s % 60);
}

static void update_title(void) {
    char title[160];
    if (pl.loaded) {
        const char* name = pl.t.title[0] ? pl.t.title : base_name(list.paths[list.index]);
        snprintf(title, sizeof(title), "%s - Media player", name);
    } else {
        strlcpy(title, "Media player", sizeof(title));
    }
    wm_window_set_title(main_win, title);
}

//
// The playlist
//

static void list_clear(void) {
    for (size_t i = 0; i < list.count; i++)
        free(list.paths[i]);
    list.count = 0;
    list.index = 0;
}

static bool list_add(const char* path) {
    if (list.count >= PLAYLIST_MAX)
        return false;
    char* copy = strdup(path);
    if (copy == NULL)
        return false;
    list.paths[list.count++] = copy;
    return true;
}

static int compare_paths(const void* a, const void* b) {
    return strcasecmp(base_name(*(char* const*)a), base_name(*(char* const*)b));
}

// An absolute form of `path` (relative ones against the cwd) into out
static void absolute(const char* path, char* out, size_t size) {
    if (path[0] == '/') {
        strlcpy(out, path, size);
        return;
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        cwd[0] = 0;
    size_t n = strlen(cwd);
    snprintf(out, size, "%s%s%s", cwd, (n > 0 && cwd[n - 1] == '/') ? "" : "/", path);
}

// One file: it and the other sound files of its folder, by name, from it
static void list_from_folder(const char* file) {
    char path[PATH_MAX];
    absolute(file, path, sizeof(path));
    list_clear();

    char dir[PATH_MAX];
    strlcpy(dir, path, sizeof(dir));
    char* slash = strrchr(dir, '/');
    if (slash == NULL) {
        list_add(path);
        return;
    }
    if (slash == dir)
        slash[1] = 0;
    else
        *slash = 0;

    DIR* d = opendir(dir);
    if (d != NULL) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR || !is_audio_name(e->d_name))
                continue;
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s%s%s", dir, (dir[strlen(dir) - 1] == '/') ? "" : "/", e->d_name);
            if (!list_add(p))
                break;
        }
        closedir(d);
    }
    if (list.count > 1)
        qsort(list.paths, list.count, sizeof(list.paths[0]), compare_paths);
    for (size_t i = 0; i < list.count; i++) {
        if (!strcmp(list.paths[i], path)) {
            list.index = i;
            return;
        }
    }
    // Not found by name (another spelling of the path): just the file
    list_clear();
    list_add(path);
}

//
// Sound output
//

static void close_device(void) {
    if (pl.fd >= 0) {
        audio_stop(pl.fd);
        audio_close(pl.fd);
    }
    pl.fd = -1;
}

static bool open_device(void) {
    if (pl.fd >= 0)
        return true;
    pl.fd = audio_open(0);
    if (pl.fd < 0) {
        if (errno == ENOENT)
            set_message(true, "No sound device");
        else if (errno == EBUSY)
            set_message(true, "The sound device is in use by another program");
        else {
            char m[96];
            snprintf(m, sizeof(m), "Sound device: %s", strerror(errno));
            set_message(true, m);
        }
        return false;
    }
    audio_set_name(pl.fd, "Media player");     // Its row on the volume panel
    if (audio_info(pl.fd, &pl.info) < 0 || !(pl.info.formats & AUDIO_FORMAT_S16)) {
        set_message(true, "The sound device can't play 16-bit sound");
        close_device();
        return false;
    }
    return true;
}

// The rate to play the track at: its own if the codec has it
static uint32_t pick_rate(uint32_t rate) {
    uint32_t rates = pl.info.rates;
    if (audio_rate_bit(rate) & rates)
        return rate;
    bool cd = (rate % 11025) == 0;          // 22050, 11025: 44.1 kHz family
    if (cd && (rates & AUDIO_RATE_44100)) return 44100;
    if (rates & AUDIO_RATE_48000) return 48000;
    if (rates & AUDIO_RATE_44100) return 44100;
    static const uint32_t all[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000,
                                    88200, 96000, 176400, 192000 };
    for (int i = 10; i >= 0; i--)
        if (rates & (1u << i))
            return all[i];
    return 48000;
}

// Set the format (which drops anything queued) and play from pl.base on
static bool start_stream(void) {
    pl.out_rate = pick_rate(pl.t.rate);
    if (audio_set_format(pl.fd, pl.out_rate, 2, AUDIO_FORMAT_S16) < 0) {
        char m[96];
        snprintf(m, sizeof(m), "Can't play %u Hz: %s", (unsigned)pl.out_rate, strerror(errno));
        set_message(true, m);
        return false;
    }
    pl.resample = (pl.out_rate != pl.t.rate);
    pl.step = (double)pl.t.rate / pl.out_rate;
    pl.rpos = 0;
    pl.prev[0] = pl.prev[1] = 0;
    pl.pend_len = pl.pend_pos = 0;
    pl.flushing = false;
    memset(&pl.status, 0, sizeof(pl.status));
    if (audio_status(pl.fd, &pl.status) == 0)
        pl.written_base = pl.status.written_frames;
    else
        pl.written_base = 0;
    return true;
}

// n decoded frames into pl.pend at the output rate
static void convert(size_t n) {
    pl.pend_pos = 0;
    if (!pl.resample) {
        memcpy(pl.pend, pl.dec, n * 4);
        pl.pend_len = n;
        return;
    }
    // x[-1] is pl.prev, x[0..n-1] the new frames; rpos in [-1, n - 1)
    size_t o = 0;
    double p = pl.rpos;
    while (p < (double)n - 1 && o < PEND_FRAMES) {
        int i = (p < 0) ? -1 : (int)p;
        double f = p - i;
        for (int c = 0; c < 2; c++) {
            int a = (i < 0) ? pl.prev[c] : pl.dec[i * 2 + c];
            int b = pl.dec[(i + 1) * 2 + c];
            pl.pend[o * 2 + c] = (int16_t)(a + (b - a) * f);
        }
        o++;
        p += pl.step;
    }
    pl.rpos = p - (double)n;
    pl.prev[0] = pl.dec[(n - 1) * 2];
    pl.prev[1] = pl.dec[(n - 1) * 2 + 1];
    pl.pend_len = o;
}

static void stop(void);

// Keep the device's queue full while playing
static void pump(void) {
    if (pl.state != P_PLAYING || pl.fd < 0)
        return;
    for (int i = 0; i < 32; i++) {
        if (pl.pend_pos < pl.pend_len) {
            long w = audio_write(pl.fd, pl.pend + pl.pend_pos * 2, (pl.pend_len - pl.pend_pos) * 4);
            if (w < 0) {
                char m[96];
                snprintf(m, sizeof(m), "Sound device: %s", strerror(errno));
                stop();
                set_message(true, m);
                return;
            }
            pl.pend_pos += (size_t)w / 4;
            if (pl.pend_pos < pl.pend_len)
                return;                     // Full for now
        }
        if (pl.flushing)
            return;
        size_t n = track_decode(&pl.t, pl.dec);
        if (n == 0) {
            // All of it is queued: play even what is below the start level
            pl.flushing = true;
            audio_start(pl.fd);
            return;
        }
        convert(n);
    }
}

// Seconds into the track that are playing now
static double position(void) {
    if (pl.state == P_STOPPED || pl.fd < 0 || pl.out_rate == 0)
        return pl.base;
    uint64_t written = pl.status.written_frames - pl.written_base;
    uint64_t queued = pl.status.queued_bytes / 4;
    double played = written > queued ? (double)(written - queued) : 0;
    double s = pl.base + played / pl.out_rate;
    if (pl.t.duration > 0 && s > pl.t.duration)
        s = pl.t.duration;
    return s;
}

//
// Playing
//

static void unload(void) {
    close_device();
    if (pl.loaded)
        track_close(&pl.t);
    pl.loaded = false;
    pl.state = P_STOPPED;
    pl.base = 0;
}

// Load list entry `index`; false (and the reason shown) if it can't be read
static bool load(size_t index) {
    bool was_open = pl.fd >= 0;
    if (pl.loaded)
        track_close(&pl.t);
    pl.loaded = false;
    pl.base = 0;
    list.index = index;
    const char* why = "Can't open";
    if (index >= list.count || !track_open(&pl.t, list.paths[index], &why)) {
        pl.t.fd = -1;
        char m[160];
        snprintf(m, sizeof(m), "%s: %s", index < list.count ? base_name(list.paths[index]) : "?", why);
        set_message(true, m);
        if (was_open)
            close_device();
        pl.state = P_STOPPED;
        update_title();
        return false;
    }
    pl.loaded = true;
    set_message(false, "");
    update_title();
    shown_second = -1;
    dirty = true;
    return true;
}

static void play(void) {
    if (!pl.loaded)
        return;
    if (pl.state == P_PLAYING)
        return;
    if (pl.state == P_PAUSED && pl.fd >= 0) {
        pl.state = P_PLAYING;
        pump();
        audio_start(pl.fd);
        dirty = true;
        return;
    }
    if (!open_device())
        return;
    if (!start_stream()) {
        close_device();
        return;
    }
    set_message(false, "");
    pl.state = P_PLAYING;
    pump();
    dirty = true;
}

static void pause_play(void) {
    if (pl.state != P_PLAYING || pl.fd < 0)
        return;
    audio_status(pl.fd, &pl.status);
    audio_pause(pl.fd);
    pl.state = P_PAUSED;
    dirty = true;
}

static void stop(void) {
    close_device();
    pl.state = P_STOPPED;
    pl.base = 0;
    if (pl.loaded)
        track_seek(&pl.t, 0);
    dirty = true;
}

static void toggle_play(void) {
    if (pl.state == P_PLAYING)
        pause_play();
    else
        play();
}

static void seek_to(double s) {
    if (!pl.loaded)
        return;
    if (s < 0)
        s = 0;
    if (pl.t.duration > 0 && s > pl.t.duration)
        s = pl.t.duration;
    track_seek(&pl.t, s);
    pl.base = s;
    if (pl.state != P_STOPPED && pl.fd >= 0) {
        if (!start_stream()) {
            stop();
            return;
        }
        pump();
    }
    shown_second = -1;
    dirty = true;
}

// Load list entry `index` and keep playing if it was; one that can't be
// read is skipped, moving by `dir`, until one can or the list runs out
static void go_to(size_t index, int dir, bool keep_playing) {
    for (size_t tries = 0; tries < list.count; tries++) {
        if (load(index)) {
            if (keep_playing) {
                if (pl.fd >= 0) {
                    if (start_stream()) {   // The device is still open: just the new format
                        pl.state = P_PLAYING;
                        pump();
                    } else {
                        stop();
                    }
                } else {
                    play();
                }
            } else {
                stop();
            }
            return;
        }
        if (dir == 0 || list.count < 2)
            return;
        index = (index + list.count + (size_t)dir) % list.count;
    }
}

static void next_track(bool from_end) {
    if (list.count == 0)
        return;
    bool playing = pl.state == P_PLAYING;
    if (list.index + 1 < list.count) {
        go_to(list.index + 1, 1, playing || from_end);
    } else if (list.repeat) {
        go_to(0, 1, playing || from_end);
    } else if (from_end) {
        stop();                             // The end of the list: back to the start of the last
    } else {
        go_to(0, 1, playing);
    }
}

static void prev_track(void) {
    if (list.count == 0)
        return;
    bool playing = pl.state == P_PLAYING;
    // A few seconds in: back to the start of this one first
    if (pl.loaded && position() > 3.0) {
        seek_to(0);
        return;
    }
    size_t i = list.index > 0 ? list.index - 1 : list.count - 1;
    go_to(i, -1, playing);
}

// Every tick while playing: top up, notice the end, repaint when the time moved
static void tick(void) {
    if (pl.state == P_PLAYING && pl.fd >= 0) {
        pump();
        if (pl.state == P_PLAYING && audio_status(pl.fd, &pl.status) == 0 && pl.flushing &&
            pl.status.queued_bytes == 0) {
            next_track(true);
        }
    }
    int sec = (int)position();
    if (sec != shown_second)
        dirty = true;
}

// Start on `files` (absolute paths): one file brings its folder along
static void open_files(char** files, size_t count) {
    unload();
    if (count == 1) {
        list_from_folder(files[0]);
    } else {
        list_clear();
        for (size_t i = 0; i < count; i++) {
            char p[PATH_MAX];
            absolute(files[i], p, sizeof(p));
            list_add(p);
        }
    }
    if (list.count == 0)
        return;
    go_to(list.index, 1, true);
}

static void open_one(const char* path) {
    char* files[1] = { (char*)path };
    open_files(files, 1);
}

//
// Drawing
//

typedef struct {
    int32_t    width, height, lh;
    int32_t    title_y, sub_y, info_y;
    gfx_rect_t bar;
    int32_t    time_y;
    gfx_rect_t btn[BTN_COUNT];
} metrics_t;

static metrics_t get_metrics(gfx_surface_t* s) {
    metrics_t m;
    wm_window_get_size(main_win, &m.width, &m.height);
    m.lh = gfx_line_height(s);
    m.title_y = PAD;
    m.sub_y = m.title_y + m.lh + 4;
    m.info_y = m.sub_y + m.lh + 2;
    m.bar = gfx_rect(PAD, m.info_y + m.lh + 10, m.width - 2 * PAD, BAR_H);
    m.time_y = m.bar.y + BAR_H + 4;
    int32_t by = m.height - PAD - BTN_H;
    int32_t min_by = m.time_y + m.lh + 6;
    if (by < min_by)
        by = min_by;
    for (int i = 0; i < BTN_OPEN; i++)
        m.btn[i] = gfx_rect(PAD + i * (BTN_W + BTN_GAP), by, BTN_W, BTN_H);
    m.btn[BTN_OPEN] = gfx_rect(m.width - PAD - OPEN_W, by, OPEN_W, BTN_H);
    return m;
}

static bool in_rect(const gfx_rect_t* r, int32_t x, int32_t y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

// A triangle pointing right (dir 1) or left (-1), its tip at x + w or x
static void fill_triangle(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h, int dir, gfx_color_t c) {
    for (int32_t r = 0; r < h; r++) {
        int32_t half = h / 2;
        int32_t d = r <= half ? r : h - 1 - r;
        int32_t len = (half > 0) ? (w * d + half / 2) / half : w;
        if (len < 1)
            len = 1;
        if (dir > 0)
            gfx_draw_hline(s, x, y + r, len, c);
        else
            gfx_draw_hline(s, x + w - len, y + r, len, c);
    }
}

static void draw_button(gfx_surface_t* s, const gfx_rect_t* r, int which, bool enabled) {
    bool pressed = (pressed_btn == which);
    gfx_color_t fill = pressed ? theme.button.fill_pressed : theme.button.fill;
    gfx_color_t border = pressed ? theme.button.border_pressed : theme.button.border;
    gfx_color_t ink = enabled ? theme.button.text : theme.client.text_soft;
    gfx_fill_rect(s, r->x, r->y, r->w, r->h, fill);
    gfx_draw_rect(s, r->x, r->y, r->w, r->h, border);

    int32_t cx = r->x + r->w / 2, cy = r->y + r->h / 2;
    int32_t g = 12;                         // Glyph height
    switch (which) {
        case BTN_PREV:
            gfx_fill_rect(s, cx - 7, cy - g / 2, 2, g, ink);
            fill_triangle(s, cx - 5, cy - g / 2, 11, g, -1, ink);
            break;
        case BTN_NEXT:
            fill_triangle(s, cx - 6, cy - g / 2, 11, g, 1, ink);
            gfx_fill_rect(s, cx + 5, cy - g / 2, 2, g, ink);
            break;
        case BTN_PLAY:
            if (pl.state == P_PLAYING) {
                gfx_fill_rect(s, cx - 6, cy - g / 2, 4, g, ink);
                gfx_fill_rect(s, cx + 2, cy - g / 2, 4, g, ink);
            } else {
                fill_triangle(s, cx - 5, cy - g / 2 - 1, 11, g + 2, 1, ink);
            }
            break;
        case BTN_STOP:
            gfx_fill_rect(s, cx - 5, cy - 5, 10, 10, ink);
            break;
        case BTN_OPEN: {
            const char* t = "Open";
            gfx_draw_text(s, r->x + (r->w - gfx_text_width(s, t)) / 2, gfx_text_y(s, r->y, r->h), t, ink);
            break;
        }
    }
}

// `text` cut with ".." to fit `w` pixels
static void draw_fit(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, const char* text, gfx_color_t c) {
    if (gfx_text_width(s, text) <= w) {
        gfx_draw_text(s, x, y, text, c);
        return;
    }
    char buf[TRACK_TEXT_MAX * 2];
    strlcpy(buf, text, sizeof(buf));
    size_t n = strlen(buf);
    while (n > 0) {
        buf[--n] = 0;
        char t[sizeof(buf) + 3];
        snprintf(t, sizeof(t), "%s..", buf);
        if (gfx_text_width(s, t) <= w || n == 0) {
            gfx_draw_text(s, x, y, t, c);
            return;
        }
    }
}

static void draw_main(void) {
    dirty = false;
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    metrics_t m = get_metrics(s);
    int32_t text_w = m.width - 2 * PAD;
    gfx_clear(s, theme.client.background);

    // What is playing
    char line[TRACK_TEXT_MAX * 2 + 8];
    if (pl.loaded) {
        const char* title = pl.t.title[0] ? pl.t.title : base_name(list.paths[list.index]);
        gfx_set_text_scale(s, 1);
        draw_fit(s, PAD, m.title_y, text_w, title, theme.client.text);
        line[0] = 0;
        if (pl.t.artist[0] && pl.t.album[0])
            snprintf(line, sizeof(line), "%s - %s", pl.t.artist, pl.t.album);
        else if (pl.t.artist[0] || pl.t.album[0])
            strlcpy(line, pl.t.artist[0] ? pl.t.artist : pl.t.album, sizeof(line));
        else if (pl.t.title[0])
            strlcpy(line, base_name(list.paths[list.index]), sizeof(line));
        draw_fit(s, PAD, m.sub_y, text_w, line, theme.client.text_soft);
    } else {
        gfx_draw_text(s, PAD, m.title_y, "Nothing open", theme.client.text);
        draw_fit(s, PAD, m.sub_y, text_w, "Open an MP3 or WAV file, or drop one here", theme.client.text_soft);
    }

    // What it is, or a message
    char right[32] = "";
    if (list.count > 1)
        snprintf(right, sizeof(right), "%u of %u", (unsigned)list.index + 1, (unsigned)list.count);
    int32_t right_w = right[0] ? gfx_text_width(s, right) + 12 : 0;
    if (message[0]) {
        draw_fit(s, PAD, m.info_y, text_w - right_w, message,
                 message_bad ? theme.client.text_danger : theme.client.text_soft);
    } else if (pl.loaded) {
        const char* kind = pl.t.kind == TRACK_WAV ? "WAV" : pl.t.layer == 3 ? "MP3" : pl.t.layer == 2 ? "MP2" : "MP1";
        char rate[16];
        if (pl.t.rate % 1000 == 0)
            snprintf(rate, sizeof(rate), "%u kHz", (unsigned)(pl.t.rate / 1000));
        else
            snprintf(rate, sizeof(rate), "%u.%u kHz", (unsigned)(pl.t.rate / 1000), (unsigned)(pl.t.rate % 1000 / 100));
        char kbps[24] = "";
        if (pl.t.bitrate_kbps > 0)
            snprintf(kbps, sizeof(kbps), ", %s%d kbps", pl.t.vbr ? "~" : "", pl.t.bitrate_kbps);
        char res[32] = "";
        if (pl.fd >= 0 && pl.resample)
            snprintf(res, sizeof(res), " (played at %u Hz)", (unsigned)pl.out_rate);
        snprintf(line, sizeof(line), "%s, %s%s, %s%s", kind, rate, kbps,
                 pl.t.channels == 2 ? "stereo" : "mono", res);
        draw_fit(s, PAD, m.info_y, text_w - right_w, line, theme.client.text_soft);
    }
    if (right[0])
        gfx_draw_text(s, m.width - PAD - gfx_text_width(s, right), m.info_y, right, theme.client.text_soft);

    // Position
    double pos = dragging ? drag_pos : position();
    double dur = pl.loaded ? pl.t.duration : 0;
    gfx_fill_rect(s, m.bar.x, m.bar.y, m.bar.w, m.bar.h, theme.edit.background);
    gfx_draw_rect(s, m.bar.x, m.bar.y, m.bar.w, m.bar.h, theme.edit.border);
    int32_t fill = 0;
    if (dur > 0) {
        fill = (int32_t)((m.bar.w - 2) * (pos / dur));
        if (fill > m.bar.w - 2) fill = m.bar.w - 2;
        if (fill < 0) fill = 0;
        if (fill > 0)
            gfx_fill_rect(s, m.bar.x + 1, m.bar.y + 1, fill, m.bar.h - 2, theme.client.accent);
        // The knob
        gfx_fill_rect(s, m.bar.x + fill - 2 + 1, m.bar.y - 3, 5, m.bar.h + 6, theme.button.border_pressed);
    }
    shown_bar = fill;

    char a[16], b[16];
    format_time(pos, a, sizeof(a));
    gfx_draw_text(s, PAD, m.time_y, pl.loaded ? a : "", theme.client.text);
    if (pl.loaded) {
        if (dur > 0)
            format_time(dur, b, sizeof(b));
        else
            strlcpy(b, "--:--", sizeof(b));
        gfx_draw_text(s, m.width - PAD - gfx_text_width(s, b), m.time_y, b, theme.client.text_soft);
        const char* st = pl.state == P_PLAYING ? "Playing" : pl.state == P_PAUSED ? "Paused" : "Stopped";
        gfx_draw_text(s, (m.width - gfx_text_width(s, st)) / 2, m.time_y, st, theme.client.text_soft);
    }
    shown_second = (int)pos;

    for (int i = 0; i < BTN_COUNT; i++) {
        bool enabled = (i == BTN_OPEN) || pl.loaded || (i == BTN_NEXT && list.count > 1);
        draw_button(s, &m.btn[i], i, enabled);
    }
    wm_window_commit(main_win, NULL);
}

//
// The Open dialog
//

static void show_open_dialog(void) {
    if (dlg != NULL) {
        wm_window_focus(filedlg_window(dlg));
        return;
    }
    filedlg_desc_t d;
    memset(&d, 0, sizeof(d));
    d.mode = FILEDLG_OPEN;
    d.title = "Open sound file";
    d.path = (list.count > 0) ? list.paths[list.index] : NULL;
    d.owner = main_win;
    dlg = filedlg_open(&d);
    if (dlg == NULL)
        wm_notify_ex(WM_NOTIFY_ERROR, "Can't open the dialog", strerror(errno), 0);
}

//
// Events
//

static void do_command(uint32_t cmd) {
    switch (cmd) {
        case CMD_OPEN:   show_open_dialog(); break;
        case CMD_PLAY:   toggle_play();      break;
        case CMD_STOP:   stop();             break;
        case CMD_PREV:   prev_track();       break;
        case CMD_NEXT:   next_track(false);  break;
        case CMD_REPEAT: list.repeat = !list.repeat; dirty = true; break;
    }
}

static void press_button(int which) {
    switch (which) {
        case BTN_PREV: do_command(CMD_PREV); break;
        case BTN_PLAY: do_command(CMD_PLAY); break;
        case BTN_STOP: do_command(CMD_STOP); break;
        case BTN_NEXT: do_command(CMD_NEXT); break;
        case BTN_OPEN: do_command(CMD_OPEN); break;
    }
}

static double bar_seconds(const metrics_t* m, int32_t x) {
    double f = (double)(x - m->bar.x) / (m->bar.w > 1 ? m->bar.w - 1 : 1);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return f * pl.t.duration;
}

static bool on_bar(const metrics_t* m, int32_t x, int32_t y) {
    return x >= m->bar.x - 2 && x < m->bar.x + m->bar.w + 2 &&
           y >= m->bar.y - BAR_HIT && y < m->bar.y + m->bar.h + BAR_HIT;
}

static void main_key(const wm_event_t* ev) {
    uint16_t key = ev->key;
    bool ctrl = (ev->mods & INPUT_MOD_CTRL) != 0;
    if (ctrl) {
        char letter = 0;
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z'))
            letter = (char)(key | 0x20);
        else if (key >= 1 && key <= 26 && key != KEY_TAB)
            letter = (char)('a' + key - 1);
        if (letter == 'o') {
            show_open_dialog();
            return;
        }
    }
    switch (key) {
        case ' ':
        case 'k':
        case KEY_ENTER:     toggle_play(); break;
        case 's':
        case 'S':           stop(); break;
        case KEY_LEFT:      seek_to(position() - (ctrl ? SEEK_STEP_BIG : SEEK_STEP)); break;
        case KEY_RIGHT:     seek_to(position() + (ctrl ? SEEK_STEP_BIG : SEEK_STEP)); break;
        case KEY_HOME:      seek_to(0); break;
        case KEY_PAGE_UP:
        case 'p':
        case 'P':           prev_track(); break;
        case KEY_PAGE_DOWN:
        case 'n':
        case 'N':           next_track(false); break;
        case 'r':
        case 'R':
            list.repeat = !list.repeat;
            set_message(false, list.repeat ? "Repeat on" : "Repeat off");
            break;
        case 'o':
        case 'O':           show_open_dialog(); break;
    }
}

static void main_mouse_down(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return;
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    metrics_t m = get_metrics(s);
    for (int i = 0; i < BTN_COUNT; i++) {
        if (in_rect(&m.btn[i], ev->x, ev->y)) {
            pressed_btn = i;
            dirty = true;
            return;
        }
    }
    if (pl.loaded && pl.t.duration > 0 && on_bar(&m, ev->x, ev->y)) {
        dragging = true;
        drag_pos = bar_seconds(&m, ev->x);
        dirty = true;
    }
}

static void main_mouse_move(const wm_event_t* ev) {
    if (!dragging)
        return;
    if (!(ev->buttons & INPUT_BTN_LEFT)) {
        dragging = false;                   // Let go somewhere the window didn't see
        seek_to(drag_pos);
        return;
    }
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    metrics_t m = get_metrics(s);
    drag_pos = bar_seconds(&m, ev->x);
    dirty = true;
}

static void main_mouse_up(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return;
    if (dragging) {
        dragging = false;
        seek_to(drag_pos);
        return;
    }
    if (pressed_btn < 0)
        return;
    gfx_surface_t* s = wm_window_surface(main_win);
    int which = pressed_btn;
    pressed_btn = -1;
    dirty = true;
    if (s == NULL)
        return;
    metrics_t m = get_metrics(s);
    if (in_rect(&m.btn[which], ev->x, ev->y))
        press_button(which);
}

static void show_context_menu(int32_t x, int32_t y) {
    uint32_t has = pl.loaded ? 0 : WM_MENU_DISABLED;
    wm_menu_item_t items[] = {
        { "Open...",                                CMD_OPEN,   0 },
        { NULL,                                     0,          WM_MENU_SEPARATOR },
        { pl.state == P_PLAYING ? "Pause" : "Play", CMD_PLAY,   has },
        { "Stop",                                   CMD_STOP,   has },
        { "Previous",                               CMD_PREV,   list.count > 0 ? 0 : WM_MENU_DISABLED },
        { "Next",                                   CMD_NEXT,   list.count > 1 ? 0 : WM_MENU_DISABLED },
        { NULL,                                     0,          WM_MENU_SEPARATOR },
        { "Repeat",                                 CMD_REPEAT, list.repeat ? WM_MENU_CHECKED : 0 },
    };
    wm_menu_show(main_win, x, y, items, sizeof(items) / sizeof(items[0]));
}

static void main_drop(const wm_event_t* ev) {
    char* data = wm_drop_get(ev, NULL);
    if (data == NULL)
        return;
    char* files[PLAYLIST_MAX];
    size_t n = 0;
    for (char* p = data; *p && n < PLAYLIST_MAX;) {
        char* end = strchr(p, '\n');
        if (end != NULL)
            *end = 0;
        struct stat st;
        if (*p && stat(p, &st) == 0 && !S_ISDIR(st.st_mode))
            files[n++] = p;
        if (end == NULL)
            break;
        p = end + 1;
    }
    if (n > 0) {
        open_files(files, n);
        wm_window_focus(main_win);
    }
    free(data);
}

static void main_event(const wm_event_t* ev) {
    // While the dialog is up it has the input
    if (dlg != NULL && (ev->type == WM_EVENT_KEY || ev->type == WM_EVENT_MOUSE_DOWN ||
                        ev->type == WM_EVENT_CONTEXT_MENU)) {
        wm_window_focus(filedlg_window(dlg));
        return;
    }
    switch (ev->type) {
        case WM_EVENT_KEY:          main_key(ev);                    break;
        case WM_EVENT_MOUSE_DOWN:   main_mouse_down(ev);             break;
        case WM_EVENT_MOUSE_MOVE:   main_mouse_move(ev);             break;
        case WM_EVENT_MOUSE_UP:     main_mouse_up(ev);               break;
        case WM_EVENT_CONTEXT_MENU: show_context_menu(ev->x, ev->y); break;
        case WM_EVENT_MENU_COMMAND: do_command(ev->param);           break;
        case WM_EVENT_DROP:         main_drop(ev);                   break;

        case WM_EVENT_MOUSE_LEAVE:
            if (pressed_btn >= 0) {
                pressed_btn = -1;
                dirty = true;
            }
            break;

        case WM_EVENT_FOCUS_LOST:
            if (pressed_btn >= 0 || dragging) {
                pressed_btn = -1;
                dragging = false;           // The drag is dropped, the position stays
                dirty = true;
            }
            break;

        case WM_EVENT_RESIZED:
            dirty = true;
            break;

        case WM_EVENT_CLOSE:
            running = false;
            break;

        case WM_EVENT_DESTROYED:
            main_win = 0;
            running = false;
            break;
    }
}

//
// Main
//

int main(int argc, char** argv) {
    pl.fd = -1;
    pl.t.fd = -1;
    pl.dec = (int16_t*)malloc((size_t)TRACK_MAX_FRAMES * 4);
    pl.pend = (int16_t*)malloc((size_t)PEND_FRAMES * 4);
    if (pl.dec == NULL || pl.pend == NULL) {
        fprintf(stderr, APP_NAME ": out of memory\n");
        return 1;
    }
    load_theme();

    wm_window_desc_t d;
    memset(&d, 0, sizeof(d));
    d.title      = "Media player";
    d.x          = WM_POS_DEFAULT;
    d.y          = WM_POS_DEFAULT;
    d.width      = WIN_W;
    d.height     = WIN_H;
    d.min_width  = WIN_MIN_W;
    d.min_height = WIN_MIN_H;
    d.style      = WM_STYLE_RESIZABLE | WM_STYLE_CONTEXT_MENU;
    main_win = wm_window_create(&d);
    if (main_win == 0) {
        fprintf(stderr, APP_NAME ": can't create a window: %s\n", strerror(errno));
        wm_disconnect();
        return 1;
    }

    // Just tall enough for the text in the dwm's font and the buttons
    gfx_surface_t* surface = wm_window_surface(main_win);
    if (surface != NULL) {
        metrics_t m = get_metrics(surface);
        int32_t need = m.time_y + m.lh + 6 + BTN_H + PAD;
        if (need != m.height)
            wm_window_resize(main_win, m.width, need);
    }

    if (argc > 1)
        open_files(argv + 1, (size_t)(argc - 1 < PLAYLIST_MAX ? argc - 1 : PLAYLIST_MAX));
    draw_main();
    wm_window_focus(main_win);

    wm_event_t ev;
    while (running) {
        int32_t timeout = -1;
        if (dirty || (dlg != NULL && filedlg_dirty(dlg)))
            timeout = 0;
        else if (pl.state == P_PLAYING)
            timeout = TICK_MS;
        int r = wm_wait_event(&ev, timeout);
        if (r < 0)
            break;                          // The dwm went away
        tick();
        if (r == 0) {
            if (dirty)
                draw_main();
            if (dlg != NULL)
                filedlg_paint(dlg);
            continue;
        }

        if (dlg != NULL) {
            filedlg_result_t fr = filedlg_event(dlg, &ev);
            if (fr == FILEDLG_ACCEPT) {
                char path[PATH_MAX];
                strlcpy(path, filedlg_path(dlg), sizeof(path));
                filedlg_close(dlg);
                dlg = NULL;
                open_one(path);
                continue;
            }
            if (fr == FILEDLG_CANCEL) {
                filedlg_close(dlg);
                dlg = NULL;
                continue;
            }
            if (fr == FILEDLG_HANDLED)
                continue;
        }

        if (ev.type == WM_EVENT_THEME_CHANGED) {
            load_theme();
            dirty = true;
        } else if (ev.window == main_win) {
            main_event(&ev);
        }
    }

    unload();
    list_clear();
    wm_disconnect();
    return 0;
}

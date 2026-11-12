/*
 * The mixer (mixer.h). Each stream is a ring in shared memory that its
 * program fills (libc audio_mix.c); every few milliseconds the due part of
 * each running ring is scaled by its level, added up with clipping, and
 * written to /dev/audio0, keeping about TARGET_MS queued there.
 *
 * A stream's level and mute are kept by name in the registry,
 * hkey_user/control/audio/apps ("<level>,<mute>"), so a program comes back
 * at the level it had.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/audio.h>
#include <os/audio_mix.h>
#include <os/os.h>
#include <os/registry.h>
#include "mixer.h"

#define RING_FRAMES     16384           // Per stream: 64 KB, 341 ms at 48 kHz
#define TARGET_MS       100             // Kept queued on the device
#define CHUNK_MAX       4096            // Frames mixed at most per tick
#define START_FRAMES    1024            // The device starts at this many queued
#define TICK_PLAY_MS    10              // While something plays
#define TICK_IDLE_MS    20              // Streams open, none playing (state changes are seen this soon)
#define TICK_HOLD_MS    250             // Only the device held
#define RELEASE_MS      3000            // Device let go this long after the last sound
#define RETRY_MS        1000            // Device busy elsewhere: try again this often
#define SAVE_DELAY_MS   1000
#define APPS_KEY        "hkey_user/control/audio/apps"
#define DEFAULT_LEVEL   100

typedef struct {
    bool              used;
    bool              ready;            // HELLO answered: shm in place
    uint32_t          id;
    int               pipe;
    int               shm;
    audio_mix_shm_t*  h;
    int16_t*          ring;
    char              name[AUDIO_MIX_NAME_MAX];
    int               level;
    bool              mute;
    uint32_t          flush_seen;
    uint64_t          clock;            // Frames mixed while running, silence too
    uint64_t          real_end;         // Device frame after this stream's last real sample
    bool              starved;          // Ran dry: counted once until it has data again
    bool              was_running;
} stream_t;

static struct {
    bool       on;
    int        port;
    stream_t   s[MIXER_STREAMS];
    uint32_t   next_id;

    int        ctl;                     // /dev/audioctl0 for the info, -1
    audio_info_t info;
    bool       have_info;
    uint32_t   rate;

    int        dev;                     // /dev/audio0 while held, -1
    uint64_t   dev_end;                 // Frames we have written to it since taking it
    bool       busy;                    // Last open found it taken
    uint64_t   retry_at;
    uint64_t   quiet_since;             // Device held, nothing running since
    uint64_t   last_tick;
    uint64_t   save_at;
    bool       changed;

    int32_t    acc[CHUNK_MAX * 2];
    int16_t    out[CHUNK_MAX * 2];
} mx;

static uint64_t now(void) {
    return os_time_ms();
}

//
// Levels kept by name
//

static void reg_name(const char* name, char* out, size_t size) {
    size_t n = 0;
    for (; *name != '\0' && n + 1 < size; name++) {
        char c = *name;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == '_';
        out[n++] = ok ? c : '_';
    }
    out[n] = '\0';
}

static void level_load(stream_t* s) {
    char key[AUDIO_MIX_NAME_MAX], text[16];
    reg_name(s->name, key, sizeof(key));
    s->level = DEFAULT_LEVEL;
    s->mute  = false;
    if (key[0] == '\0' || os_reg_get_string(APPS_KEY, key, text, sizeof(text)) != 0)
        return;
    int v = atoi(text);
    if (v >= 0 && v <= 100)
        s->level = v;
    const char* comma = strchr(text, ',');
    s->mute = comma != NULL && atoi(comma + 1) != 0;
}

static void levels_save(void) {
    os_reg_mkkey("hkey_user/control/audio");
    os_reg_mkkey(APPS_KEY);
    for (int i = 0; i < MIXER_STREAMS; i++) {
        stream_t* s = &mx.s[i];
        if (!s->used || !s->ready)
            continue;
        char key[AUDIO_MIX_NAME_MAX], text[16];
        reg_name(s->name, key, sizeof(key));
        if (key[0] == '\0')
            continue;
        snprintf(text, sizeof(text), "%d,%d", s->level, s->mute ? 1 : 0);
        os_reg_set_string(APPS_KEY, key, text);
    }
}

static void publish_level(stream_t* s) {
    if (s->h != NULL) {
        s->h->level = (uint32_t)s->level;
        s->h->mute  = s->mute ? 1 : 0;
    }
}

//
// The device
//

static bool ctl_info(void) {
    if (mx.have_info)
        return true;
    if (mx.ctl < 0)
        mx.ctl = audio_ctl_open(0);
    if (mx.ctl < 0)
        return false;
    if (audio_info(mx.ctl, &mx.info) < 0) {
        audio_close(mx.ctl);
        mx.ctl = -1;
        return false;
    }
    mx.have_info = true;
    uint32_t r = mx.info.rates;
    mx.rate = (r & AUDIO_RATE_48000) ? 48000 : (r & AUDIO_RATE_44100) ? 44100 :
              (r & AUDIO_RATE_96000) ? 96000 : 48000;
    return true;
}

static void device_release(void) {
    if (mx.dev >= 0) {
        audio_stop(mx.dev);
        audio_close(mx.dev);
    }
    mx.dev = -1;
    for (int i = 0; i < MIXER_STREAMS; i++) {
        mx.s[i].real_end = 0;
        if (mx.s[i].h != NULL)
            mx.s[i].h->inflight = 0;
    }
}

static bool device_take(void) {
    if (mx.dev >= 0)
        return true;
    uint64_t t = now();
    if (t < mx.retry_at)
        return false;
    if (!ctl_info()) {
        mx.retry_at = t + RETRY_MS;
        return false;
    }
    int fd = audio_open_device(0);
    if (fd < 0) {
        bool was = mx.busy;
        mx.busy = (errno == EBUSY);
        if (mx.busy != was)
            mx.changed = true;
        mx.retry_at = t + RETRY_MS;
        return false;
    }
    audio_format_t f;
    memset(&f, 0, sizeof(f));
    f.rate        = mx.rate;
    f.channels    = 2;
    f.format      = AUDIO_FORMAT_S16;
    f.start_bytes = START_FRAMES * 4;
    if (audio_set_format_ex(fd, &f) < 0) {
        audio_close(fd);
        mx.retry_at = t + RETRY_MS;
        return false;
    }
    mx.dev = fd;
    mx.dev_end = 0;
    if (mx.busy)
        mx.changed = true;
    mx.busy = false;
    mx.quiet_since = t;
    for (int i = 0; i < MIXER_STREAMS; i++) {
        mx.s[i].real_end = 0;
        if (mx.s[i].h != NULL)
            mx.s[i].h->device_ok = 1;
    }
    return true;
}

//
// Streams
//

static void stream_drop(stream_t* s) {
    if (s->pipe > 0)
        ipc_close(s->pipe);
    if (s->shm > 0)
        ipc_close(s->shm);
    memset(s, 0, sizeof(*s));
    mx.changed = true;
}

static void welcome_error(int pipe, int err) {
    audio_mix_welcome_t w;
    memset(&w, 0, sizeof(w));
    w.type    = AUDIO_MIX_WELCOME;
    w.version = AUDIO_MIX_VERSION;
    w.error   = err;
    ipc_send(pipe, &w, sizeof(w), 0);
}

static void stream_hello(stream_t* s, const audio_mix_msg_t* m) {
    if (s->ready)
        return;
    if (!ctl_info()) {
        welcome_error(s->pipe, ENOENT);         // No sound device at all
        stream_drop(s);
        return;
    }
    size_t bytes = AUDIO_MIX_HEADER + (size_t)RING_FRAMES * 4;
    int shm = shm_create(bytes);
    audio_mix_shm_t* h = (shm > 0) ? (audio_mix_shm_t*)shm_map(shm) : NULL;
    if (h == NULL) {
        if (shm > 0)
            ipc_close(shm);
        welcome_error(s->pipe, ENOMEM);
        stream_drop(s);
        return;
    }
    memset(h, 0, AUDIO_MIX_HEADER);
    h->magic       = AUDIO_MIX_MAGIC;
    h->version     = AUDIO_MIX_VERSION;
    h->ring_frames = RING_FRAMES;
    h->rate        = mx.rate;
    h->state       = AUDIO_STATE_STOPPED;
    h->device_ok   = mx.busy ? 0 : 1;

    s->shm  = shm;
    s->h    = h;
    s->ring = (int16_t*)((uint8_t*)h + AUDIO_MIX_HEADER);
    snprintf(s->name, sizeof(s->name), "%.*s", AUDIO_MIX_NAME_MAX - 1, m->name[0] ? m->name : "program");
    s->name[sizeof(s->name) - 1] = '\0';
    level_load(s);
    publish_level(s);

    audio_mix_welcome_t w;
    memset(&w, 0, sizeof(w));
    w.type        = AUDIO_MIX_WELCOME;
    w.version     = AUDIO_MIX_VERSION;
    w.ring_frames = RING_FRAMES;
    w.rate        = mx.rate;
    w.info        = mx.info;
    w.info.rates        = audio_rate_bit(mx.rate);
    w.info.formats      = AUDIO_FORMAT_S16;
    w.info.channels_min = 2;
    w.info.channels_max = 2;
    w.info.hw_volume    = 1;                    // The stream's level has steps
    w.info.buffer_bytes = RING_FRAMES * 4;
    if (ipc_send(s->pipe, &w, sizeof(w), shm) < 0) {
        stream_drop(s);
        return;
    }
    s->ready = true;
    mx.changed = true;
}

static void stream_messages(stream_t* s) {
    for (int i = 0; i < 16 && s->used; i++) {
        audio_mix_msg_t m;
        ipc_msg_info_t info;
        ssize_t n = ipc_recv(s->pipe, &m, sizeof(m), &info);
        if (n < 0) {
            if (errno == EAGAIN)
                return;
            stream_drop(s);                     // EPIPE: the program is done (or gone)
            return;
        }
        if (info.handle > 0)
            ipc_close(info.handle);             // Nothing of ours
        if ((size_t)n < sizeof(m))
            continue;
        m.name[AUDIO_MIX_NAME_MAX - 1] = '\0';
        switch (m.type) {
            case AUDIO_MIX_HELLO:
                stream_hello(s, &m);
                break;
            case AUDIO_MIX_VOLUME:
                if (s->ready) {
                    s->level = m.level > 100 ? 100 : (int)m.level;
                    s->mute  = m.mute != 0;
                    publish_level(s);
                    mx.save_at = now() + SAVE_DELAY_MS;
                    mx.changed = true;
                }
                break;
            case AUDIO_MIX_NAME:
                if (s->ready && m.name[0] != '\0') {
                    snprintf(s->name, sizeof(s->name), "%s", m.name);
                    level_load(s);              // Its own level, under its new name
                    publish_level(s);
                    mx.changed = true;
                }
                break;
            default:
                break;
        }
    }
}

static void accept_new(void) {
    for (;;) {
        int pipe = ipc_accept(mx.port);
        if (pipe < 0)
            return;
        stream_t* s = NULL;
        for (int i = 0; i < MIXER_STREAMS; i++) {
            if (!mx.s[i].used) {
                s = &mx.s[i];
                break;
            }
        }
        if (s == NULL) {
            welcome_error(pipe, EBUSY);
            ipc_close(pipe);
            continue;
        }
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->pipe = pipe;
        s->id   = ++mx.next_id;
    }
}

static bool running(const stream_t* s) {
    return s->used && s->ready && s->h->state == AUDIO_STATE_RUNNING;
}

// What a stop asked for: the queue dropped
static void flushes(void) {
    for (int i = 0; i < MIXER_STREAMS; i++) {
        stream_t* s = &mx.s[i];
        if (!s->used || !s->ready)
            continue;
        uint32_t req = s->h->flush_req;
        if (req == s->h->flush_ack)
            continue;
        __sync_synchronize();
        s->h->read_pos = s->h->write_pos;
        s->h->inflight = 0;
        s->starved = false;
        // Alone on the device: what it still holds goes too
        bool others = false;
        for (int j = 0; j < MIXER_STREAMS; j++)
            others = others || (j != i && mx.s[j].used && mx.s[j].ready && mx.s[j].real_end > 0 &&
                                mx.s[j].h->inflight > 0);
        if (!others && mx.dev >= 0 && s->real_end > 0)
            audio_stop(mx.dev);
        s->real_end = 0;
        __sync_synchronize();
        s->h->flush_ack = req;
    }
}

static uint32_t gain_of(const stream_t* s) {
    if (s->mute || s->level <= 0)
        return 0;
    // Squared, so the slider sounds even: 50 % is about -12 dB
    return (uint32_t)(s->level * s->level) * 65536u / 10000u;
}

// n frames of every running stream into mx.out; per-stream bookkeeping
// with the device at frame `dev_at` (where this chunk will start)
static void mix_chunk(uint32_t n, uint64_t dev_at) {
    memset(mx.acc, 0, (size_t)n * 2 * sizeof(int32_t));
    for (int i = 0; i < MIXER_STREAMS; i++) {
        stream_t* s = &mx.s[i];
        if (!running(s))
            continue;
        audio_mix_shm_t* h = s->h;
        uint32_t r = h->read_pos;
        uint32_t avail = h->write_pos - r;
        __sync_synchronize();                   // The samples behind write_pos are in
        if (avail > RING_FRAMES)
            avail = 0;                          // Nonsense from the program: take nothing
        uint32_t take = avail < n ? avail : n;
        uint32_t g = gain_of(s);
        for (uint32_t k = 0; k < take; k++) {
            uint32_t at = ((r + k) & (RING_FRAMES - 1)) * 2;
            if (g != 0) {
                mx.acc[k * 2]     += (int32_t)(((int64_t)s->ring[at] * g) >> 16);
                mx.acc[k * 2 + 1] += (int32_t)(((int64_t)s->ring[at + 1] * g) >> 16);
            }
        }
        __sync_synchronize();
        h->read_pos = r + take;
        if (take > 0)
            s->real_end = dev_at + take;
        if (take == n) {
            s->starved = false;
        } else if (!s->starved && s->clock > 0) {
            s->starved = true;                  // Ran dry while running: silence fills in
            h->underruns++;
        }
        s->clock += n;
    }
    for (uint32_t k = 0; k < n * 2; k++) {
        int32_t v = mx.acc[k];
        mx.out[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

// Without the device: running streams still move, in real time, so the
// programs feeding them don't wait forever
static void drop_in_time(uint64_t elapsed_ms) {
    uint64_t frames = elapsed_ms * mx.rate / 1000;
    if (frames > RING_FRAMES)
        frames = RING_FRAMES;
    for (int i = 0; i < MIXER_STREAMS; i++) {
        stream_t* s = &mx.s[i];
        if (!running(s))
            continue;
        uint32_t avail = s->h->write_pos - s->h->read_pos;
        uint32_t take = avail < frames ? avail : (uint32_t)frames;
        s->h->read_pos += take;
        s->h->inflight = 0;
        s->clock += frames;
        s->h->played = (uint32_t)s->clock;
    }
}

static void play(void) {
    uint64_t t = now();
    uint64_t elapsed = mx.last_tick ? t - mx.last_tick : 0;
    mx.last_tick = t;

    bool any_running = false, any_paused = false;
    for (int i = 0; i < MIXER_STREAMS; i++) {
        stream_t* s = &mx.s[i];
        if (!s->used || !s->ready)
            continue;
        bool r = running(s);
        if (r != s->was_running) {
            s->was_running = r;
            mx.changed = true;
        }
        any_running = any_running || r;
        any_paused = any_paused || s->h->state == AUDIO_STATE_PAUSED;
    }

    if (any_running && !device_take()) {
        drop_in_time(elapsed);
        return;
    }
    if (mx.dev < 0)
        return;

    audio_status_t st;
    if (audio_status(mx.dev, &st) < 0) {
        device_release();                       // Gone: open again when needed
        mx.retry_at = t + RETRY_MS;
        return;
    }
    // Our own count of what went to the device, and how much of it is
    // still queued there: the device's played / written counters drift
    // apart over a stop or while it runs dry
    uint64_t dev_w = mx.dev_end;
    uint32_t queued = st.queued_bytes / 4;
    if (queued > dev_w)
        queued = (uint32_t)dev_w;

    // A pause with nobody else playing holds the device too, so what it
    // still has comes out on resume; a resume lets it go
    if (!any_running && any_paused && st.state == AUDIO_STATE_RUNNING && queued > 0)
        audio_pause(mx.dev);
    else if (any_running && st.state == AUDIO_STATE_PAUSED)
        audio_start(mx.dev);

    uint32_t target = mx.rate * TARGET_MS / 1000;
    if (any_running && queued < target) {
        uint32_t n = target - queued;
        if (n > CHUNK_MAX)
            n = CHUNK_MAX;
        if (n * 4 > st.free_bytes)
            n = st.free_bytes / 4;
        if (n > 0) {
            mix_chunk(n, dev_w);
            long w = audio_write(mx.dev, mx.out, (size_t)n * 4);
            if (w < 0) {
                device_release();
                mx.retry_at = t + RETRY_MS;
                return;
            }
            dev_w += (uint64_t)w / 4;
            queued += (uint32_t)w / 4;
            mx.dev_end = dev_w;
        }
    }

    // What each stream has on the device still, and its clock as heard
    uint64_t dev_p = dev_w - queued;           // The frame playing now
    uint64_t latency = queued;
    for (int i = 0; i < MIXER_STREAMS; i++) {
        stream_t* s = &mx.s[i];
        if (!s->used || !s->ready)
            continue;
        uint64_t in = s->real_end > dev_p ? s->real_end - dev_p : 0;
        if (in > queued)
            in = queued;
        s->h->inflight = (uint32_t)in;
        uint64_t heard = s->clock > latency ? s->clock - latency : 0;
        if ((uint32_t)heard - s->h->played < 0x80000000u)       // Never backwards
            s->h->played = (uint32_t)heard;
    }

    if (any_running || queued > 0)
        mx.quiet_since = t;
    else if (!any_paused && t - mx.quiet_since >= RELEASE_MS)
        device_release();                       // Free for a program that wants it directly
}

//
// Interface
//

bool mixer_init(void) {
    memset(&mx, 0, sizeof(mx));
    mx.ctl = -1;
    mx.dev = -1;
    mx.rate = 48000;
    mx.port = ipc_port_create(AUDIO_MIX_PORT);
    if (mx.port < 0)
        return false;
    mx.on = true;
    ctl_info();
    return true;
}

void mixer_shutdown(void) {
    if (!mx.on)
        return;
    if (mx.save_at != 0)
        levels_save();
    for (int i = 0; i < MIXER_STREAMS; i++)
        if (mx.s[i].used)
            stream_drop(&mx.s[i]);
    device_release();
    if (mx.ctl >= 0)
        audio_close(mx.ctl);
    ipc_close(mx.port);
    mx.on = false;
}

size_t mixer_wait_items(ipc_wait_item_t* items, size_t max) {
    if (!mx.on)
        return 0;
    size_t n = 0;
    if (n < max) {
        items[n].handle = mx.port;
        items[n].events = IPC_READABLE;
        items[n].ready  = 0;
        n++;
    }
    for (int i = 0; i < MIXER_STREAMS && n < max; i++) {
        if (!mx.s[i].used)
            continue;
        items[n].handle = mx.s[i].pipe;
        items[n].events = IPC_READABLE;
        items[n].ready  = 0;
        n++;
    }
    return n;
}

int32_t mixer_timeout(void) {
    if (!mx.on)
        return -1;
    bool streams = false, playing = false;
    for (int i = 0; i < MIXER_STREAMS; i++) {
        if (!mx.s[i].used || !mx.s[i].ready)
            continue;
        streams = true;
        playing = playing || running(&mx.s[i]);
    }
    int32_t t = -1;
    if (playing)
        t = TICK_PLAY_MS;
    else if (streams)
        t = TICK_IDLE_MS;
    else if (mx.dev >= 0)
        t = TICK_HOLD_MS;
    if (mx.save_at != 0) {
        uint64_t now_ms = now();
        int32_t s = mx.save_at <= now_ms ? 0 : (int32_t)(mx.save_at - now_ms);
        if (t < 0 || s < t)
            t = s;
    }
    return t;
}

bool mixer_tick(void) {
    if (!mx.on)
        return false;
    accept_new();
    for (int i = 0; i < MIXER_STREAMS; i++)
        if (mx.s[i].used)
            stream_messages(&mx.s[i]);
    flushes();
    play();
    if (mx.save_at != 0 && now() >= mx.save_at) {
        mx.save_at = 0;
        levels_save();
    }
    bool c = mx.changed;
    mx.changed = false;
    return c;
}

size_t mixer_streams(mixer_stream_info_t* out, size_t max) {
    size_t n = 0;
    // Oldest first: ids grow
    uint32_t last = 0;
    for (;;) {
        stream_t* next = NULL;
        for (int i = 0; i < MIXER_STREAMS; i++) {
            stream_t* s = &mx.s[i];
            if (s->used && s->ready && s->id > last && (next == NULL || s->id < next->id))
                next = s;
        }
        if (next == NULL || n >= max)
            break;
        out[n].id = next->id;
        snprintf(out[n].name, sizeof(out[n].name), "%s", next->name);
        out[n].level   = next->level;
        out[n].mute    = next->mute;
        out[n].playing = running(next);
        last = next->id;
        n++;
    }
    return n;
}

void mixer_set_level(uint32_t id, int level, bool mute) {
    if (level < 0)   level = 0;
    if (level > 100) level = 100;
    for (int i = 0; i < MIXER_STREAMS; i++) {
        stream_t* s = &mx.s[i];
        if (s->used && s->ready && s->id == id) {
            s->level = level;
            s->mute  = mute;
            publish_level(s);
            mx.save_at = now() + SAVE_DELAY_MS;
            return;
        }
    }
}

const char* mixer_problem(void) {
    return mx.busy ? "Sound device in use by another program" : NULL;
}

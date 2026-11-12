/*
 * Sound streams through the volume service's mixer (os/audio_mix.h).
 *
 * audio_open(0) comes here first: while audiosrv runs, a program gets a
 * stream of its own on the mixer instead of the device, so several play at
 * once, each with its own level on the service's panel. The descriptor is
 * one of ours (AUDIO_MIX_FD_BASE and up), and every audio_* call in
 * audio.c hands it here. The calls behave as on the device:
 *
 *   - the format is S16 stereo at the mix rate (audio_info().rates has
 *     just that one); anything else is EINVAL
 *   - writes go into a ring in shared memory and never block; the stream
 *     starts at start_bytes queued or at audio_start, pauses, stops and
 *     drains as the device does, and stops by itself a second after it
 *     runs dry
 *   - played_frames counts frames played while running (silence too),
 *     queued_bytes includes what the service has sent on to the device
 *   - audio_set_volume / audio_get_volume are the stream's own level, the
 *     one on the panel (the master is the codec's, on /dev/audioctl0)
 *
 * If the service goes away the calls fail with EPIPE.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/audio.h>
#include <os/audio_mix.h>
#include <os/ipc.h>
#include <os/os.h>
#include "audio_internal.h"

#define MIX_MAX             4           // Streams per process
#define FLUSH_WAIT_MS       250
#define WELCOME_WAIT_MS     2000

typedef struct {
    bool              used;
    int               pipe;
    int               shm;
    audio_mix_shm_t*  h;
    int16_t*          ring;
    uint32_t          mask;             // ring_frames - 1
    audio_info_t      info;
    uint32_t          rate;
    uint32_t          start_frames;
    uint32_t          base_write;       // write_pos at the last set_format / stop
    uint64_t          played;           // h->played, widened
    uint32_t          played_seen;
    uint64_t          base_played;
    uint64_t          busy_at;          // Last time it had something queued
} mix_stream_t;

static mix_stream_t mix[MIX_MAX];

static mix_stream_t* get(int fd) {
    int i = fd - AUDIO_MIX_FD_BASE;
    if (i < 0 || i >= MIX_MAX || !mix[i].used) {
        errno = EBADF;
        return NULL;
    }
    return &mix[i];
}

bool __audio_mix_fd(int fd) {
    return fd >= AUDIO_MIX_FD_BASE && fd < AUDIO_MIX_FD_BASE + MIX_MAX;
}

// The service still there? Its end closing means the stream is gone.
static bool alive(mix_stream_t* s) {
    int r = ipc_wait_one(s->pipe, 0, 0);
    if (r > 0 && (r & IPC_HANGUP)) {
        errno = EPIPE;
        return false;
    }
    return true;
}

static const char* program_name(void) {
    const char* n = __libc_argv0;
    if (n == NULL || n[0] == '\0')
        return "program";
    const char* slash = strrchr(n, '/');
    return slash != NULL ? slash + 1 : n;
}

int __audio_mix_open(void) {
    int slot = -1;
    for (int i = 0; i < MIX_MAX; i++) {
        if (!mix[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        errno = EMFILE;
        return -1;
    }

    int pipe = ipc_connect(AUDIO_MIX_PORT);
    if (pipe < 0)
        return -1;                      // ENOENT: no service; EPERM: no ipc

    audio_mix_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = AUDIO_MIX_HELLO;
    m.version = AUDIO_MIX_VERSION;
    snprintf(m.name, sizeof(m.name), "%s", program_name());
    if (ipc_send(pipe, &m, sizeof(m), 0) < 0) {
        int err = errno;
        ipc_close(pipe);
        errno = err;
        return -1;
    }

    audio_mix_welcome_t w;
    ipc_msg_info_t mi;
    ssize_t n = -1;
    uint64_t until = os_time_ms() + WELCOME_WAIT_MS;
    for (;;) {
        int r = ipc_wait_one(pipe, IPC_READABLE, WELCOME_WAIT_MS);
        if (r > 0 && (r & IPC_READABLE)) {
            n = ipc_recv(pipe, &w, sizeof(w), &mi);
            break;
        }
        if (r < 0 || (r & IPC_HANGUP) || os_time_ms() >= until)
            break;
    }
    if (n != (ssize_t)sizeof(w) || w.type != AUDIO_MIX_WELCOME || w.error != 0 || mi.handle <= 0) {
        if (n > 0 && mi.handle > 0)
            ipc_close(mi.handle);
        ipc_close(pipe);
        errno = (n == (ssize_t)sizeof(w) && w.error != 0) ? w.error : EIO;
        return -1;
    }

    audio_mix_shm_t* h = (audio_mix_shm_t*)shm_map(mi.handle);
    uint32_t frames = w.ring_frames;
    if (h == NULL || h->magic != AUDIO_MIX_MAGIC || frames == 0 || (frames & (frames - 1)) != 0 ||
        shm_size(mi.handle) < AUDIO_MIX_HEADER + (size_t)frames * 4) {
        ipc_close(mi.handle);
        ipc_close(pipe);
        errno = EIO;
        return -1;
    }

    mix_stream_t* s = &mix[slot];
    memset(s, 0, sizeof(*s));
    s->used  = true;
    s->pipe  = pipe;
    s->shm   = mi.handle;
    s->h     = h;
    s->ring  = (int16_t*)((uint8_t*)h + AUDIO_MIX_HEADER);
    s->mask  = frames - 1;
    s->info  = w.info;
    s->rate  = w.rate;
    s->start_frames = frames / 8;
    s->base_write   = h->write_pos;
    s->played_seen  = h->played;
    return AUDIO_MIX_FD_BASE + slot;
}

int __audio_mix_close(int fd) {
    mix_stream_t* s = get(fd);
    if (s == NULL)
        return -1;
    ipc_close(s->pipe);                 // The service drops the stream
    ipc_close(s->shm);
    memset(s, 0, sizeof(*s));
    return 0;
}

int __audio_mix_info(int fd, audio_info_t* info) {
    mix_stream_t* s = get(fd);
    if (s == NULL)
        return -1;
    *info = s->info;
    return 0;
}

static uint32_t queued_ring(const mix_stream_t* s) {
    return s->h->write_pos - s->h->read_pos;
}

static void update_played(mix_stream_t* s) {
    uint32_t p = s->h->played;
    s->played += (uint32_t)(p - s->played_seen);
    s->played_seen = p;
}

// Drop everything queued: the service does it, we wait for it to say so
static int flush(mix_stream_t* s) {
    s->h->state = AUDIO_STATE_STOPPED;
    __sync_synchronize();
    uint32_t req = s->h->flush_req + 1;
    s->h->flush_req = req;
    __sync_synchronize();
    uint64_t until = os_time_ms() + FLUSH_WAIT_MS;
    while (s->h->flush_ack != req) {
        if (!alive(s))
            return -1;
        if (os_time_ms() >= until)
            break;                      // Late: carry on, it catches up
        os_sleep_ms(2);
    }
    update_played(s);
    s->base_write  = s->h->write_pos;
    s->base_played = s->played;
    return 0;
}

int __audio_mix_set_format(int fd, const audio_format_t* f) {
    mix_stream_t* s = get(fd);
    if (s == NULL)
        return -1;
    if (f->rate != s->rate || f->channels != 2 || f->format != AUDIO_FORMAT_S16) {
        errno = EINVAL;
        return -1;
    }
    uint32_t ring_bytes = (s->mask + 1) * 4;
    uint32_t start = f->start_bytes;
    if (start == 0)
        start = ring_bytes / 8;
    if (start > ring_bytes)
        start = ring_bytes;
    if (flush(s) < 0)
        return -1;
    s->start_frames = start / 4 ? start / 4 : 1;
    return 0;
}

int __audio_mix_get_format(int fd, audio_format_t* f) {
    mix_stream_t* s = get(fd);
    if (s == NULL)
        return -1;
    memset(f, 0, sizeof(*f));
    f->version     = AUDIO_FORMAT_VERSION;
    f->rate        = s->rate;
    f->channels    = 2;
    f->format      = AUDIO_FORMAT_S16;
    f->start_bytes = s->start_frames * 4;
    return 0;
}

long __audio_mix_write(int fd, const void* data, size_t bytes) {
    mix_stream_t* s = get(fd);
    if (s == NULL)
        return -1;
    if (!alive(s))
        return -1;
    uint32_t size = s->mask + 1;
    uint32_t room = size - queued_ring(s);
    uint32_t n = (uint32_t)(bytes / 4);
    if (n > room)
        n = room;
    const int16_t* src = (const int16_t*)data;
    uint32_t w = s->h->write_pos;
    uint32_t at = w & s->mask;
    uint32_t first = size - at;
    if (first > n)
        first = n;
    memcpy(s->ring + at * 2, src, (size_t)first * 4);
    if (n > first)
        memcpy(s->ring, src + first * 2, (size_t)(n - first) * 4);
    __sync_synchronize();               // The samples before the position that shows them
    s->h->write_pos = w + n;
    if (n > 0)
        s->busy_at = os_time_ms();
    if (s->h->state == AUDIO_STATE_STOPPED && queued_ring(s) >= s->start_frames)
        s->h->state = AUDIO_STATE_RUNNING;
    return (long)n * 4;
}

int __audio_mix_status(int fd, audio_status_t* st) {
    mix_stream_t* s = get(fd);
    if (s == NULL)
        return -1;
    if (!alive(s))
        return -1;
    update_played(s);
    uint32_t ring = queued_ring(s);
    uint32_t queued = ring + s->h->inflight;
    uint64_t t = os_time_ms();
    if (queued > 0)
        s->busy_at = t;
    else if (s->h->state == AUDIO_STATE_RUNNING && t - s->busy_at > AUDIO_IDLE_STOP_MS)
        s->h->state = AUDIO_STATE_STOPPED;      // As the device does when it runs dry

    memset(st, 0, sizeof(*st));
    st->version        = AUDIO_STATUS_VERSION;
    st->state          = (uint8_t)s->h->state;
    st->headphone      = AUDIO_JACK_UNKNOWN;
    st->frame_bytes    = 4;
    st->queued_bytes   = queued * 4;
    st->free_bytes     = (s->mask + 1 - ring) * 4;
    st->buffer_bytes   = (s->mask + 1) * 4;
    st->underruns      = s->h->underruns;
    st->played_frames  = s->played - s->base_played;
    st->written_frames = (uint32_t)(s->h->write_pos - s->base_write);
    return 0;
}

int __audio_mix_start(int fd) {
    mix_stream_t* s = get(fd);
    if (s == NULL || !alive(s))
        return -1;
    s->busy_at = os_time_ms();
    s->h->state = AUDIO_STATE_RUNNING;
    return 0;
}

int __audio_mix_pause(int fd) {
    mix_stream_t* s = get(fd);
    if (s == NULL || !alive(s))
        return -1;
    if (s->h->state == AUDIO_STATE_RUNNING)
        s->h->state = AUDIO_STATE_PAUSED;
    return 0;
}

int __audio_mix_stop(int fd) {
    mix_stream_t* s = get(fd);
    if (s == NULL)
        return -1;
    return flush(s);
}

int __audio_mix_set_volume(int fd, int left, int right, int mute) {
    mix_stream_t* s = get(fd);
    if (s == NULL)
        return -1;
    audio_mix_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type    = AUDIO_MIX_VOLUME;
    m.version = AUDIO_MIX_VERSION;
    m.level   = (uint32_t)(left > right ? left : right);
    m.mute    = mute ? 1 : 0;
    return ipc_send(s->pipe, &m, sizeof(m), 0) < 0 ? -1 : 0;
}

int __audio_mix_get_volume(int fd, audio_volume_t* v) {
    mix_stream_t* s = get(fd);
    if (s == NULL)
        return -1;
    memset(v, 0, sizeof(*v));
    v->version = AUDIO_VOLUME_VERSION;
    v->left = v->right = (uint8_t)(s->h->level > 100 ? 100 : s->h->level);
    v->mute = s->h->mute ? 1 : 0;
    return 0;
}

int audio_set_name(int fd, const char* name) {
    if (!__audio_mix_fd(fd))
        return 0;                       // The device has no names: nothing to do
    mix_stream_t* s = get(fd);
    if (s == NULL)
        return -1;
    if (name == NULL || name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    audio_mix_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type    = AUDIO_MIX_NAME;
    m.version = AUDIO_MIX_VERSION;
    snprintf(m.name, sizeof(m.name), "%s", name);
    return ipc_send(s->pipe, &m, sizeof(m), 0) < 0 ? -1 : 0;
}

int audio_is_mixed(int fd) {
    return __audio_mix_fd(fd) && get(fd) != NULL;
}

/*
 * Sound output (os/audio.h) on /dev/audioN: open, write and ioctl. A
 * descriptor from the volume service's mixer goes to audio_mix.c instead.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <os/audio.h>
#include <os/os.h>
#include "audio_internal.h"

int audio_open(int index) {
    if (index == 0) {
        // The volume service's mixer when it runs; the device itself when
        // it doesn't (or we may not talk to it)
        int saved = errno;
        int fd = __audio_mix_open();
        if (fd >= 0)
            return fd;
        errno = saved;
    }
    return audio_open_device(index);
}

int audio_open_device(int index) {
    if (index < 0 || index > 99) {
        errno = EINVAL;
        return -1;
    }
    char path[24];
    snprintf(path, sizeof(path), "/dev/audio%d", index);
    int fd = open(path, O_WRONLY);
    if (fd < 0 && errno != ENOENT && errno != EPERM && errno != EACCES) {
        // The kernel's open says only that it failed; for a device that is
        // there, the driver's one reason to refuse is another handle
        struct stat st;
        if (stat(path, &st) == 0)
            errno = EBUSY;
    }
    return fd;
}

int audio_ctl_open(int index) {
    if (index < 0 || index > 99) {
        errno = EINVAL;
        return -1;
    }
    char path[24];
    snprintf(path, sizeof(path), "/dev/audioctl%d", index);
    return open(path, O_RDONLY);
}

int audio_close(int fd) {
    if (__audio_mix_fd(fd))
        return __audio_mix_close(fd);
    return close(fd);
}

int audio_info(int fd, audio_info_t* info) {
    if (info == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (__audio_mix_fd(fd))
        return __audio_mix_info(fd, info);
    return ioctl(fd, AUDIO_IOCTL_INFO, info);
}

int audio_set_format_ex(int fd, const audio_format_t* format) {
    if (format == NULL) {
        errno = EINVAL;
        return -1;
    }
    audio_format_t f = *format;
    f.version = AUDIO_FORMAT_VERSION;
    f.reserved = 0;
    if (__audio_mix_fd(fd))
        return __audio_mix_set_format(fd, &f);
    return ioctl(fd, AUDIO_IOCTL_SET_FORMAT, &f);
}

int audio_set_format(int fd, uint32_t rate, int channels, int format) {
    if (channels <= 0 || channels > 0xFFFF || format <= 0 || format > 0xFFFF) {
        errno = EINVAL;
        return -1;
    }
    audio_format_t f;
    memset(&f, 0, sizeof(f));
    f.rate     = rate;
    f.channels = (uint16_t)channels;
    f.format   = (uint16_t)format;
    return audio_set_format_ex(fd, &f);
}

int audio_get_format(int fd, audio_format_t* format) {
    if (format == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (__audio_mix_fd(fd))
        return __audio_mix_get_format(fd, format);
    return ioctl(fd, AUDIO_IOCTL_GET_FORMAT, format);
}

long audio_write(int fd, const void* data, size_t bytes) {
    if (bytes == 0)
        return 0;
    if (data == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (__audio_mix_fd(fd))
        return __audio_mix_write(fd, data, bytes);
    return (long)write(fd, data, bytes);
}

int audio_status(int fd, audio_status_t* status) {
    if (status == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (__audio_mix_fd(fd))
        return __audio_mix_status(fd, status);
    return ioctl(fd, AUDIO_IOCTL_STATUS, status);
}

// How long until `bytes` of room appear, from the format: at least 1 ms,
// at most 20 so a stop elsewhere is noticed soon
static uint32_t audio_wait_ms(int fd, size_t bytes) {
    audio_format_t f;
    if (audio_get_format(fd, &f) < 0 || f.rate == 0)
        return 5;
    int frame = audio_frame_bytes(f.format, f.channels);
    if (frame <= 0)
        return 5;
    uint64_t ms = (uint64_t)bytes * 1000u / ((uint64_t)f.rate * (uint64_t)frame);
    if (ms < 1)  ms = 1;
    if (ms > 20) ms = 20;
    return (uint32_t)ms;
}

long audio_write_all(int fd, const void* data, size_t bytes) {
    if (bytes == 0)
        return 0;
    if (data == NULL) {
        errno = EINVAL;
        return -1;
    }
    audio_format_t f;
    if (audio_get_format(fd, &f) < 0)
        return -1;
    size_t frame = (size_t)audio_frame_bytes(f.format, f.channels);
    if (frame == 0) {
        errno = EINVAL;
        return -1;
    }
    bytes -= bytes % frame;         // A part frame at the end can never be taken

    const uint8_t* p = (const uint8_t*)data;
    size_t done = 0;
    while (done < bytes) {
        long n = audio_write(fd, p + done, bytes - done);
        if (n < 0)
            return (done > 0) ? (long)done : -1;
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        // Full. If it isn't playing (paused, or start_bytes not reached
        // because they are more than the room left), it never empties:
        // start it.
        audio_status_t s;
        if (audio_status(fd, &s) < 0)
            return (done > 0) ? (long)done : -1;
        if (s.state != AUDIO_STATE_RUNNING) {
            if (audio_start(fd) < 0)
                return (done > 0) ? (long)done : -1;
            continue;
        }
        size_t want = bytes - done;
        if (want > s.buffer_bytes / 4)
            want = s.buffer_bytes / 4;
        os_sleep_ms(audio_wait_ms(fd, want));
    }
    return (long)done;
}

int audio_start(int fd) { return __audio_mix_fd(fd) ? __audio_mix_start(fd) : ioctl(fd, AUDIO_IOCTL_START, 0); }
int audio_pause(int fd) { return __audio_mix_fd(fd) ? __audio_mix_pause(fd) : ioctl(fd, AUDIO_IOCTL_PAUSE, 0); }
int audio_stop(int fd)  { return __audio_mix_fd(fd) ? __audio_mix_stop(fd)  : ioctl(fd, AUDIO_IOCTL_STOP, 0); }

int audio_drain(int fd) {
    audio_status_t s;
    if (audio_status(fd, &s) < 0)
        return -1;
    if (s.queued_bytes == 0)
        return 0;
    if (s.state != AUDIO_STATE_RUNNING && audio_start(fd) < 0)
        return -1;
    for (;;) {
        if (audio_status(fd, &s) < 0)
            return -1;
        if (s.queued_bytes == 0 || s.state != AUDIO_STATE_RUNNING)
            return 0;
        os_sleep_ms(audio_wait_ms(fd, s.queued_bytes));
    }
}

int audio_set_volume(int fd, int left, int right, int mute) {
    if (left < 0 || left > 100 || right < 0 || right > 100) {
        errno = EINVAL;
        return -1;
    }
    audio_volume_t v;
    memset(&v, 0, sizeof(v));
    v.version = AUDIO_VOLUME_VERSION;
    v.left    = (uint8_t)left;
    v.right   = (uint8_t)right;
    v.mute    = mute ? 1 : 0;
    if (__audio_mix_fd(fd))
        return __audio_mix_set_volume(fd, left, right, mute);
    return ioctl(fd, AUDIO_IOCTL_SET_VOLUME, &v);
}

int audio_get_volume(int fd, audio_volume_t* volume) {
    if (volume == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (__audio_mix_fd(fd))
        return __audio_mix_get_volume(fd, volume);
    return ioctl(fd, AUDIO_IOCTL_GET_VOLUME, volume);
}

int audio_report(int fd, char* text, size_t size) {
    if (text == NULL || size == 0) {
        errno = EINVAL;
        return -1;
    }
    audio_report_t* r = (audio_report_t*)malloc(sizeof(audio_report_t));
    if (r == NULL) {
        errno = ENOMEM;
        return -1;
    }
    // A mixer stream: the device's report, from its control handle
    int ctl = -1;
    if (__audio_mix_fd(fd)) {
        ctl = audio_ctl_open(0);
        if (ctl < 0) {
            free(r);
            return -1;
        }
    }
    int result = ioctl(ctl >= 0 ? ctl : fd, AUDIO_IOCTL_REPORT, r);
    if (ctl >= 0)
        close(ctl);
    if (result == 0) {
        r->text[AUDIO_REPORT_MAX - 1] = '\0';
        size_t n = strlen(r->text);
        if (n >= size)
            n = size - 1;
        memcpy(text, r->text, n);
        text[n] = '\0';
        result = (int)n;
    }
    free(r);
    return result;
}

int audio_frame_bytes(int format, int channels) {
    if (channels <= 0)
        return 0;
    switch (format) {
        case AUDIO_FORMAT_S16: return 2 * channels;
        case AUDIO_FORMAT_S32: return 4 * channels;
        default:               return 0;
    }
}

uint32_t audio_rate_bit(uint32_t rate) {
    static const uint32_t rates[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000,
                                      88200, 96000, 176400, 192000 };
    for (uint32_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        if (rates[i] == rate)
            return 1u << i;
    }
    return 0;
}

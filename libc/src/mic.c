/*
 * Sound input (os/audio.h, mic_*) on /dev/micN: open, read and ioctl
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <os/audio.h>
#include <os/os.h>
#include <os/security.h>

int mic_request(const char* reason) {
    if (sec_has(SEC_MICROPHONE))
        return 0;
    return sec_elevate_bits(reason != NULL ? reason : "Record sound from the microphone", SEC_MICROPHONE);
}

int mic_open(int index) {
    if (index < 0 || index > 99) {
        errno = EINVAL;
        return -1;
    }
    char path[24];
    snprintf(path, sizeof(path), "/dev/mic%d", index);
    struct stat st;
    if (stat(path, &st) < 0) {
        errno = ENOENT;
        return -1;
    }
    // The kernel's open says only that it failed: the bit is the one thing
    // we can see, and with it the driver's one reason is another handle
    if (!sec_has(SEC_MICROPHONE)) {
        errno = EACCES;
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        errno = EBUSY;
    return fd;
}

int mic_close(int fd) {
    return close(fd);
}

int mic_set_format(int fd, uint32_t rate, int channels, int format) {
    if (channels <= 0 || channels > 0xFFFF || format <= 0 || format > 0xFFFF) {
        errno = EINVAL;
        return -1;
    }
    audio_format_t f;
    memset(&f, 0, sizeof(f));
    f.version  = AUDIO_FORMAT_VERSION;
    f.rate     = rate;
    f.channels = (uint16_t)channels;
    f.format   = (uint16_t)format;
    return ioctl(fd, AUDIO_IOCTL_SET_FORMAT, &f);
}

int mic_start(int fd) { return ioctl(fd, AUDIO_IOCTL_START, 0); }
int mic_stop(int fd)  { return ioctl(fd, AUDIO_IOCTL_STOP, 0); }

long mic_read(int fd, void* data, size_t bytes) {
    if (bytes == 0)
        return 0;
    if (data == NULL) {
        errno = EINVAL;
        return -1;
    }
    return (long)read(fd, data, bytes);
}

long mic_read_all(int fd, void* data, size_t bytes) {
    if (data == NULL && bytes > 0) {
        errno = EINVAL;
        return -1;
    }
    audio_format_t f;
    if (ioctl(fd, AUDIO_IOCTL_GET_FORMAT, &f) < 0)
        return -1;
    size_t frame = (size_t)audio_frame_bytes(f.format, f.channels);
    if (frame == 0 || f.rate == 0) {
        errno = EINVAL;
        return -1;
    }
    bytes -= bytes % frame;
    uint8_t* p = (uint8_t*)data;
    size_t done = 0;
    while (done < bytes) {
        long n = mic_read(fd, p + done, bytes - done);
        if (n < 0)
            return (done > 0) ? (long)done : -1;
        done += (size_t)n;
        if (done >= bytes)
            break;
        // Wait about as long as the rest takes to come in, at most 20 ms
        uint64_t ms = (uint64_t)(bytes - done) * 1000u / ((uint64_t)f.rate * frame);
        if (ms < 2)  ms = 2;
        if (ms > 20) ms = 20;
        os_sleep_ms((uint32_t)ms);
    }
    return (long)done;
}

int mic_set_gain(int fd, int level, int mute) {
    if (level < 0 || level > 100) {
        errno = EINVAL;
        return -1;
    }
    audio_volume_t v;
    memset(&v, 0, sizeof(v));
    v.version = AUDIO_VOLUME_VERSION;
    v.left = v.right = (uint8_t)level;
    v.mute = mute ? 1 : 0;
    return ioctl(fd, AUDIO_IOCTL_SET_VOLUME, &v);
}

int mic_get_gain(int fd, int* level, int* mute) {
    audio_volume_t v;
    if (ioctl(fd, AUDIO_IOCTL_GET_VOLUME, &v) < 0)
        return -1;
    if (level != NULL) *level = v.left;
    if (mute != NULL)  *mute = v.mute;
    return 0;
}

int audio_capture_status(int fd, audio_capture_t* status) {
    if (status == NULL) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(fd, AUDIO_IOCTL_CAPTURE, status);
}

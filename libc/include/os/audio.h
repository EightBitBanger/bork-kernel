#ifndef _OS_AUDIO_H
#define _OS_AUDIO_H

/*
 * Sound output: raw PCM to /dev/audioN (the kernel's HDA driver, for the
 * Realtek ALC269 and other HDA codecs).
 *
 * One stream per device, played exactly as written. The kernel does no
 * mixing, resampling or format conversion: to play several sounds at once,
 * add them up in your own buffer and write that. One program at a time
 * holds a device (audio_open_device fails with EBUSY while another has it).
 *
 * The mixer: while the volume service (audiosrv) runs, it holds
 * /dev/audio0 and mixes in user space, and audio_open(0) gives a stream on
 * it instead, so several programs play at once, each with its own slider
 * and mute on the volume panel. The same calls work on it; what differs:
 * the format is S16 stereo at the mix rate only (audio_info().rates shows
 * it, normally 48 kHz), and audio_set_volume / audio_get_volume are the
 * program's own level, not the codec's. audio_set_name names the stream on
 * the panel (the program's file name until then). Without the service,
 * audio_open(0) is audio_open_device(0). To always have the hardware (a
 * mixer of your own, the lowest latency), call audio_open_device.
 *
 *     int fd = audio_open(0);                              // /dev/audio0
 *     audio_set_format(fd, 48000, 2, AUDIO_FORMAT_S16);    // 4-byte frames: L, R
 *     audio_write_all(fd, samples, bytes);                 // waits for room as needed
 *     audio_drain(fd);                                     // until it has all played
 *     audio_close(fd);
 *
 * Or, from a game loop that must never wait:
 *
 *     audio_status_t s;
 *     audio_status(fd, &s);
 *     if (s.queued_bytes < want_latency_bytes)             // keep the queue short
 *         audio_write(fd, next, bytes);                    // takes what fits, never blocks
 *
 * The stream starts once start_bytes are queued (audio_set_format: an
 * eighth of the buffer, about 85 ms at 48 kHz 16-bit; audio_set_format_ex
 * to choose) or at audio_start(). If the queue runs dry the device plays
 * silence and counts an underrun; a second later it stops, and the next
 * write starts it again. played_frames in the status counts every frame
 * the hardware has played since the format was set (silence included), so
 * it works as the stream's clock.
 *
 * Samples are signed, little-endian, interleaved left then right:
 *   AUDIO_FORMAT_S16  16 bits each
 *   AUDIO_FORMAT_S32  32-bit each, MSB-aligned: the codec plays the top
 *                     24 (or 20) bits
 * Rates: what the codec takes, audio_info().rates (AUDIO_RATE_* bits; the
 * ALC269 does 44.1, 48, 96 and 192 kHz). Channels: 2.
 *
 * Every call returns 0 (audio_write*: the bytes taken; audio_open: a
 * descriptor) or -1 with errno: ENOENT (no such device), EBUSY (another
 * program has it), EINVAL (a rate, format or channel count the device
 * can't do), EPERM (no file write permission), EBADF.
 */

#include <stddef.h>
#include <stdint.h>

/* The kernel's interface (kernel/arch/x86/drivers/audio/audio.h), kept in step */

#define AUDIO_IOCTL_INFO        0x41550001u
#define AUDIO_IOCTL_SET_FORMAT  0x41550002u
#define AUDIO_IOCTL_GET_FORMAT  0x41550003u
#define AUDIO_IOCTL_START       0x41550004u
#define AUDIO_IOCTL_PAUSE       0x41550005u
#define AUDIO_IOCTL_STOP        0x41550006u
#define AUDIO_IOCTL_STATUS      0x41550007u
#define AUDIO_IOCTL_SET_VOLUME  0x41550008u
#define AUDIO_IOCTL_GET_VOLUME  0x41550009u
#define AUDIO_IOCTL_REPORT      0x4155000Au

#define AUDIO_INFO_VERSION      1
#define AUDIO_FORMAT_VERSION    1
#define AUDIO_STATUS_VERSION    1
#define AUDIO_VOLUME_VERSION    1
#define AUDIO_REPORT_VERSION    1

#define AUDIO_FORMAT_S16        0x0001
#define AUDIO_FORMAT_S32        0x0002

#define AUDIO_RATE_8000         (1u << 0)
#define AUDIO_RATE_11025        (1u << 1)
#define AUDIO_RATE_16000        (1u << 2)
#define AUDIO_RATE_22050        (1u << 3)
#define AUDIO_RATE_32000        (1u << 4)
#define AUDIO_RATE_44100        (1u << 5)
#define AUDIO_RATE_48000        (1u << 6)
#define AUDIO_RATE_88200        (1u << 7)
#define AUDIO_RATE_96000        (1u << 8)
#define AUDIO_RATE_176400       (1u << 9)
#define AUDIO_RATE_192000       (1u << 10)

#define AUDIO_OUT_SPEAKER       0x01
#define AUDIO_OUT_HEADPHONE     0x02
#define AUDIO_OUT_LINE          0x04

#define AUDIO_STATE_STOPPED     0
#define AUDIO_STATE_RUNNING     1
#define AUDIO_STATE_PAUSED      2

#define AUDIO_JACK_UNKNOWN      0xFF
#define AUDIO_IDLE_STOP_MS      1000
#define AUDIO_REPORT_MAX        2048

typedef struct {
    uint32_t version;
    uint32_t codec_id;          /* 0x10EC0269 for the ALC269 */
    uint32_t codec_revision;
    uint16_t pci_vendor;        /* The HDA controller */
    uint16_t pci_device;
    uint32_t rates;             /* AUDIO_RATE_* */
    uint32_t formats;           /* AUDIO_FORMAT_* */
    uint8_t  channels_min;
    uint8_t  channels_max;
    uint8_t  outputs;           /* AUDIO_OUT_* */
    uint8_t  hw_volume;         /* 1: audio_set_volume has steps; 0: mute only */
    uint32_t buffer_bytes;      /* Most that can be queued */
    char     codec[32];         /* "Realtek ALC269" */
} audio_info_t;

typedef struct {
    uint32_t version;
    uint32_t rate;
    uint16_t channels;
    uint16_t format;            /* AUDIO_FORMAT_* */
    uint32_t start_bytes;       /* Queued bytes that start the stream (0: an eighth of the buffer) */
    uint32_t reserved;
} audio_format_t;

typedef struct {
    uint32_t version;
    uint8_t  state;             /* AUDIO_STATE_* */
    uint8_t  headphone;         /* 1 plugged in, 0 not, AUDIO_JACK_UNKNOWN */
    uint16_t frame_bytes;
    uint32_t queued_bytes;      /* Written, not played yet */
    uint32_t free_bytes;        /* What audio_write could take now */
    uint32_t buffer_bytes;
    uint32_t underruns;
    uint64_t played_frames;     /* Since the format was set, silence included */
    uint64_t written_frames;
} audio_status_t;

typedef struct {
    uint32_t version;
    uint8_t  left;              /* 0..100 */
    uint8_t  right;
    uint8_t  mute;
    uint8_t  reserved;
} audio_volume_t;

typedef struct {
    uint32_t version;
    uint32_t length;
    char     text[AUDIO_REPORT_MAX];
} audio_report_t;

_Static_assert(sizeof(audio_info_t) == 64, "audio_info_t is the kernel's AudioInfo");
_Static_assert(sizeof(audio_format_t) == 20, "audio_format_t is the kernel's AudioFormat");
_Static_assert(sizeof(audio_status_t) == 40, "audio_status_t is the kernel's AudioStatus");
_Static_assert(sizeof(audio_volume_t) == 8, "audio_volume_t is the kernel's AudioVolume");

/* A stream for writing: on the volume service's mixer when it runs (index
   0), else /dev/audio<index>. A descriptor, or -1. */
int  audio_open(int index);
/* /dev/audio<index> itself, never the mixer */
int  audio_open_device(int index);
/* The name shown for a mixer stream on the volume panel ("Media player").
   0 and nothing done on a device descriptor. */
int  audio_set_name(int fd, const char* name);
/* 1: the descriptor is a stream on the mixer; 0: a device */
int  audio_is_mixed(int fd);
/* /dev/audioctl<index>: the same device for a volume control or a status
   display. Read-only, never busy (any number of programs at once), so it
   works while another program plays. audio_info, audio_status,
   audio_get_format, audio_get/set_volume and audio_report work on it; the
   stream calls (set_format, start, pause, stop) fail with EPERM. */
int  audio_ctl_open(int index);
int  audio_close(int fd);

int  audio_info(int fd, audio_info_t* info);

/* Stops the stream and drops what is queued, then takes the new format */
int  audio_set_format(int fd, uint32_t rate, int channels, int format);
int  audio_set_format_ex(int fd, const audio_format_t* format);
int  audio_get_format(int fd, audio_format_t* format);

/* Queue what fits right now, whole frames only: the bytes taken (0: full) */
long audio_write(int fd, const void* data, size_t bytes);
/* Queue all of it, sleeping while the buffer is full: bytes, or -1. A part
   frame at the end is left out. Starts (or resumes) the stream when it is
   full and not playing, so it never waits forever. */
long audio_write_all(int fd, const void* data, size_t bytes);

int  audio_status(int fd, audio_status_t* status);
int  audio_start(int fd);       /* Play now, even below start_bytes; resumes after a pause */
int  audio_pause(int fd);       /* Hold the position; the queue is kept */
int  audio_stop(int fd);        /* Stop and drop everything queued */
/* Start if needed and wait until everything queued has played */
int  audio_drain(int fd);

/* The codec's output amplifier, 0..100 a side. Not the samples. On a mixer
   stream: that program's own level (the larger side), which does scale
   its samples. */
int  audio_set_volume(int fd, int left, int right, int mute);
int  audio_get_volume(int fd, audio_volume_t* volume);

/* What the driver found (controller, codec, pins, routes) as text lines,
   for bug reports. NUL-terminated; returns its length. */
int  audio_report(int fd, char* text, size_t size);

/* Helpers */
int      audio_frame_bytes(int format, int channels);   /* 4 for S16 stereo, 0 if unknown */
uint32_t audio_rate_bit(uint32_t rate);                 /* AUDIO_RATE_48000 for 48000, 0 if none */

#endif

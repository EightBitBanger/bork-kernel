#ifndef _KERNEL_DRIVERS_AUDIO_H_
#define _KERNEL_DRIVERS_AUDIO_H_

#include <stdint.h>

//
// Sound output: /dev/audio0, /dev/audio1, ...
//
// One raw PCM stream per device, played as written: no mixing, no
// resampling, no format conversion in the kernel. A program that wants to
// play several sounds at once mixes them itself and writes the result.
// The libc wraps all of this in <os/audio.h> (audio_open, audio_write ...).
//
// Only one handle at a time (open is SYS_EBUSY while another is open).
// Opening for writing needs the file write permission like any file.
//
// /dev/audioctlN is the same device for anyone else, read-only and any
// number of handles at once: INFO, GET_FORMAT, STATUS, GET_VOLUME,
// SET_VOLUME and REPORT work on it (the volume tray icon uses it while a
// program plays); SET_FORMAT, START, PAUSE and STOP are SYS_EPERM.
//
//   struct AudioInfo info;
//   ioctl(fd, AUDIO_IOCTL_INFO, &info);         // codec, rates, buffer size
//
//   struct AudioFormat f = { AUDIO_FORMAT_VERSION, 48000, 2, AUDIO_FORMAT_S16 };
//   ioctl(fd, AUDIO_IOCTL_SET_FORMAT, &f);      // stops and empties the stream
//
//   write(fd, frames, bytes);                    // queues what fits, returns that
//                                                // (0: the buffer is full, try later)
//   struct AudioStatus s;
//   ioctl(fd, AUDIO_IOCTL_STATUS, &s);          // queued / free bytes, frames played
//
// write never waits: it copies what fits into the device's ring buffer and
// returns how many bytes that was (whole frames only). The stream starts by
// itself once AudioFormat.start_bytes are queued, or at AUDIO_IOCTL_START.
// When the program stops writing, the queue runs dry and the device plays
// silence (counted in AudioStatus.underruns); after AUDIO_IDLE_STOP_MS of
// that it stops, and the next write starts it again like the first.
//
// Samples are little-endian, signed and interleaved (left, right):
//   AUDIO_FORMAT_S16   16 bits per sample, 4 bytes a stereo frame
//   AUDIO_FORMAT_S32   32-bit containers, 8 bytes a frame; the codec uses the
//                      top 24 (or 20) bits, so the low byte may be anything
//
// Rates are those the codec takes (AudioInfo.rates, AUDIO_RATE_* bits);
// anything else is SYS_EINVAL, resample in the program. Channels: 2.
//
// Volume (AUDIO_IOCTL_SET_VOLUME) is the codec's own output amplifier, in
// 0..100 per side, and a mute. It does not touch the samples.
//
// Sound input: /dev/micN
//
// The same controller's microphone (or line in), recorded from one of its
// input streams into a ring of its own. Opening it needs SYS_SEC_MICROPHONE
// (CharDevice.security, checked again on every read and ioctl, so taking
// the bit away stops a handle already open). One handle at a time
// (SYS_EBUSY while another program has it). Read-only.
//
//   ioctl(fd, AUDIO_IOCTL_SET_FORMAT, &f);      // rate and S16 / S32 stereo
//   ioctl(fd, AUDIO_IOCTL_START, 0);            // or just read: that starts it
//   read(fd, frames, bytes);                    // what has come in, whole frames
//                                               // (0: nothing yet, try later)
//
// read never waits. What the program doesn't read within the ring
// (AudioInfo.buffer_bytes, about 680 ms at 48 kHz 16-bit) is overwritten:
// the oldest goes first and AudioStatus.underruns counts the times. STOP
// (or close) ends the recording and powers the input path down; PAUSE is
// the same as STOP here.
//
// On /dev/micN the shared structs mean:
//   AudioInfo.rates / formats   what the ADC takes
//   AudioInfo.outputs           AUDIO_IN_* the codec has wired up
//   AudioInfo.hw_volume         1: SET_VOLUME has steps (the capture gain)
//   AudioStatus.queued_bytes    recorded and not read yet
//   AudioStatus.free_bytes      room before the oldest is overwritten
//   AudioStatus.headphone       1: a microphone is plugged into the jack
//   AudioStatus.underruns       overruns: times recorded sound was lost
//   AudioStatus.played_frames   recorded since the format was set
//   AudioStatus.written_frames  read by the program
//   AudioVolume                 capture gain 0..100 (AudioInfo.hw_volume),
//                               and a mute that records silence
//
// AUDIO_IOCTL_CAPTURE, on /dev/audioctlN or /dev/micN, says whether the
// microphone is recording and for which program, so the tray can show it.
// The program name is the kernel's own (the executable's file name).
//

#define AUDIO_IOCTL_INFO        0x41550001u     // 'AU' 1: struct AudioInfo* to fill
#define AUDIO_IOCTL_SET_FORMAT  0x41550002u     // const struct AudioFormat*
#define AUDIO_IOCTL_GET_FORMAT  0x41550003u     // struct AudioFormat* to fill
#define AUDIO_IOCTL_START       0x41550004u     // start (or resume) now; arg unused
#define AUDIO_IOCTL_PAUSE       0x41550005u     // hold the stream where it is; arg unused
#define AUDIO_IOCTL_STOP        0x41550006u     // stop and drop everything queued; arg unused
#define AUDIO_IOCTL_STATUS      0x41550007u     // struct AudioStatus* to fill
#define AUDIO_IOCTL_SET_VOLUME  0x41550008u     // const struct AudioVolume*
#define AUDIO_IOCTL_GET_VOLUME  0x41550009u     // struct AudioVolume* to fill
#define AUDIO_IOCTL_REPORT      0x4155000Au     // struct AudioReport* to fill: what the driver found
#define AUDIO_IOCTL_CAPTURE     0x4155000Bu     // struct AudioCapture* to fill: who records

#define AUDIO_INFO_VERSION      1
#define AUDIO_FORMAT_VERSION    1
#define AUDIO_STATUS_VERSION    1
#define AUDIO_VOLUME_VERSION    1
#define AUDIO_REPORT_VERSION    1
#define AUDIO_CAPTURE_VERSION   1

// AudioFormat.format, and bits of AudioInfo.formats
#define AUDIO_FORMAT_S16        0x0001
#define AUDIO_FORMAT_S32        0x0002

// AudioInfo.rates: the HDA order
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

// AudioInfo.outputs
#define AUDIO_OUT_SPEAKER       0x01
#define AUDIO_OUT_HEADPHONE     0x02
#define AUDIO_OUT_LINE          0x04

// AudioInfo.outputs on /dev/micN, AudioCapture.source
#define AUDIO_IN_MIC            0x01    // Microphone jack
#define AUDIO_IN_INTERNAL_MIC   0x02    // Built-in microphone
#define AUDIO_IN_LINE           0x04    // Line in

// AudioStatus.state
#define AUDIO_STATE_STOPPED     0       // Nothing playing; a write queues and may start it
#define AUDIO_STATE_RUNNING     1
#define AUDIO_STATE_PAUSED      2       // AUDIO_IOCTL_PAUSE: queue kept, START resumes

// AudioStatus.headphone
#define AUDIO_JACK_UNKNOWN      0xFF

#define AUDIO_IDLE_STOP_MS      1000

struct AudioInfo {
    uint32_t version;           // AUDIO_INFO_VERSION
    uint32_t codec_id;          // Vendor (31:16) and device (15:0): 0x10EC0269
    uint32_t codec_revision;
    uint16_t pci_vendor;        // The HDA controller
    uint16_t pci_device;
    uint32_t rates;             // AUDIO_RATE_*
    uint32_t formats;           // AUDIO_FORMAT_*
    uint8_t  channels_min;
    uint8_t  channels_max;
    uint8_t  outputs;           // AUDIO_OUT_*: the outputs the codec has wired up
    uint8_t  hw_volume;         // 1: AUDIO_IOCTL_SET_VOLUME has steps; 0: mute only
    uint32_t buffer_bytes;      // The ring buffer: most that can be queued
    char     codec[32];         // "Realtek ALC269", NUL-terminated
};

struct AudioFormat {
    uint32_t version;           // AUDIO_FORMAT_VERSION
    uint32_t rate;              // 44100, 48000 ...
    uint16_t channels;          // 2
    uint16_t format;            // AUDIO_FORMAT_S16 / AUDIO_FORMAT_S32
    uint32_t start_bytes;       // Queued bytes that start the stream (0: an eighth of the buffer)
    uint32_t reserved;
};

struct AudioStatus {
    uint32_t version;           // AUDIO_STATUS_VERSION
    uint8_t  state;             // AUDIO_STATE_*
    uint8_t  headphone;         // 1 plugged in, 0 not, AUDIO_JACK_UNKNOWN
    uint16_t frame_bytes;       // Bytes per frame in the current format
    uint32_t queued_bytes;      // Written and not played yet
    uint32_t free_bytes;        // What a write could take right now
    uint32_t buffer_bytes;
    uint32_t underruns;         // Times the queue ran dry while running
    uint64_t played_frames;     // Since the format was set
    uint64_t written_frames;
};

struct AudioVolume {
    uint32_t version;           // AUDIO_VOLUME_VERSION
    uint8_t  left;              // 0..100
    uint8_t  right;
    uint8_t  mute;              // 1: muted
    uint8_t  reserved;
};

#define AUDIO_REPORT_MAX        2048

struct AudioReport {
    uint32_t version;           // AUDIO_REPORT_VERSION
    uint32_t length;            // strlen(text)
    char     text[AUDIO_REPORT_MAX];   // Lines: controller, codec, pins, paths
};

// AUDIO_IOCTL_CAPTURE
#define AUDIO_CAPTURE_IDLE      0       // Nobody has /dev/micN open
#define AUDIO_CAPTURE_OPEN      1       // Open, not recording
#define AUDIO_CAPTURE_RUNNING   2       // Recording now

struct AudioCapture {
    uint32_t version;           // AUDIO_CAPTURE_VERSION
    uint8_t  state;             // AUDIO_CAPTURE_*
    uint8_t  source;            // AUDIO_IN_* being recorded (0: none)
    uint8_t  present;           // 1: /dev/micN exists (the codec has an input)
    uint8_t  reserved;
    uint32_t pid;               // The program holding it (0: none)
    uint32_t starts;            // Recordings started since boot: a change means one ran
    uint32_t rate;
    char     program[16];       // Its executable's name, NUL-terminated
};

_Static_assert(sizeof(struct AudioCapture) == 36, "AudioCapture is an ABI");
_Static_assert(sizeof(struct AudioInfo) == 64, "AudioInfo is an ABI");
_Static_assert(sizeof(struct AudioFormat) == 20, "AudioFormat is an ABI");
_Static_assert(sizeof(struct AudioStatus) == 40, "AudioStatus is an ABI");
_Static_assert(sizeof(struct AudioVolume) == 8, "AudioVolume is an ABI");

#endif

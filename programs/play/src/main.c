/*
 * play: sound from the command line, through os/audio.h
 *
 *   play file.wav            play a PCM WAV file (8, 16, 24 or 32-bit, mono or stereo)
 *   play -t [hz] [seconds]   a sine tone (default 440 Hz, 2 s), left then right then both
 *   play -i                  what the driver found: codec, pins, routes (send this
 *                            along when something doesn't play)
 *   play -v <0..100>         set the volume (-m mutes, -u unmutes)
 *   play -d <n> ...          use /dev/audio<n> instead of audio0
 *
 * The kernel plays raw PCM only, so the conversions a WAV needs (mono to
 * stereo, 8 and 24-bit to the device's formats) happen here. It does not
 * resample: a WAV at a rate the codec can't take is refused with the rates
 * it can.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <os/audio.h>
#include <os/os.h>

#define CHUNK_FRAMES    2048

static int device_index = 0;

static int open_device(void) {
    int fd = audio_open(device_index);
    if (fd < 0) {
        if (errno == ENOENT)
            printf("play: no /dev/audio%d (no sound card the kernel could use)\n", device_index);
        else if (errno == EBUSY)
            printf("play: /dev/audio%d is in use by another program\n", device_index);
        else
            printf("play: /dev/audio%d: %s\n", device_index, strerror(errno));
    }
    return fd;
}

static void print_rates(uint32_t rates) {
    static const uint32_t r[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000,
                                  88200, 96000, 176400, 192000 };
    for (int i = 0; i < 11; i++) {
        if (rates & (1u << i))
            printf(" %u", (unsigned)r[i]);
    }
}

static int cmd_info(void) {
    int fd = open_device();
    if (fd < 0)
        return 1;
    audio_info_t info;
    if (audio_info(fd, &info) == 0) {
        printf("%s (codec %08x rev %08x) on controller %04x:%04x\n", info.codec, (unsigned)info.codec_id,
               (unsigned)info.codec_revision, info.pci_vendor, info.pci_device);
        printf("outputs:%s%s%s\n", (info.outputs & AUDIO_OUT_SPEAKER) ? " speaker" : "",
               (info.outputs & AUDIO_OUT_HEADPHONE) ? " headphone" : "",
               (info.outputs & AUDIO_OUT_LINE) ? " line-out" : "");
        printf("rates:");
        print_rates(info.rates);
        printf("\nformats:%s%s, %u-%u channels, buffer %u bytes, volume %s\n",
               (info.formats & AUDIO_FORMAT_S16) ? " s16" : "", (info.formats & AUDIO_FORMAT_S32) ? " s32" : "",
               info.channels_min, info.channels_max, (unsigned)info.buffer_bytes,
               info.hw_volume ? "in steps" : "mute only");
    }
    audio_status_t s;
    audio_volume_t v;
    if (audio_status(fd, &s) == 0 && audio_get_volume(fd, &v) == 0) {
        printf("headphones: %s, volume %u/%u%s, underruns %u\n",
               s.headphone == 1 ? "in" : s.headphone == 0 ? "out" : "can't tell",
               v.left, v.right, v.mute ? " (muted)" : "", (unsigned)s.underruns);
    }
    char* text = (char*)malloc(AUDIO_REPORT_MAX);
    if (text != NULL && audio_report(fd, text, AUDIO_REPORT_MAX) >= 0)
        printf("\n%s", text);
    free(text);
    audio_close(fd);
    return 0;
}

static int cmd_volume(int value, int mute) {
    int fd = open_device();
    if (fd < 0)
        return 1;
    audio_volume_t v;
    if (audio_get_volume(fd, &v) < 0) {
        printf("play: %s\n", strerror(errno));
        audio_close(fd);
        return 1;
    }
    int left = v.left, right = v.right;
    if (value >= 0)
        left = right = value;
    if (mute < 0)
        mute = v.mute;
    int r = audio_set_volume(fd, left, right, mute);
    if (r < 0)
        printf("play: %s\n", strerror(errno));
    else
        printf("volume %d%s\n", left, mute ? " (muted)" : "");
    audio_close(fd);
    return r < 0 ? 1 : 0;
}

// Gaps while playing (asked before the drain: the queue running out at
// the end counts as one too)
static void report_underruns(int fd) {
    audio_status_t s;
    if (audio_status(fd, &s) == 0 && s.underruns > 0)
        printf("(%u underruns: the sound stopped and started again)\n", (unsigned)s.underruns);
}

// The nearest of 48 and 44.1 kHz the device takes
static uint32_t pick_rate(int fd) {
    audio_info_t info;
    if (audio_info(fd, &info) < 0)
        return 48000;
    if (info.rates & AUDIO_RATE_48000)
        return 48000;
    if (info.rates & AUDIO_RATE_44100)
        return 44100;
    for (int i = 0; i < 11; i++) {
        static const uint32_t r[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000,
                                      88200, 96000, 176400, 192000 };
        if (info.rates & (1u << i))
            return r[i];
    }
    return 48000;
}

// sin(x) for x in [0, 2 pi): the libc has cos
static double sine(double x) {
    return cos(x - 1.5707963267948966);
}

static int cmd_tone(double hz, double seconds) {
    if (hz < 20 || hz > 20000 || seconds <= 0 || seconds > 60) {
        printf("play: tone wants 20..20000 Hz and up to 60 seconds\n");
        return 1;
    }
    int fd = open_device();
    if (fd < 0)
        return 1;
    uint32_t rate = pick_rate(fd);
    if (audio_set_format(fd, rate, 2, AUDIO_FORMAT_S16) < 0) {
        printf("play: %u Hz 16-bit stereo: %s\n", (unsigned)rate, strerror(errno));
        audio_close(fd);
        return 1;
    }
    printf("%.0f Hz for %.1f s at %u Hz: left, right, then both\n", hz, seconds, (unsigned)rate);

    int16_t* buf = (int16_t*)malloc(CHUNK_FRAMES * 4);
    if (buf == NULL) {
        audio_close(fd);
        return 1;
    }
    uint32_t total = (uint32_t)(seconds * rate);
    double phase = 0, step = 2 * 3.14159265358979 * hz / rate;
    uint32_t done = 0;
    while (done < total) {
        uint32_t n = total - done;
        if (n > CHUNK_FRAMES)
            n = CHUNK_FRAMES;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t at = done + i;
            // Fade 5 ms in and out so it doesn't click
            double env = 1.0;
            uint32_t fade = rate / 200;
            if (at < fade) env = (double)at / fade;
            if (total - at < fade) env = (double)(total - at) / fade;
            int16_t v = (int16_t)(sine(phase) * 12000.0 * env);
            phase += step;
            if (phase >= 2 * 3.14159265358979)
                phase -= 2 * 3.14159265358979;
            uint32_t third = total / 3;
            buf[i * 2]     = (at < third || at >= 2 * third) ? v : 0;
            buf[i * 2 + 1] = (at >= third) ? v : 0;
        }
        if (audio_write_all(fd, buf, n * 4) < 0) {
            printf("play: %s\n", strerror(errno));
            break;
        }
        done += n;
    }
    report_underruns(fd);
    audio_drain(fd);
    free(buf);
    audio_close(fd);
    return 0;
}

//
// WAV
//

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int cmd_wav(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        printf("play: %s: %s\n", path, strerror(errno));
        return 1;
    }
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) {
        printf("play: %s: not a WAV file\n", path);
        fclose(f);
        return 1;
    }
    uint16_t tag = 0, channels = 0, bits = 0, block = 0;
    uint32_t rate = 0, data_len = 0;
    bool have_fmt = false, have_data = false;
    while (!have_data) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8)
            break;
        uint32_t len = le32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0 && len >= 16 && len <= 64) {
            uint8_t fmt[64];
            if (fread(fmt, 1, len, f) != len)
                break;
            tag      = le16(fmt);
            channels = le16(fmt + 2);
            rate     = le32(fmt + 4);
            block    = le16(fmt + 12);
            bits     = le16(fmt + 14);
            if (tag == 0xFFFE && len >= 26)
                tag = le16(fmt + 24);           // WAVE_FORMAT_EXTENSIBLE: the subformat
            have_fmt = true;
            if (len & 1) fgetc(f);
        } else if (memcmp(ch, "data", 4) == 0) {
            data_len = len;
            have_data = true;
        } else {
            if (fseek(f, (long)(len + (len & 1)), SEEK_CUR) != 0)
                break;
        }
    }
    if (!have_fmt || !have_data || tag != 1 || (channels != 1 && channels != 2) ||
        (bits != 8 && bits != 16 && bits != 24 && bits != 32) || block != channels * (bits / 8)) {
        printf("play: %s: only PCM WAV, mono or stereo, 8/16/24/32-bit (this is format %u, %u ch, %u bits)\n",
               path, tag, channels, bits);
        fclose(f);
        return 1;
    }

    int fd = open_device();
    if (fd < 0) {
        fclose(f);
        return 1;
    }
    audio_info_t info;
    audio_info(fd, &info);
    int out_format = (bits <= 16) ? AUDIO_FORMAT_S16 : AUDIO_FORMAT_S32;
    if (!(info.formats & out_format))
        out_format = (info.formats & AUDIO_FORMAT_S16) ? AUDIO_FORMAT_S16 : AUDIO_FORMAT_S32;
    if (audio_set_format(fd, rate, 2, out_format) < 0) {
        printf("play: the device can't play %u Hz (it takes", (unsigned)rate);
        print_rates(info.rates);
        printf(")\n");
        audio_close(fd);
        fclose(f);
        return 1;
    }
    printf("%s: %u Hz, %u-bit, %s, %u.%u s\n", path, (unsigned)rate, bits, channels == 1 ? "mono" : "stereo",
           (unsigned)(data_len / block / rate), (unsigned)(data_len / block % rate * 10 / rate));

    uint8_t* in  = (uint8_t*)malloc(CHUNK_FRAMES * block);
    uint8_t* out = (uint8_t*)malloc(CHUNK_FRAMES * 8);
    if (in == NULL || out == NULL) {
        free(in);
        free(out);
        audio_close(fd);
        fclose(f);
        return 1;
    }
    uint32_t left = data_len;
    while (left >= block) {
        uint32_t want = left / block;
        if (want > CHUNK_FRAMES)
            want = CHUNK_FRAMES;
        size_t got = fread(in, block, want, f);
        if (got == 0)
            break;
        left -= (uint32_t)got * block;
        for (size_t i = 0; i < got; i++) {
            int32_t s[2];
            for (int c = 0; c < channels; c++) {
                const uint8_t* p = in + i * block + c * (bits / 8);
                switch (bits) {
                    case 8:  s[c] = ((int32_t)p[0] - 128) << 24; break;
                    case 16: s[c] = (int32_t)((uint32_t)le16(p) << 16); break;
                    case 24: s[c] = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)); break;
                    default: s[c] = (int32_t)le32(p); break;
                }
            }
            if (channels == 1)
                s[1] = s[0];
            if (out_format == AUDIO_FORMAT_S16) {
                int16_t* o = (int16_t*)out;
                o[i * 2]     = (int16_t)(s[0] >> 16);
                o[i * 2 + 1] = (int16_t)(s[1] >> 16);
            } else {
                int32_t* o = (int32_t*)out;
                o[i * 2]     = s[0];
                o[i * 2 + 1] = s[1];
            }
        }
        size_t bytes = got * (out_format == AUDIO_FORMAT_S16 ? 4 : 8);
        if (audio_write_all(fd, out, bytes) < 0) {
            printf("play: %s\n", strerror(errno));
            break;
        }
    }
    report_underruns(fd);
    audio_drain(fd);
    free(in);
    free(out);
    audio_close(fd);
    fclose(f);
    return 0;
}

static void usage(void) {
    printf("usage: play file.wav\n"
           "       play -t [hz] [seconds]   test tone (440 Hz, 2 s)\n"
           "       play -i                  what the sound driver found\n"
           "       play -v <0-100> | -m | -u   volume, mute, unmute\n"
           "       play -d <n> ...          /dev/audio<n> instead of audio0\n");
}

int main(int argc, char** argv) {
    int i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-d") == 0) {
        device_index = atoi(argv[i + 1]);
        i += 2;
    }
    if (i >= argc) {
        usage();
        return 1;
    }
    const char* a = argv[i];
    if (strcmp(a, "-i") == 0)
        return cmd_info();
    if (strcmp(a, "-t") == 0) {
        double hz  = (i + 1 < argc) ? atof(argv[i + 1]) : 440;
        double sec = (i + 2 < argc) ? atof(argv[i + 2]) : 2;
        return cmd_tone(hz, sec);
    }
    if (strcmp(a, "-v") == 0 && i + 1 < argc)
        return cmd_volume(atoi(argv[i + 1]), -1);
    if (strcmp(a, "-m") == 0)
        return cmd_volume(-1, 1);
    if (strcmp(a, "-u") == 0)
        return cmd_volume(-1, 0);
    if (a[0] == '-') {
        usage();
        return 1;
    }
    return cmd_wav(a);
}

/*
 * rec: record sound from the microphone, through os/audio.h (mic_*)
 *
 *   rec file.wav             record 5 seconds into a 16-bit WAV file
 *   rec -t <seconds> ...     how long (1..600)
 *   rec -r <rate> ...        sample rate (default 48000; the ADC's rates only)
 *   rec -1 ...               mono: left and right averaged (half the size)
 *   rec -g <0..100> ...      capture gain (default: 0 dB)
 *   rec -l [seconds]         no file: show the level once a second
 *   rec -i                   what the microphone is: inputs, rates, routes
 *   rec -d <n> ...           use /dev/mic<n> instead of mic0
 *
 * Recording needs the microphone permission. Without it, rec asks for it
 * (the desktop's prompt); a program set to Deny in Properties gets "not
 * allowed" without one. Play the result back with "play file.wav" or the
 * media player.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/audio.h>
#include <os/os.h>
#include <os/security.h>

#define CHUNK_BYTES     8192
#define MAX_SECONDS     600

static int device_index = 0;

static void print_rates(uint32_t rates) {
    static const uint32_t r[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000,
                                  88200, 96000, 176400, 192000 };
    for (int i = 0; i < 11; i++) {
        if (rates & (1u << i))
            printf(" %u", (unsigned)r[i]);
    }
}

static const char* source_name(int source) {
    switch (source) {
        case AUDIO_IN_MIC:          return "the microphone jack";
        case AUDIO_IN_INTERNAL_MIC: return "the built-in microphone";
        case AUDIO_IN_LINE:         return "line in";
        default:                    return "the input";
    }
}

static int open_mic(const char* why) {
    if (mic_request(why) < 0) {
        if (errno == EPERM)
            printf("rec: not allowed to use the microphone (Properties > Permissions says Deny, or no way to ask)\n");
        else if (errno == EACCES)
            printf("rec: the microphone permission was refused\n");
        else if (errno == EBUSY)
            printf("rec: refused a moment ago (or too many prompts waiting); try again in a few seconds\n");
        else
            printf("rec: can't ask for the microphone permission: %s\n", strerror(errno));
        return -1;
    }
    int fd = mic_open(device_index);
    if (fd < 0) {
        if (errno == ENOENT)
            printf("rec: no /dev/mic%d (no sound input the kernel could use)\n", device_index);
        else if (errno == EBUSY)
            printf("rec: /dev/mic%d is in use by another program\n", device_index);
        else
            printf("rec: /dev/mic%d: %s\n", device_index, strerror(errno));
    }
    return fd;
}

static int cmd_info(void) {
    int fd = open_mic("Show what the microphone is");
    if (fd < 0)
        return 1;
    audio_info_t info;
    if (audio_info(fd, &info) == 0) {
        printf("%s (codec %08x) on controller %04x:%04x\n", info.codec, (unsigned)info.codec_id,
               info.pci_vendor, info.pci_device);
        printf("inputs:%s%s%s\n", (info.outputs & AUDIO_IN_INTERNAL_MIC) ? " built-in mic" : "",
               (info.outputs & AUDIO_IN_MIC) ? " mic jack" : "", (info.outputs & AUDIO_IN_LINE) ? " line-in" : "");
        printf("rates:");
        print_rates(info.rates);
        printf("\nformats:%s%s, buffer %u bytes, gain %s\n",
               (info.formats & AUDIO_FORMAT_S16) ? " s16" : "", (info.formats & AUDIO_FORMAT_S32) ? " s32" : "",
               (unsigned)info.buffer_bytes, info.hw_volume ? "in steps" : "none");
    }
    audio_status_t s;
    int gain = 0, mute = 0;
    if (audio_status(fd, &s) == 0 && mic_get_gain(fd, &gain, &mute) == 0)
        printf("mic jack: %s, gain %d%s\n", s.headphone == 1 ? "plugged in" : s.headphone == 0 ? "empty" : "can't tell",
               gain, mute ? " (muted)" : "");
    char* text = (char*)malloc(AUDIO_REPORT_MAX);
    if (text != NULL && audio_report(fd, text, AUDIO_REPORT_MAX) >= 0)
        printf("%s", text);
    free(text);
    mic_close(fd);
    return 0;
}

static void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

static void wav_header(uint8_t* h, uint32_t rate, int channels, uint32_t data_bytes) {
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);                               // PCM
    put16(h + 22, (uint32_t)channels);
    put32(h + 24, rate);
    put32(h + 28, rate * (uint32_t)channels * 2);
    put16(h + 32, (uint32_t)channels * 2);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data_bytes);
}

static double peak_db(int peak) {
    if (peak <= 0)
        return -99.0;
    double db = 20.0 * log10((double)peak / 32768.0);
    return db < -99.0 ? -99.0 : db;
}

static int record(const char* path, uint32_t rate, int seconds, bool mono, int gain) {
    char why[128];
    if (path != NULL)
        snprintf(why, sizeof(why), "Record %d seconds of sound into %s", seconds, path);
    else
        snprintf(why, sizeof(why), "Show the microphone's level");
    int fd = open_mic(why);
    if (fd < 0)
        return 1;
    if (mic_set_format(fd, rate, 2, AUDIO_FORMAT_S16) < 0) {
        audio_info_t info;
        printf("rec: the microphone can't record at %u Hz; it takes:", (unsigned)rate);
        if (audio_info(fd, &info) == 0)
            print_rates(info.rates);
        printf("\n");
        mic_close(fd);
        return 1;
    }
    if (gain >= 0 && mic_set_gain(fd, gain, 0) < 0)
        printf("rec: can't set the gain: %s\n", strerror(errno));

    FILE* out = NULL;
    uint8_t header[44];
    int channels = mono ? 1 : 2;
    if (path != NULL) {
        out = fopen(path, "wb");
        if (out == NULL) {
            printf("rec: %s: %s\n", path, strerror(errno));
            mic_close(fd);
            return 1;
        }
        wav_header(header, rate, channels, 0);
        fwrite(header, 1, sizeof(header), out);
    }

    int16_t* buf = (int16_t*)malloc(CHUNK_BYTES);
    if (buf == NULL) {
        printf("rec: out of memory\n");
        if (out != NULL)
            fclose(out);
        mic_close(fd);
        return 1;
    }
    if (mic_start(fd) < 0) {
        printf("rec: the microphone would not start: %s\n", strerror(errno));
        free(buf);
        if (out != NULL)
            fclose(out);
        mic_close(fd);
        return 1;
    }
    audio_capture_t cap;
    int source = (audio_capture_status(fd, &cap) == 0) ? cap.source : 0;
    if (path != NULL)
        printf("Recording %d s from %s at %u Hz%s into %s\n", seconds, source_name(source), (unsigned)rate,
               mono ? " mono" : "", path);
    else
        printf("Level of %s for %d s:\n", source_name(source), seconds);

    uint64_t total_frames = (uint64_t)rate * (uint64_t)seconds;
    uint64_t frames = 0;
    uint64_t next_report = rate;
    int peak = 0, peak_all = 0;
    bool failed = false;
    while (frames < total_frames && !failed) {
        size_t want = CHUNK_BYTES;
        if ((total_frames - frames) * 4 < want)
            want = (size_t)(total_frames - frames) * 4;
        long n = mic_read_all(fd, buf, want);
        if (n <= 0) {
            printf("\nrec: reading the microphone failed: %s\n", n < 0 ? strerror(errno) : "nothing came");
            failed = true;
            break;
        }
        size_t count = (size_t)n / 4;
        for (size_t i = 0; i < count; i++) {
            int l = buf[2 * i], r = buf[2 * i + 1];
            int a = l < 0 ? -l : l, b = r < 0 ? -r : r;
            if (a > peak) peak = a;
            if (b > peak) peak = b;
            if (mono)
                buf[i] = (int16_t)((l + r) / 2);
        }
        if (out != NULL) {
            size_t bytes = count * (size_t)channels * 2;
            if (fwrite(buf, 1, bytes, out) != bytes) {
                printf("\nrec: writing %s failed: %s\n", path, strerror(errno));
                failed = true;
            }
        }
        frames += count;
        if (frames >= next_report || frames >= total_frames) {
            int bars = (int)((peak_db(peak) + 60.0) / 2.0);
            if (bars < 0)  bars = 0;
            if (bars > 30) bars = 30;
            char meter[32];
            memset(meter, '#', (size_t)bars);
            meter[bars] = '\0';
            printf("  %3u s  peak %4.0f dB  %s\n", (unsigned)((frames + rate / 2) / rate), peak_db(peak), meter);
            if (peak > peak_all)
                peak_all = peak;
            peak = 0;
            next_report += rate;
        }
    }

    audio_status_t st;
    unsigned overruns = (audio_status(fd, &st) == 0) ? (unsigned)st.underruns : 0;
    mic_close(fd);
    free(buf);

    if (out != NULL) {
        uint32_t data = (uint32_t)(frames * (uint64_t)channels * 2);
        wav_header(header, rate, channels, data);
        if (fseek(out, 0, SEEK_SET) != 0 || fwrite(header, 1, sizeof(header), out) != sizeof(header))
            printf("rec: could not finish the WAV header in %s\n", path);
        fclose(out);
        printf("%s: %u.%u s, %u bytes, loudest %.0f dB", path, (unsigned)(frames / rate),
               (unsigned)(frames % rate * 10 / rate), (unsigned)(data + 44), peak_db(peak_all));
    } else {
        printf("loudest %.0f dB", peak_db(peak_all));
    }
    if (overruns > 0)
        printf(", %u gap%s (sound lost while busy)", overruns, overruns == 1 ? "" : "s");
    printf("\n");
    return failed ? 1 : 0;
}

static void usage(void) {
    printf("usage: rec [-t seconds] [-r rate] [-1] [-g 0..100] [-d n] file.wav\n"
           "       rec -l [seconds]     show the microphone's level\n"
           "       rec -i               what the microphone is\n");
}

int main(int argc, char** argv) {
    uint32_t rate = 48000;
    int seconds = 5;
    bool mono = false, level = false;
    int gain = -1;
    const char* path = NULL;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        bool more = i + 1 < argc;
        if (strcmp(a, "-i") == 0) {
            return cmd_info();
        } else if (strcmp(a, "-t") == 0 && more) {
            seconds = atoi(argv[++i]);
        } else if (strcmp(a, "-r") == 0 && more) {
            rate = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(a, "-g") == 0 && more) {
            gain = atoi(argv[++i]);
            if (gain < 0 || gain > 100) {
                usage();
                return 1;
            }
        } else if (strcmp(a, "-d") == 0 && more) {
            device_index = atoi(argv[++i]);
        } else if (strcmp(a, "-1") == 0) {
            mono = true;
        } else if (strcmp(a, "-l") == 0) {
            level = true;
            if (more && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                seconds = atoi(argv[++i]);
        } else if (a[0] == '-' || path != NULL) {
            usage();
            return 1;
        } else {
            path = a;
        }
    }
    if (seconds < 1 || seconds > MAX_SECONDS) {
        printf("rec: -t takes 1 to %d seconds\n", MAX_SECONDS);
        return 1;
    }
    if (!level && path == NULL) {
        usage();
        return 1;
    }
    return record(level ? NULL : path, rate, seconds, mono, gain);
}

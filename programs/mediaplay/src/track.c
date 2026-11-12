/*
 * mediaplay: reading and decoding one file (track.h).
 *
 * MP3: an ID3v2 tag at the start is skipped (its title, artist and album
 * kept), an ID3v1 tag at the end too. The first frame says the rate and
 * channels; a Xing / Info or VBRI frame there gives the length of a VBR
 * file (and Xing a seek table), otherwise the length comes from the
 * bitrate. The file is read 16 KB at a time, never all at once.
 *
 * WAV: PCM 8, 16, 24 or 32-bit and 32-bit float, mono or stereo.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "track.h"

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t le32(const uint8_t* p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t syncsafe(const uint8_t* p) {
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

// Read exactly n bytes at `offset`: false if the file ends first
static bool read_at(int fd, long offset, void* buf, size_t n) {
    if (lseek(fd, offset, SEEK_SET) != offset)
        return false;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (uint8_t*)buf + got, n - got);
        if (r <= 0)
            return false;
        got += (size_t)r;
    }
    return true;
}

//
// Tag text, for drawing
//

static size_t put_text(char* out, size_t at, size_t size, const char* text) {
    size_t n = strlen(text);
    if (at + n >= size)
        return at;
    memcpy(out + at, text, n);
    return at + n;
}

// One character of tag text, as the window draws text: a byte a cell,
// Latin-1 past ASCII (os/gfx.h). Others become their nearest plain letter
// or '?'.
static size_t put_char(char* out, size_t at, size_t size, uint32_t c) {
    const char* rep = NULL;
    switch (c) {
        case 0x2018: case 0x2019: case 0x201a: case 0x2032: rep = "'"; break;
        case 0x201c: case 0x201d: case 0x201e: case 0x2033: rep = "\""; break;
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: rep = "-"; break;
        case 0x2026: rep = "..."; break;
        case 0x2022: rep = "\xb7"; break;
        case 0x0152: rep = "OE"; break;
        case 0x0153: rep = "oe"; break;
        case 0x0160: rep = "S"; break;
        case 0x0161: rep = "s"; break;
        case 0x017d: rep = "Z"; break;
        case 0x017e: rep = "z"; break;
        case 0x0178: rep = "Y"; break;
        case 0x20ac: rep = "EUR"; break;
        case 0x2122: rep = "TM"; break;
    }
    if (rep == NULL) {
        if (c < 0x20 || (c >= 0x7f && c < 0xa0))
            c = ' ';
        else if (c > 0xff)
            c = '?';
        char one[2] = { (char)c, 0 };
        return put_text(out, at, size, one);
    }
    return put_text(out, at, size, rep);
}

static void trim(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
    size_t lead = 0;
    while (s[lead] == ' ')
        lead++;
    if (lead > 0)
        memmove(s, s + lead, n - lead + 1);
}

// An ID3 text: encoding byte, then the text (0 Latin-1, 1 UTF-16 with a
// BOM, 2 UTF-16BE, 3 UTF-8). Only the first string of a list is kept.
static void id3_text(const uint8_t* p, size_t len, char* out, size_t size) {
    out[0] = 0;
    if (len < 1 || size < 2)
        return;
    uint8_t enc = p[0];
    p++;
    len--;
    size_t at = 0;
    if (enc == 0 || enc > 3) {
        for (size_t i = 0; i < len && p[i] != 0; i++)
            at = put_char(out, at, size, p[i]);
    } else if (enc == 3) {
        for (size_t i = 0; i < len && p[i] != 0;) {
            uint32_t c = p[i];
            int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
            if (c >= 0x80 && more == 0) {
                c = '?';                // A stray continuation byte
            } else if (more > 0) {
                c &= 0x3f >> more;
                for (int k = 1; k <= more; k++)
                    c = (i + k < len) ? (c << 6) | (p[i + k] & 0x3f) : '?';
            }
            at = put_char(out, at, size, c);
            i += 1 + (size_t)more;
        }
    } else {
        bool big = (enc == 2);
        size_t i = 0;
        if (len >= 2 && p[0] == 0xff && p[1] == 0xfe) { big = false; i = 2; }
        else if (len >= 2 && p[0] == 0xfe && p[1] == 0xff) { big = true; i = 2; }
        for (; i + 1 < len; i += 2) {
            uint32_t c = big ? (uint32_t)((p[i] << 8) | p[i + 1]) : (uint32_t)(p[i] | (p[i + 1] << 8));
            if (c == 0)
                break;
            if (c >= 0xd800 && c < 0xdc00 && i + 3 < len) {
                uint32_t lo = big ? (uint32_t)((p[i + 2] << 8) | p[i + 3]) : (uint32_t)(p[i + 2] | (p[i + 3] << 8));
                if (lo >= 0xdc00 && lo < 0xe000) {
                    c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                    i += 2;
                }
            }
            at = put_char(out, at, size, c);
        }
    }
    out[at] = 0;
    trim(out);
}

// Undo ID3 unsynchronisation (FF 00 -> FF) in place; the new length
static size_t unsync(uint8_t* p, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        p[o++] = p[i];
        if (p[i] == 0xff && i + 1 < n && p[i + 1] == 0x00)
            i++;
    }
    return o;
}

#define ID3_READ_MAX    (64 * 1024)     // Text frames come first; pictures after are not needed

// The ID3v2 tag at the start: its size (0: none); title, artist, album filled
static long read_id3v2(track_t* t) {
    uint8_t h[10];
    if (!read_at(t->fd, 0, h, 10) || memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4 ||
        (h[6] | h[7] | h[8] | h[9]) & 0x80)
        return 0;
    uint8_t ver = h[3], flags = h[5];
    uint32_t size = syncsafe(h + 6);
    long total = 10 + (long)size + ((ver == 4 && (flags & 0x10)) ? 10 : 0);

    size_t n = size < ID3_READ_MAX ? size : ID3_READ_MAX;
    uint8_t* tag = (uint8_t*)malloc(n + 1);
    if (tag == NULL || !read_at(t->fd, 10, tag, n)) {
        free(tag);
        return total;
    }
    if ((flags & 0x80) && ver < 4)
        n = unsync(tag, n);             // v2.4 marks it per frame instead

    size_t p = 0;
    if (flags & 0x40) {                 // Extended header
        if (n < 4) { free(tag); return total; }
        uint32_t ext = (ver == 4) ? syncsafe(tag) : be32(tag) + 4;
        p = ext;
    }
    size_t head = (ver == 2) ? 6 : 10;
    while (p + head <= n) {
        const uint8_t* f = tag + p;
        if (f[0] == 0)
            break;                      // Padding
        uint32_t fsize;
        char id[5] = { 0 };
        uint16_t fflags = 0;
        if (ver == 2) {
            memcpy(id, f, 3);
            fsize = ((uint32_t)f[3] << 16) | (f[4] << 8) | f[5];
        } else {
            memcpy(id, f, 4);
            fsize = (ver == 4) ? syncsafe(f + 4) : be32(f + 4);
            fflags = (uint16_t)((f[8] << 8) | f[9]);
        }
        p += head;
        if (fsize > n - p)
            break;
        uint8_t* body = tag + p;
        size_t blen = fsize;
        p += fsize;
        if (ver == 4) {
            if (fflags & 0x000c)        // Compressed or encrypted: skip
                continue;
            if (fflags & 0x0001) {      // Data length indicator
                if (blen < 4) continue;
                body += 4;
                blen -= 4;
            }
            if (fflags & 0x0002)
                blen = unsync(body, blen);
        } else if (ver == 3 && (fflags & 0x00c0)) {
            continue;
        }
        char* dst = NULL;
        if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) dst = t->title;
        else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) dst = t->artist;
        else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) dst = t->album;
        if (dst != NULL && dst[0] == 0)
            id3_text(body, blen, dst, TRACK_TEXT_MAX);
    }
    free(tag);
    return total;
}

// A Latin-1 field of an ID3v1 tag, when there is no ID3v2 one
static void v1_field(const uint8_t* p, size_t n, char* out) {
    if (out[0] != 0)
        return;
    size_t at = 0;
    for (size_t i = 0; i < n && p[i] != 0; i++)
        at = put_char(out, at, TRACK_TEXT_MAX, p[i]);
    out[at] = 0;
    trim(out);
}

//
// MP3
//

// Fill the input buffer from the file, keeping what is left in it
static void mp3_refill(track_t* t) {
    if (t->in_pos > 0) {
        memmove(t->in, t->in + t->in_pos, t->in_len - t->in_pos);
        t->in_len -= t->in_pos;
        t->in_pos = 0;
    }
    if (t->in_eof)
        return;
    if (lseek(t->fd, t->next_read, SEEK_SET) != t->next_read) {
        t->in_eof = true;
        return;
    }
    while (t->in_len < TRACK_IN_BUF) {
        long room = (long)(TRACK_IN_BUF - t->in_len);
        if (t->next_read + room > t->data_end)
            room = t->data_end - t->next_read;
        if (room <= 0) {
            t->in_eof = true;
            break;
        }
        ssize_t r = read(t->fd, t->in + t->in_len, (size_t)room);
        if (r <= 0) {
            t->in_eof = true;
            break;
        }
        t->in_len += (size_t)r;
        t->next_read += r;
    }
}

// Start reading the stream again at `offset`
static void mp3_restart(track_t* t, long offset) {
    t->in_len = t->in_pos = 0;
    t->next_read = offset;
    t->in_eof = false;
    t->done = false;
    mp3dec_init(&t->dec);
}

// The Xing / Info or VBRI header in the first frame, if there is one.
// True when it is that kind of frame (not sound: skip it).
static bool mp3_vbr_header(track_t* t, const uint8_t* f, size_t len, int samples_per_frame) {
    if (len < 4)
        return false;
    bool mpeg1 = (f[1] & 0x08) != 0;
    bool mono = (f[3] >> 6) == 3;
    size_t off = 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
    uint32_t frames = 0, bytes = 0;
    bool found = false;
    if (off + 8 <= len && (!memcmp(f + off, "Xing", 4) || !memcmp(f + off, "Info", 4))) {
        found = true;
        t->vbr = !memcmp(f + off, "Xing", 4);
        uint32_t flags = be32(f + off + 4);
        size_t p = off + 8;
        if ((flags & 1) && p + 4 <= len) { frames = be32(f + p); p += 4; }
        if ((flags & 2) && p + 4 <= len) { bytes = be32(f + p); p += 4; }
        if ((flags & 4) && p + 100 <= len) {
            memcpy(t->toc, f + p, 100);
            t->have_toc = true;
        }
    } else if (36 + 18 <= len && !memcmp(f + 36, "VBRI", 4)) {
        found = true;
        t->vbr = true;
        bytes = be32(f + 36 + 10);
        frames = be32(f + 36 + 14);
    }
    if (!found)
        return false;
    if (frames > 0 && t->rate > 0) {
        t->duration = (double)frames * samples_per_frame / t->rate;
        long audio = bytes > 0 ? (long)bytes : t->data_end - t->data_start;
        if (t->duration > 0)
            t->bitrate_kbps = (int)(audio * 8.0 / t->duration / 1000.0 + 0.5);
    }
    return true;
}

static bool mp3_open(track_t* t, const char** why) {
    t->kind = TRACK_MP3;
    t->in = (uint8_t*)malloc(TRACK_IN_BUF);
    if (t->in == NULL) {
        *why = "Out of memory";
        return false;
    }
    long start = read_id3v2(t);
    t->data_end = t->file_size;
    if (t->file_size >= 128 + start) {
        uint8_t v1[128];
        if (read_at(t->fd, t->file_size - 128, v1, 128) && !memcmp(v1, "TAG", 3)) {
            t->data_end -= 128;
            v1_field(v1 + 3, 30, t->title);
            v1_field(v1 + 33, 30, t->artist);
            v1_field(v1 + 63, 30, t->album);
        }
    }
    if (start >= t->data_end) {
        *why = "No sound in the file";
        return false;
    }

    // The first frame
    mp3_restart(t, start);
    mp3_refill(t);
    mp3dec_frame_info_t info;
    memset(&info, 0, sizeof(info));
    int spf = mp3dec_decode_frame(&t->dec, t->in, (int)t->in_len, NULL, &info);
    if (info.frame_bytes <= 0 || spf <= 0 || info.hz <= 0) {
        *why = "Not an MP3 file";
        return false;
    }
    t->rate = (uint32_t)info.hz;
    t->channels = info.channels;
    t->layer = info.layer;
    t->bitrate_kbps = info.bitrate_kbps;
    t->data_start = start + info.frame_offset;

    const uint8_t* first = t->in + info.frame_offset;
    if (mp3_vbr_header(t, first, (size_t)info.frame_bytes, spf))
        t->data_start += info.frame_bytes;        // Not sound
    if (t->duration <= 0 && t->bitrate_kbps > 0)
        t->duration = (t->data_end - t->data_start) * 8.0 / (t->bitrate_kbps * 1000.0);
    mp3_restart(t, t->data_start);
    return true;
}

static size_t mp3_decode(track_t* t, int16_t* out) {
    while (!t->done) {                          // Each pass uses input or reads more
        if (t->in_len - t->in_pos < TRACK_IN_BUF / 2 && !t->in_eof)
            mp3_refill(t);
        size_t avail = t->in_len - t->in_pos;
        if (avail == 0) {
            t->done = true;
            break;
        }
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&t->dec, t->in + t->in_pos, (int)avail, t->pcm, &info);
        if (info.frame_bytes <= 0) {
            // No frame in what is there: more input, or the end
            if (t->in_eof) {
                t->done = true;
                break;
            }
            if (avail >= TRACK_IN_BUF)
                t->in_pos = t->in_len;          // A full buffer of junk: drop it
            mp3_refill(t);
            continue;
        }
        t->in_pos += (size_t)info.frame_bytes;
        if (samples <= 0)
            continue;                           // Skipped (junk, or the reservoir after a seek)
        if (info.channels == 2) {
            memcpy(out, t->pcm, (size_t)samples * 4);
        } else {
            for (int i = 0; i < samples; i++)
                out[i * 2] = out[i * 2 + 1] = t->pcm[i];
        }
        return (size_t)samples;
    }
    return 0;
}

static void mp3_seek(track_t* t, double seconds) {
    long span = t->data_end - t->data_start;
    long off = 0;
    if (t->duration > 0 && seconds > 0) {
        double f = seconds / t->duration;
        if (f > 1) f = 1;
        if (t->have_toc) {
            double pc = f * 100.0;
            int i = (int)pc;
            if (i > 99) i = 99;
            double a = t->toc[i];
            double b = (i < 99) ? t->toc[i + 1] : 256.0;
            off = (long)((a + (b - a) * (pc - i)) / 256.0 * span);
        } else {
            off = (long)(f * span);
        }
    }
    if (off < 0) off = 0;
    if (off > span) off = span;
    mp3_restart(t, t->data_start + off);
}

//
// WAV
//

static bool wav_open(track_t* t, const char** why) {
    t->kind = TRACK_WAV;
    uint8_t h[12];
    if (!read_at(t->fd, 0, h, 12)) {
        *why = "Not a WAV file";
        return false;
    }
    long p = 12;
    bool have_fmt = false;
    uint16_t tag = 0, ch = 0, bits = 0, block = 0;
    uint32_t rate = 0;
    for (;;) {
        uint8_t c[8];
        if (!read_at(t->fd, p, c, 8)) {
            *why = "No sound in the WAV file";
            return false;
        }
        uint32_t len = le32(c + 4);
        p += 8;
        if (!memcmp(c, "fmt ", 4) && len >= 16) {
            uint8_t f[40];
            size_t n = len < sizeof(f) ? len : sizeof(f);
            if (!read_at(t->fd, p, f, n))
                break;
            tag = le16(f);
            ch = le16(f + 2);
            rate = le32(f + 4);
            block = le16(f + 12);
            bits = le16(f + 14);
            if (tag == 0xfffe && n >= 26)
                tag = le16(f + 24);             // WAVE_FORMAT_EXTENSIBLE: the subformat
            have_fmt = true;
        } else if (!memcmp(c, "data", 4)) {
            if (!have_fmt)
                break;
            t->data_start = p;
            t->data_end = p + (long)len;
            if (t->data_end > t->file_size || len == 0xffffffffu)
                t->data_end = t->file_size;     // Cut short, or written while recording
            break;
        }
        p += (long)len + (len & 1);
        if (p > t->file_size)
            break;
    }
    if (!have_fmt || t->data_end <= t->data_start) {
        *why = "No sound in the WAV file";
        return false;
    }
    bool pcm = (tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32));
    bool flt = (tag == 3 && bits == 32);
    if ((!pcm && !flt) || (ch != 1 && ch != 2) || block != ch * (bits / 8) || rate == 0) {
        *why = "Only PCM WAV can be played (mono or stereo)";
        return false;
    }
    t->wav_tag = tag;
    t->wav_bits = bits;
    t->wav_block = block;
    t->rate = rate;
    t->channels = ch;
    t->bitrate_kbps = (int)((uint64_t)rate * block * 8 / 1000);
    t->duration = (double)((t->data_end - t->data_start) / block) / rate;
    t->wav_pos = t->data_start;
    t->in = (uint8_t*)malloc((size_t)TRACK_MAX_FRAMES * 8);
    if (t->in == NULL) {
        *why = "Out of memory";
        return false;
    }
    return true;
}

static int16_t wav_sample(const track_t* t, const uint8_t* p) {
    switch (t->wav_bits) {
        case 8:  return (int16_t)(((int)p[0] - 128) << 8);
        case 16: return (int16_t)le16(p);
        case 24: return (int16_t)(p[1] | (p[2] << 8));
        default:
            if (t->wav_tag == 3) {
                float f;
                uint32_t u = le32(p);
                memcpy(&f, &u, 4);
                if (!(f == f)) f = 0;           // NaN
                if (f > 1) f = 1;
                if (f < -1) f = -1;
                return (int16_t)(f * 32767.0f);
            }
            return (int16_t)(le32(p) >> 16);
    }
}

static size_t wav_decode(track_t* t, int16_t* out) {
    if (t->done)
        return 0;
    long left = (t->data_end - t->wav_pos) / t->wav_block;
    size_t want = left < TRACK_MAX_FRAMES ? (size_t)left : TRACK_MAX_FRAMES;
    if (want == 0 || lseek(t->fd, t->wav_pos, SEEK_SET) != t->wav_pos) {
        t->done = true;
        return 0;
    }
    size_t bytes = want * t->wav_block, got = 0;
    while (got < bytes) {
        ssize_t r = read(t->fd, t->in + got, bytes - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    size_t n = got / t->wav_block;
    if (n == 0) {
        t->done = true;
        return 0;
    }
    t->wav_pos += (long)(n * t->wav_block);
    size_t step = t->wav_bits / 8;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* f = t->in + i * t->wav_block;
        int16_t l = wav_sample(t, f);
        out[i * 2] = l;
        out[i * 2 + 1] = (t->channels == 2) ? wav_sample(t, f + step) : l;
    }
    return n;
}

static void wav_seek(track_t* t, double seconds) {
    if (seconds < 0)
        seconds = 0;
    long frames = (t->data_end - t->data_start) / t->wav_block;
    long at = (long)(seconds * t->rate);
    if (at > frames)
        at = frames;
    t->wav_pos = t->data_start + at * t->wav_block;
    t->done = false;
}

//
// Either
//

bool track_open(track_t* t, const char* path, const char** why) {
    memset(t, 0, sizeof(*t));
    t->fd = open(path, O_RDONLY);
    if (t->fd < 0) {
        *why = strerror(errno);
        return false;
    }
    struct stat st;
    if (fstat(t->fd, &st) < 0 || st.st_size <= 0) {
        *why = "The file is empty";
        track_close(t);
        return false;
    }
    t->file_size = (long)st.st_size;

    uint8_t h[12];
    bool ok;
    if (read_at(t->fd, 0, h, 12) && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4))
        ok = wav_open(t, why);
    else
        ok = mp3_open(t, why);
    if (!ok)
        track_close(t);
    return ok;
}

void track_close(track_t* t) {
    if (t->fd >= 0)
        close(t->fd);
    t->fd = -1;
    free(t->in);
    t->in = NULL;
}

size_t track_decode(track_t* t, int16_t* out) {
    if (t->fd < 0)
        return 0;
    return t->kind == TRACK_WAV ? wav_decode(t, out) : mp3_decode(t, out);
}

void track_seek(track_t* t, double seconds) {
    if (t->fd < 0)
        return;
    if (t->kind == TRACK_WAV)
        wav_seek(t, seconds);
    else
        mp3_seek(t, seconds);
}

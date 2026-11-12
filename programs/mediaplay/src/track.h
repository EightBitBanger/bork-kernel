/*
 * mediaplay: one file being decoded (track.c). MP3 (and MPEG layer 1/2)
 * through minimp3, PCM WAV read as it is. Either way it comes out as
 * 16-bit stereo frames at the file's own rate.
 */

#ifndef MEDIAPLAY_TRACK_H
#define MEDIAPLAY_TRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "minimp3.h"

#define TRACK_TEXT_MAX      128         // Title, artist, album (Latin-1, as os/gfx.h draws text)
#define TRACK_IN_BUF        16384       // Input kept for the decoder (minimp3 wants 16 KB to sync)
#define TRACK_MAX_FRAMES    (MINIMP3_MAX_SAMPLES_PER_FRAME / 2)     // Most frames one decode gives

typedef enum {
    TRACK_MP3 = 1,
    TRACK_WAV,
} track_kind_t;

typedef struct {
    track_kind_t kind;
    int          fd;
    long         file_size;

    // What it is
    uint32_t     rate;                  // Hz
    int          channels;              // 1 or 2 in the file
    int          bitrate_kbps;          // Average (0: not known)
    int          layer;                 // MP3: 1, 2 or 3
    bool         vbr;
    double       duration;              // Seconds (0: not known)
    char         title[TRACK_TEXT_MAX];
    char         artist[TRACK_TEXT_MAX];
    char         album[TRACK_TEXT_MAX];

    // Where the sound is in the file
    long         data_start;            // First audio frame (MP3) / first sample (WAV)
    long         data_end;              // After the last (MP3: before an ID3v1 tag)

    // MP3
    mp3dec_t     dec;
    bool         have_toc;              // Xing seek table
    uint8_t      toc[100];
    uint8_t*     in;                    // TRACK_IN_BUF bytes
    size_t       in_len, in_pos;
    long         next_read;             // File offset of the byte after in[in_len - 1]
    bool         in_eof;
    int16_t      pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    // WAV
    uint16_t     wav_tag;               // 1 PCM, 3 float
    uint16_t     wav_bits, wav_block;
    long         wav_pos;               // Next byte to read

    bool         done;                  // Nothing more to decode
} track_t;

// Open `path` and find out what it is. false with *why set (a short
// message for the window) when it can't be played.
bool track_open(track_t* t, const char* path, const char** why);
void track_close(track_t* t);

// The next bit of sound as 16-bit stereo at t->rate: how many frames
// (at most TRACK_MAX_FRAMES), 0 at the end.
size_t track_decode(track_t* t, int16_t* out);

// Go to `seconds` from the start (clamped). Exact for WAV and CBR MP3,
// close for VBR (Xing table) MP3.
void track_seek(track_t* t, double seconds);

#endif

#ifndef _OS_AUDIO_MIX_H
#define _OS_AUDIO_MIX_H

/*
 * The sound mixer's protocol: between libc (audio_open, os/audio.h) and the
 * volume service, audiosrv, which mixes in user space. Programs don't need
 * this file: they use os/audio.h and get the mixer without asking.
 *
 * While audiosrv runs it holds /dev/audio0 and serves port AUDIO_MIX_PORT.
 * audio_open(0) connects there and gets a stream of its own:
 *
 *   client -> AUDIO_MIX_HELLO (its name)
 *   server -> AUDIO_MIX_WELCOME, with a shared memory block attached:
 *             an audio_mix_shm_t header, then ring_frames S16 stereo frames
 *
 * The ring has one writer and one reader. The client writes samples and
 * moves write_pos; the server mixes from read_pos while state is RUNNING,
 * scaled by the stream's own level, and moves read_pos. Positions are
 * frame counts that wrap at 2^32; ring_frames is a power of two.
 *
 * Stop: the client sets state STOPPED, adds one to flush_req and waits for
 * flush_ack to match; the server has then dropped what was queued.
 *
 * The level and mute of a stream are the service's (its panel, or
 * AUDIO_MIX_VOLUME from the program); the server shows them in the header.
 * The client's pipe closing ends the stream.
 */

#include <stdint.h>
#include <os/audio.h>

#define AUDIO_MIX_PORT          "audiomix"
#define AUDIO_MIX_VERSION       1
#define AUDIO_MIX_MAGIC         0x58494D41u     /* "AMIX" */
#define AUDIO_MIX_NAME_MAX      32
#define AUDIO_MIX_HEADER        256             /* Samples start this far into the block */

/* Messages, client to server */
#define AUDIO_MIX_HELLO         1
#define AUDIO_MIX_VOLUME        2               /* level, mute */
#define AUDIO_MIX_NAME          3               /* A new name to show */
/* Server to client */
#define AUDIO_MIX_WELCOME       0x81

typedef struct {
    uint32_t type;
    uint32_t version;
    uint32_t level;                             /* AUDIO_MIX_VOLUME: 0..100 */
    uint32_t mute;
    char     name[AUDIO_MIX_NAME_MAX];          /* HELLO, NAME */
} audio_mix_msg_t;

typedef struct {
    uint32_t     type;                          /* AUDIO_MIX_WELCOME */
    uint32_t     version;
    int32_t      error;                         /* 0, or an errno: the shm block comes only with 0 */
    uint32_t     ring_frames;
    uint32_t     rate;                          /* The mix rate: the only one a stream takes */
    uint32_t     reserved;
    audio_info_t info;                          /* The device's, rates and buffer as the stream sees them */
} audio_mix_welcome_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t ring_frames;
    uint32_t rate;

    /* The client's */
    volatile uint32_t state;                    /* AUDIO_STATE_* */
    volatile uint32_t write_pos;
    volatile uint32_t flush_req;
    uint32_t          client_reserved[5];

    /* The server's */
    volatile uint32_t read_pos;
    volatile uint32_t flush_ack;
    volatile uint32_t played;                   /* Frames played while running, silence included */
    volatile uint32_t inflight;                 /* Taken from the ring, still queued on the device */
    volatile uint32_t underruns;
    volatile uint32_t level;                    /* 0..100 */
    volatile uint32_t mute;
    volatile uint32_t device_ok;                /* 0: the service can't get the device (busy elsewhere) */
    uint32_t          server_reserved[8];
} audio_mix_shm_t;

_Static_assert(sizeof(audio_mix_shm_t) <= AUDIO_MIX_HEADER, "header fits before the samples");

#endif

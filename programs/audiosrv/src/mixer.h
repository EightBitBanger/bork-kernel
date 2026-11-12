#ifndef AUDIOSRV_MIXER_H
#define AUDIOSRV_MIXER_H

/*
 * The mixer: programs' streams (libc audio_open, os/audio_mix.h) added up,
 * each at its own level, and played on /dev/audio0. The device is held
 * only while something plays (and a few seconds after), so a program that
 * opens it directly (audio_open_device) still can when all is quiet.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os/ipc.h>

#define MIXER_STREAMS   8

typedef struct {
    uint32_t id;                // Stays the same while the stream lives
    char     name[32];
    int      level;             // 0..100
    bool     mute;
    bool     playing;           // Running right now
} mixer_stream_info_t;

// Serve AUDIO_MIX_PORT. False if the port can't be made (the mixer is off).
bool    mixer_init(void);
void    mixer_shutdown(void);

// Handles to wait on (port and streams' pipes) into items; how many
size_t  mixer_wait_items(ipc_wait_item_t* items, size_t max);
// Milliseconds until mixer_tick() is due (-1: only when a handle is ready)
int32_t mixer_timeout(void);
// Connections, messages, the mix. True when the stream list, a name, a
// level or what plays changed (redraw the panel).
bool    mixer_tick(void);

// The streams for the panel, oldest first
size_t  mixer_streams(mixer_stream_info_t* out, size_t max);
void    mixer_set_level(uint32_t id, int level, bool mute);

// "Sound device in use by another program" while it can't be had, else NULL
const char* mixer_problem(void);

#endif

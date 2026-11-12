#ifndef _LIBC_AUDIO_INTERNAL_H
#define _LIBC_AUDIO_INTERNAL_H

/* Between audio.c (the device) and audio_mix.c (streams on audiosrv's mixer) */

#include <stdbool.h>
#include <stddef.h>
#include <os/audio.h>

/* Mixer stream descriptors: ours, well above any file descriptor */
#define AUDIO_MIX_FD_BASE   0x4D0000

extern char* __libc_argv0;              /* start.c: argv[0], or NULL */

bool __audio_mix_fd(int fd);
int  __audio_mix_open(void);
int  __audio_mix_close(int fd);
int  __audio_mix_info(int fd, audio_info_t* info);
int  __audio_mix_set_format(int fd, const audio_format_t* format);
int  __audio_mix_get_format(int fd, audio_format_t* format);
long __audio_mix_write(int fd, const void* data, size_t bytes);
int  __audio_mix_status(int fd, audio_status_t* status);
int  __audio_mix_start(int fd);
int  __audio_mix_pause(int fd);
int  __audio_mix_stop(int fd);
int  __audio_mix_set_volume(int fd, int left, int right, int mute);
int  __audio_mix_get_volume(int fd, audio_volume_t* volume);

#endif

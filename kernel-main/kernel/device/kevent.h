#ifndef _KERNEL_EVENT_H_
#define _KERNEL_EVENT_H_

#include <stdint.h>

#define  KEVENT_NAME_LENGTH_MAX       16
#define  KEVENT_ARG_LENGTH_MAX       256

struct KEvent {
    char name[KEVENT_NAME_LENGTH_MAX];
    char args[KEVENT_ARG_LENGTH_MAX];
    
    uint16_t flags;
};

#endif

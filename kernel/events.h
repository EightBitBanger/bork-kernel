#ifndef _KERNEL_EVENT_SYSTEM_H_
#define _KERNEL_EVENT_SYSTEM_H_

#include <kernel/programs/explorer/explorer.h>
#include <kernel/programs/notepad/notepad.h>

#include <kernel/events.h>

#define KEVENT_DEAD            0x0001
#define KEVENT_EXECUTE         0x0002
#define KEVENT_DWM_REFRESH     0x0004

void kernel_event_init(void);

bool kernel_event_send(uint8_t flags, const char* name, const char* arguments);
bool kernel_event_remove(const char* name);

void kernel_event_update(void);

#endif

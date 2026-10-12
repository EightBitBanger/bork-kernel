#ifndef WINDOW_MESSAGE_DISPATCH_H
#define WINDOW_MESSAGE_DISPATCH_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/dwm/dwm_events.h>
#include <kernel/dwm/objects/window_handle.h>

typedef struct {
    WindowHandle hwnd;
    wEvent       message;
    uint32_t     wparam;
    int32_t      lparam;
    uint32_t     time;      // For click delta checking later
} DWMMessage;

bool dwm_post_message(WindowHandle hwnd, wEvent message, uint32_t wparam, int32_t lparam);
bool dwm_get_message(DWMMessage* out_msg);
void dwm_dispatch_message(const DWMMessage* msg);

#endif

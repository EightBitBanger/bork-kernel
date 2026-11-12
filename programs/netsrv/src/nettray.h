#ifndef NETTRAY_H
#define NETTRAY_H

// The network tray service: everything but the event loop (main.c), so the
// host test can drive it a step at a time

#include <stdbool.h>
#include <stdint.h>
#include <os/wm.h>

#define NETTRAY_ICON_ID     1

// Held while netsrv runs, so a second copy can tell (an IPC port name)
#define NETTRAY_INSTANCE_PORT   "netsrv"

// The icon while connected: the arcs lit, and the ones not lit
#define NETTRAY_ICON_BLUE       0xFF3A9BFFu
#define NETTRAY_ICON_BLUE_DIM   0x703A9BFFu

// After wm_connect: trim permissions, find the adapter, put the icon up and
// start the first scan. 0, or -1 with errno: EEXIST when netsrv is already
// running (nothing was put up), else the icon could not be added.
int     nettray_start(void);

// One event from the dwm. False: stop (nothing asks for that now: the menu
// has no Exit)
bool    nettray_handle(const wm_event_t* ev);

// Work that is due (polling a scan). Call it after every wait.
void    nettray_tick(void);

// How long the loop may wait for the next event: ms, or -1 for no limit
int32_t nettray_timeout(void);

void    nettray_stop(void);

// For the host test: the list's scrolling, and its scroll bar's thumb
// (client coordinates)
void    nettray_scroll_state(int32_t* top, int32_t* max_top, gfx_rect_t* thumb);

#endif

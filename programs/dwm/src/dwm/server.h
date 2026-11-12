#ifndef DWM_SERVER_H
#define DWM_SERVER_H

/*
 * Window server: lets other programs have windows on this DWM.
 *
 * The DWM registers the port WM_PORT_NAME ("sys.dwm", os/wm_proto.h).
 * Programs connect with os/wm.h; each connection is a client, and each
 * window a client asks for is an ordinary DWM window whose procedure lives
 * here: it paints from the client's shared-memory pixel buffer and forwards
 * the window's events down the pipe.
 *
 * The DWM never waits on a client: requests are read without blocking,
 * and events go into a per-client outbox that is sent as the pipe has room
 * (mouse moves and resizes are merged, and the oldest mouse moves are
 * dropped first if a client stops reading). A client that hangs up loses
 * its windows.
 *
 * Theme and menus: WM_REQ_THEME answers with dwm_theme(), and a
 * change made with dwm_theme_set() reaches every client as
 * WM_EVENT_THEME_CHANGED. A window created WM_STYLE_CONTEXT_MENU gets its
 * client-area right clicks as WM_EVENT_CONTEXT_MENU and opens dwm menus with
 * WM_REQ_MENU (dwm_menu_show); choices come back as WM_EVENT_MENU_COMMAND.
 *
 * Drag and drop (drag.h): WM_REQ_DRAG turns the client's left press into a
 * file drag; a drop on a client window goes out as WM_EVENT_DROP, and only
 * that client can fetch the paths (WM_REQ_DROP_GET).
 *
 * Tray icons (tray.h): WM_REQ_TRAY_ADD / MODIFY / REMOVE put a client's
 * icons on the taskbar; clicks go back as WM_EVENT_TRAY, and right after
 * one the client may open a menu above the icon (WM_REQ_TRAY_MENU, choices
 * as WM_EVENT_TRAY_COMMAND). A client's icons go when it does.
 *
 * Main loop (dwm.c):
 *
 *     n = server_wait_items(items + 1, ...);   // what to wait on
 *     ipc_wait(items, n + 1, timeout);
 *     ...input...
 *     server_poll();                           // accept, read requests
 *     event_dispatch();                        // fills the outboxes
 *     server_flush();                          // send them
 */

#include <stdbool.h>
#include <stddef.h>
#include <os/ipc.h>

#include "event.h"

// The client and window tables grow as needed, up to these. Clients: the
// wait list holds the DWM's input, the port and one pipe per client, within
// IPC_WAIT_MAX. Windows: each holds a shm handle, and with the pipes and the
// handles queued to go out they stay well within IPC_HANDLE_MAX. In practice
// shared memory (SHM_TOTAL_MAX for every window buffer together) runs out
// before either.
#define SERVER_MAX_CLIENTS  (IPC_WAIT_MAX - 2)
#define SERVER_MAX_WINDOWS  384

_Static_assert(1 + SERVER_MAX_CLIENTS + SERVER_MAX_WINDOWS <= IPC_HANDLE_MAX / 2,
               "leave the DWM handles for the ones it sends out");

// Register the port. False (with a message) when it can't be: no SEC_IPC,
// no SEC_ADMIN for the "sys." name, or another process has it. The DWM
// then runs without client windows.
bool   server_init(void);

// Fill up to `max` wait items (port, client pipes). Returns how many.
size_t server_wait_items(ipc_wait_item_t* items, size_t max);

// Accept new clients, read and carry out requests, drop clients that hung up
void   server_poll(void);

// Send what is in the outboxes (as much as the pipes take)
void   server_flush(void);

// The app font (text.h) changed: tell every client on the next flush, so it
// fetches the font again
void   server_font_changed(void);

// Destroy every client window, close every connection and the port
void   server_shutdown(void);


#endif


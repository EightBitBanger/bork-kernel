#ifndef DWM_H
#define DWM_H

/*
 * User-space desktop window manager.
 *
 *     if (dwm_init() < 0) ...
 *     dwm_window_create(&desc);            // windows, a desktop procedure ...
 *     dwm_run();                           // until dwm_quit()
 *     dwm_shutdown();
 *
 * dwm_init() takes the keyboard and mouse (os/input.h), puts the pointer in
 * the middle of the screen, makes a full-screen surface, and loads the
 * system images (<home>/sys/images) into the image library: "cur_pointer"
 * is the cursor, "cur_angle" the resize cursor, "ui_close" the close box,
 * "ui_minimize" the minimize box and "ui_start" the Start button, each
 * drawn plainly when missing. Font files in
 * <home>/sys/fonts (known by their header, whatever their name) go into the font library
 * (resources/fontlib.h); the folder is optional. The DWM's own text uses
 * one of them when there is one (text.h).
 *
 * One thread: window procedures run inside dwm_run(), one event at a time.
 *
 * Other programs get windows too: dwm_init() registers the port
 * WM_PORT_NAME ("sys.dwm") and dwm_run() serves it (server.h). A program
 * links the libc's os/wm.h, draws into its window's shared-memory buffer and
 * gets the window's events back over IPC.
 *
 * Needs SEC_FILE_READ, SEC_GFX, SEC_GFX_SCREEN and SEC_INPUT; SEC_IPC and
 * SEC_ADMIN (for the "sys." port name) to serve other programs.
 *
 * The DWM owns the screen, so it never prints: anything worth saying is a
 * notification (notify.h). Before the screen exists there is nowhere to say
 * it, so dwm_init() failures only set errno.
 */

#include "background.h"
#include "clipboard.h"
#include "drag.h"
#include "event.h"
#include "icons.h"
#include "menu.h"
#include "notify.h"
#include "taskbar.h"
#include "text.h"
#include "theme.h"
#include "timer.h"
#include "window.h"

// Start the DWM. 0, or -1 with errno:
// EPERM (missing clearance), ENODEV (no display), or what creating the
// screen or taking the input gave (ENOMEM, EBUSY, ...).
int  dwm_init(void);

// Run the main loop: input -> events -> procedures -> repaint dirty
// regions. Returns when dwm_quit() is called (0) or input fails (-1).
int  dwm_run(void);

// Make dwm_run() return after the current round
void dwm_quit(void);

// Destroy every window (each gets DESTROY), give back the screen and the
// input. The kernel repaints its own screen afterwards.
void dwm_shutdown(void);

// Screen size in pixels
int32_t dwm_screen_width(void);
int32_t dwm_screen_height(void);

// Files changed (the desktop made, renamed or removed one): every program
// gets WM_EVENT_FS_CHANGED this round, as with wm_fs_changed (os/wm.h),
// and so does the desktop procedure (DWM_EVENT_FS_CHANGED)
void dwm_fs_changed(void);

#endif


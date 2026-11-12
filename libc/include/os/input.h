#ifndef _OS_INPUT_H
#define _OS_INPUT_H

/*
 * Keyboard and mouse. Needs SEC_INPUT (os/security.h).
 *
 * input_open() takes both devices for this process: the window manager
 * stops reading input and stops drawing until the handle is closed (or the
 * process exits), then repaints the screen. Only one process can hold them
 * (EBUSY otherwise). The first event is always a mouse event with the
 * current position and buttons, so a program knows where the pointer is
 * without waiting for it to move.
 *
 *     int in = input_open();
 *     input_event_t ev[16];
 *     while (input_wait(in, IPC_FOREVER) > 0) {
 *         int n = input_read(in, ev, 16);
 *         for (int i = 0; i < n; i++) ...
 *     }
 *
 * Keys: presses only; `key` is the character (Shift applied) or KEY_*. The
 * one exception is KEY_ALT_UP: both Alt keys are up again (for Alt+Tab).
 * Mouse: absolute screen position and the buttons held (INPUT_BTN_*).
 * `mods` (INPUT_MOD_*) is the modifier keys when the event was read.
 */

#include <stddef.h>
#include <stdint.h>
#include <os/syscall.h>
#include <os/ipc.h>

typedef SysInputEvent input_event_t;

#define INPUT_MOUSE         SYS_INPUT_MOUSE
#define INPUT_KEY           SYS_INPUT_KEY

#define INPUT_BTN_LEFT      SYS_INPUT_BTN_LEFT
#define INPUT_BTN_RIGHT     SYS_INPUT_BTN_RIGHT
#define INPUT_BTN_MIDDLE    SYS_INPUT_BTN_MIDDLE

#define INPUT_MOD_SHIFT     SYS_INPUT_MOD_SHIFT
#define INPUT_MOD_CTRL      SYS_INPUT_MOD_CTRL
#define INPUT_MOD_ALT       SYS_INPUT_MOD_ALT

#define INPUT_READ_MAX      SYS_INPUT_READ_MAX

#define KEY_BACKSPACE       SYS_KEY_BACKSPACE
#define KEY_ENTER           SYS_KEY_ENTER
#define KEY_TAB             SYS_KEY_TAB
#define KEY_ESCAPE          SYS_KEY_ESCAPE
#define KEY_HOME            SYS_KEY_HOME
#define KEY_UP              SYS_KEY_UP
#define KEY_PAGE_UP         SYS_KEY_PAGE_UP
#define KEY_LEFT            SYS_KEY_LEFT
#define KEY_RIGHT           SYS_KEY_RIGHT
#define KEY_END             SYS_KEY_END
#define KEY_DOWN            SYS_KEY_DOWN
#define KEY_PAGE_DOWN       SYS_KEY_PAGE_DOWN
#define KEY_DELETE          SYS_KEY_DELETE
#define KEY_F1              SYS_KEY_F1
#define KEY_F2              SYS_KEY_F2
#define KEY_F3              SYS_KEY_F3
#define KEY_F4              SYS_KEY_F4
#define KEY_F5              SYS_KEY_F5
#define KEY_F6              SYS_KEY_F6
#define KEY_F7              SYS_KEY_F7
#define KEY_F8              SYS_KEY_F8
#define KEY_F9              SYS_KEY_F9
#define KEY_F10             SYS_KEY_F10
#define KEY_F11             SYS_KEY_F11
#define KEY_F12             SYS_KEY_F12
#define KEY_ALT_UP          SYS_KEY_ALT_UP      // Alt let go (no key; see below)

// Take the keyboard and mouse. Handle, or -1 with errno (EBUSY, EPERM).
int input_open(void);

// Up to `max` queued events (at most INPUT_READ_MAX per call). Returns
// how many, 0 when none are waiting.
int input_read(int handle, input_event_t* events, size_t max);

// 1 when events are waiting, 0 on timeout (timeout_ms < 0: forever)
int input_wait(int handle, int32_t timeout_ms);

// Give the devices back (same as ipc_close)
int input_close(int handle);

// Move the mouse pointer to (x, y) (clamped to the screen). Only the process
// holding the input (this handle) can. Events already queued keep their old
// positions; the next movement starts from (x, y). 0, or -1 with errno
// (EBADF: not an open input handle, EPERM: no SEC_INPUT).
int input_warp(int handle, int32_t x, int32_t y);

#endif


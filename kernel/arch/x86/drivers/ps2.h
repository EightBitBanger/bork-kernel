#ifndef _PS2_DRIVER_
#define _PS2_DRIVER_

#define PS2_STATUS_REG         0x64
#define PS2_DATA_REG           0x60
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_MOUSE_DATA  0x20

#include <stdint.h>
#include <stdbool.h>

// Check if the keyboard has a character ready
bool ps2_check_keyboard(void);

// Route keyboard input to the console
void ps2_route_console(void);

// Flush out any stale keys left in the keyboard input buffer
void flush_keyboard_buffer(void);

#endif

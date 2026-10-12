#ifndef _CURSOR_IMAGE_LIBRARY_H_
#define _CURSOR_IMAGE_LIBRARY_H_

#include <kernel/dwm/configuration.h>

#ifdef ADD_IMAGE_LIB

#include <stdint.h>
#include <kernel/arch/x86/drivers/display/draw.h>

extern const struct Sprite rc_cursor_pointer;
extern const struct Sprite rc_cursor_edge;
extern const struct Sprite rc_cursor_angle;

#endif
#endif

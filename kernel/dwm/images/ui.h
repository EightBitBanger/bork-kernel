#ifndef _UI_IMAGE_LIBRARY_H_
#define _UI_IMAGE_LIBRARY_H_

#include <kernel/dwm/configuration.h>

#ifdef ADD_IMAGE_LIB

#include <stdint.h>
#include <kernel/arch/x86/drivers/display/draw.h>

extern const struct Sprite rc_button_minimize;
extern const struct Sprite rc_button_close;
extern const struct Sprite rc_button_close_red;
extern const struct Sprite rc_button_close_purple;

extern const struct Sprite rc_button_check;
extern const struct Sprite rc_button_x;
extern const struct Sprite rc_button_plus;
extern const struct Sprite rc_button_arrow_down;
extern const struct Sprite rc_button_arrow_up;

extern const struct Sprite rc_button_back;
extern const struct Sprite rc_button_new;

extern const struct Sprite rc_button_power;

extern const struct Sprite rc_button;

#endif
#endif

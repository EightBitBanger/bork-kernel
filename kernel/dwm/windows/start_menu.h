#ifndef _START_MENU_WINDOW_H_
#define _START_MENU_WINDOW_H_

#include <kernel/dwm/dwm.h>

#define DWM_START_MENU_WIDTH     220
#define DWM_START_MENU_HEIGHT    230     // Room for the power box and the search box

#define DWM_START_BUTTON_X       0
#define DWM_START_BUTTON_Y       0
#define DWM_START_BUTTON_W       30
#define DWM_START_BUTTON_H       30

WindowHandle dwm_start_menu_open(void);

void dwm_start_menu_close(void);

void dwm_start_menu_toggle(void);

bool dwm_start_menu_is_open(void);

#endif

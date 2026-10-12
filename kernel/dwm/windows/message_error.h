#ifndef _ERROR_MESSAGE_WINDOW_H_
#define _ERROR_MESSAGE_WINDOW_H_

#include <kernel/dwm/dwm.h>

WindowHandle dwm_summon_message_error(const char* title, const char* message, const char* details, const char* icon);

#endif

#ifndef _DIALOG_DELETE_WINDOW_H_
#define _DIALOG_DELETE_WINDOW_H_

#include <kernel/dwm/dwm.h>

WindowHandle dwm_summon_dialog_delete(const char* title, const char* file_path, WindowHandle parent, uint16_t icon_index);

#endif

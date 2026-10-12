#ifndef _DWM_WINDOW_CLASS_H_
#define _DWM_WINDOW_CLASS_H_

#include <stdint.h>

#include <kernel/dwm/configuration.h>

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint16_t max_width;
    uint16_t max_height;
    char title[DWM_MAX_TITLE_LEN];
} WindowClass;

#endif

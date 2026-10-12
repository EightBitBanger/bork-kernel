#ifndef PROGRAM_EXPLORER_H
#define PROGRAM_EXPLORER_H

#include <kernel/events.h>

//#define _DEBUG_DRAW_ENABLE_

#define MAX_ITEMS                   64
#define MAX_TITLE_LEN               64
#define MAX_PATH_LEN               256
#define MAX_RENAME_WIDTH           140

#define ICON_FOLDER                  0
#define ICON_FILE                    1
#define ICON_DOCUMENT                2
#define ICON_SYSTEM                  3
#define ICON_STORAGE                 4

#define MAX_PATH_DEPTH               5

#define PATH_FIELD_X                 0
#define PATH_FIELD_Y                 4

// Window
#define WINDOW_WIDTH                 480
#define WINDOW_HEIGHT                350

// Button placements
#define BACK_BTN_X                   4
#define BACK_BTN_Y                   4

// Background Bounds
#define EXPLORER_BG_X                0
#define EXPLORER_BG_Y                0

// Back Button Geometry
#define BACK_BTN_CONTAINER_X         5 + BACK_BTN_X
#define BACK_BTN_CONTAINER_Y         3 + BACK_BTN_Y
#define BACK_BTN_CONTAINER_W        18
#define BACK_BTN_CONTAINER_H        16

#define BACK_BTN_BORDER_X            4 + BACK_BTN_X
#define BACK_BTN_BORDER_Y            2 + BACK_BTN_Y
#define BACK_BTN_BORDER_W           20
#define BACK_BTN_BORDER_H           18

#define BACK_BTN_SPRITE_X            4 + BACK_BTN_X
#define BACK_BTN_SPRITE_Y            0 + BACK_BTN_Y

// Path Text Input Field Geometry
#define PATH_FIELD_BG_X             35 + PATH_FIELD_X
#define PATH_FIELD_BG_Y              3 + PATH_FIELD_Y
#define PATH_FIELD_BG_H             16

#define PATH_FIELD_BORDER_X         34 + PATH_FIELD_X
#define PATH_FIELD_BORDER_Y          2 + PATH_FIELD_Y
#define PATH_FIELD_BORDER_H         18

// Path Text Typography
#define PATH_TEXT_X                 40 + PATH_FIELD_X
#define PATH_TEXT_Y                  7 + PATH_FIELD_Y
#define PATH_FONT_CHAR_WIDTH         6

// Navigation Divider Line
#define NAV_DIVIDER_X                0
#define NAV_DIVIDER_Y               30
#define NAV_DIVIDER_H                0 

// Grid Layout Dimensions
#define NAV_X                        5
#define NAV_Y                       38
#define NAV_Y_OFF                    7

// Grid Item Typography
#define ITEM_WIDTH                  72
#define ITEM_HEIGHT                 65
#define ITEM_TEXT_HEIGHT_OFF        44
#define ITEM_FONT_CHAR_WIDTH         6

void explorer_main(const char* arguments);

#endif

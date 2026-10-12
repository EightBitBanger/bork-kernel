#ifndef PROGRAM_NOTEPAD_H
#define PROGRAM_NOTEPAD_H

#include <kernel/kernel.h>
#include <kernel/dwm/dwm.h>
#include <kernel/events.h>
#include <kernel/util/string.h>

#include <kernel/programs/notepad/internal.h>
#include <kernel/programs/notepad/notepad.h>
#include <kernel/malloc.h>

// Background Bounds
#define NOTEPAD_BG_X                 0
#define NOTEPAD_BG_Y                 0

// Text Area Layout
#define TEXT_PADDING_X               8
#define TEXT_PADDING_Y               8
#define TEXT_FONT_CHAR_WIDTH         6
#define TEXT_FONT_LINE_HEIGHT       12

WindowHandle notepad_main(const char* arguments);

#endif

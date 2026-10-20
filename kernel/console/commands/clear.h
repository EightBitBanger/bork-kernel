#ifndef CONSOLE_CMD_CLEAR_H
#define CONSOLE_CMD_CLEAR_H

#include <stdint.h>

#include <kernel/kernel.h>
#include <kernel/console/display.h>

int console_cmd_clear(int arg_count, char** args) {
    (void)arg_count;
    (void)args;
    
    // display_clear() repaints and flushes the screen itself
    display_clear();
    display_cursor_set_position(0);
    display_cursor_set_line(0);
    return 0;
}

#endif

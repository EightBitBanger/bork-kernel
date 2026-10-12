
#ifndef DISPLAY_IO_H
#define DISPLAY_IO_H

#include <stdint.h>

void display_init(void);

uint16_t display_get_width(void);
uint16_t display_get_height(void);

// Text grid size: columns = characters per line, rows = lines per screen
uint16_t display_get_rows(void);
uint16_t display_get_columns(void);

uint8_t display_get_glyph_width(void);
uint8_t display_get_glyph_height(void);

// Write a character at the cursor (the cursor does not move)
void display_putc(const char ch);

// Scroll the text up one line (the cursor does not move)
void display_newline(void);

void display_cursor_set_position(uint16_t position);
void display_cursor_set_line(uint16_t line);
uint16_t display_cursor_get_position(void);
uint16_t display_cursor_get_line(void);

// Blank the whole screen (the cursor does not move)
void display_clear(void);

// Repaint everything, e.g. after something else drew over the console
void display_invalidate(void);

// Batch updates: nothing reaches the screen until the outermost
// display_end_update(), which then draws only what changed. Nests.
void display_begin_update(void);
void display_end_update(void);

void display_busy_wait(void);
uint32_t display_get_device_address(void);

#endif

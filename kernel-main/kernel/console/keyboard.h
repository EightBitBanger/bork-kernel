#ifndef KEYBOARD_IO_H
#define KEYBOARD_IO_H

#include <stdint.h>
#include <stdbool.h>

void kb_init(void);

uint16_t kb_getc(void);

// Decode one Scan Code Set 1 byte (prefixes E0 / E1 included) and track the
// modifier and virtual key state. Returns the key, as kb_getc() does.
uint16_t kb_decode_scancode(uint8_t scancode);
void kb_flush(void);

uint8_t kb_check_input_state(void);
uint8_t kb_get_current_char(void);
void kb_clear_input_state(void);

void kb_isr_callback(void);
void kb_event_handler(void);

bool kb_shift_down(void);
bool kb_ctrl_down(void);
bool kb_alt_down(void);

#endif

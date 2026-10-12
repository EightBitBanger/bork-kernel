#ifndef KEYBOARD_IO_H
#define KEYBOARD_IO_H

#include <stdint.h>
#include <stdbool.h>

void kb_init(void);

uint16_t kb_getc(void);
void kb_flush(void);

uint8_t kb_check_input_state(void);
uint8_t kb_get_current_char(void);
void kb_clear_input_state(void);

void kb_isr_callback(void);
void kb_event_handler(void);

// Feed one decoded key (as returned by kb_getc / queued by the keyboard IRQ)
// to the console line editor. Thread context only.
void kb_console_key(uint16_t key);

bool kb_shift_down(void);
bool kb_ctrl_down(void);
bool kb_alt_down(void);

#endif

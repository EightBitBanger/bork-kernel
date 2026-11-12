#ifndef _KERNEL_X86_SERIAL_H_
#define _KERNEL_X86_SERIAL_H_

#include <stdint.h>
#include <stdbool.h>

// COM1 text output (115200 8N1, polled). Safe to call from any context,
// including with interrupts off. Every call is a no-op when no UART is
// present, so it can stay in release builds.
//
// The watchdog (scheduler/watchdog.c) programs the same port with the same
// settings; calling both inits is harmless.

bool serial_init(void);
bool serial_present(void);

// '\n' is sent as "\r\n"
void serial_write(const char* text, uint32_t length);
void serial_puts(const char* text);

void serial_put_hex(uint32_t value);    // 0x0000abcd
void serial_put_dec(uint32_t value);
void serial_put_int(int32_t value);

#endif

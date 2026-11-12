#ifndef _KERNEL_BOOT_RPI3_UART_H_
#define _KERNEL_BOOT_RPI3_UART_H_

#include <stdbool.h>

// PL011 serial console, 115200 8N1 on GPIO 14 / 15 (header pins 8 / 10).
// The early-boot stand-in for kernel/arch/x86/drivers/serial.c.

void uart_init(void);
void uart_putc(char c);                 // '\n' goes out as "\r\n"
void uart_puts(const char* str);
void uart_printf(const char* format, ...) __attribute__((format(printf, 1, 2)));

bool uart_has_char(void);
char uart_getc(void);                   // Blocks

#endif

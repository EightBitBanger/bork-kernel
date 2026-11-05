// PL011 UART0 on a Raspberry Pi 3

#include <stdarg.h>
#include <stdint.h>

#include <kernel/arch/aarch64/mmio.h>
#include <kernel/boot/rpi3/board.h>
#include <kernel/boot/rpi3/uart.h>
#include <kernel/util/snprint.h>

#define UART_DR         (UART0_BASE + 0x00)
#define UART_FR         (UART0_BASE + 0x18)
#define UART_IBRD       (UART0_BASE + 0x24)
#define UART_FBRD       (UART0_BASE + 0x28)
#define UART_LCRH       (UART0_BASE + 0x2C)
#define UART_CR         (UART0_BASE + 0x30)
#define UART_IMSC       (UART0_BASE + 0x38)
#define UART_ICR        (UART0_BASE + 0x44)

#define FR_RXFE         (1u << 4)       // Receive FIFO empty
#define FR_TXFF         (1u << 5)       // Transmit FIFO full

#define GPFSEL1         (GPIO_BASE + 0x04)
#define GPPUD           (GPIO_BASE + 0x94)
#define GPPUDCLK0       (GPIO_BASE + 0x98)

#define GPIO_ALT0       4u

void uart_init(void) {
    mmio_write32(UART_CR, 0);           // Off while it is reconfigured

    // GPIO 14 / 15 to ALT0 (TXD0 / RXD0)
    uint32_t sel = mmio_read32(GPFSEL1);
    sel &= ~((7u << 12) | (7u << 15));
    sel |= (GPIO_ALT0 << 12) | (GPIO_ALT0 << 15);
    mmio_write32(GPFSEL1, sel);

    // No pull-up / pull-down on either pin (the BCM2837 clocks this in)
    mmio_write32(GPPUD, 0);
    mmio_delay(150);
    mmio_write32(GPPUDCLK0, (1u << 14) | (1u << 15));
    mmio_delay(150);
    mmio_write32(GPPUDCLK0, 0);

    mmio_write32(UART_ICR, 0x7FF);      // Clear pending interrupts

    // 115200 baud from the 48 MHz UART clock (config.txt init_uart_clock):
    // 48e6 / (16 * 115200) = 26.0417 -> IBRD 26, FBRD round(0.0417 * 64) = 3
    mmio_write32(UART_IBRD, 26);
    mmio_write32(UART_FBRD, 3);

    mmio_write32(UART_LCRH, (1u << 4) | (3u << 5));     // FIFOs on, 8 bits, no parity, 1 stop
    mmio_write32(UART_IMSC, 0);                         // Polled for now
    mmio_write32(UART_CR, (1u << 0) | (1u << 8) | (1u << 9));  // UARTEN, TXE, RXE
}

static void uart_put_raw(char c) {
    while (mmio_read32(UART_FR) & FR_TXFF) {}
    mmio_write32(UART_DR, (uint32_t)(uint8_t)c);
}

void uart_putc(char c) {
    if (c == '\n')
        uart_put_raw('\r');
    uart_put_raw(c);
}

void uart_puts(const char* str) {
    while (*str)
        uart_putc(*str++);
}

void uart_printf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    uart_puts(buffer);
}

bool uart_has_char(void) {
    return (mmio_read32(UART_FR) & FR_RXFE) == 0;
}

char uart_getc(void) {
    while (!uart_has_char()) {}
    return (char)(mmio_read32(UART_DR) & 0xFF);
}

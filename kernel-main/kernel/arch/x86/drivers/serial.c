#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/drivers/serial.h>

#define COM1            0x3F8
#define COM_LSR_THRE    0x20        // Transmit holding register empty
#define COM_TX_SPINS    100000      // Give up on a stuck UART instead of hanging

static bool serial_ok = false;

bool serial_init(void) {
    // Scratch register test: no UART, no serial output
    outb(COM1 + 7, 0xA5);
    if (inb(COM1 + 7) != 0xA5) {
        serial_ok = false;
        return false;
    }

    outb(COM1 + 1, 0x00);   // No serial interrupts
    outb(COM1 + 3, 0x80);   // DLAB on
    outb(COM1 + 0, 0x01);   // 115200 baud
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   // 8N1, DLAB off
    outb(COM1 + 2, 0xC7);   // FIFO on, cleared
    outb(COM1 + 4, 0x03);   // DTR + RTS

    serial_ok = true;
    return true;
}

bool serial_present(void) {
    return serial_ok;
}

static void serial_putc(char c) {
    for (int i = 0; i < COM_TX_SPINS && !(inb(COM1 + 5) & COM_LSR_THRE); i++) { }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char* text, uint32_t length) {
    if (!serial_ok || text == 0)
        return;
    for (uint32_t i = 0; i < length; i++) {
        if (text[i] == '\n')
            serial_putc('\r');
        serial_putc(text[i]);
    }
}

void serial_puts(const char* text) {
    if (!serial_ok || text == 0)
        return;
    while (*text) {
        if (*text == '\n')
            serial_putc('\r');
        serial_putc(*text++);
    }
}

void serial_put_hex(uint32_t value) {
    char buf[11] = "0x";
    for (int i = 0; i < 8; i++)
        buf[2 + i] = "0123456789abcdef"[(value >> (28 - 4 * i)) & 0xF];
    buf[10] = '\0';
    serial_puts(buf);
}

void serial_put_dec(uint32_t value) {
    char digits[11];
    int n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value != 0);

    char buf[11];
    int i = 0;
    while (n > 0) buf[i++] = digits[--n];
    buf[i] = '\0';
    serial_puts(buf);
}

void serial_put_int(int32_t value) {
    if (value < 0) {
        serial_puts("-");
        serial_put_dec((uint32_t)0 - (uint32_t)value);
    } else {
        serial_put_dec((uint32_t)value);
    }
}

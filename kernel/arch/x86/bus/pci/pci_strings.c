#include <kernel/util/string.h>

void format_device_string(char* buf, uint8_t dev, uint8_t func) {
    buf[0] = '0' + (dev / 10);
    buf[1] = '0' + (dev % 10);
    buf[2] = ':';
    buf[3] = '0' + (func / 10);
    buf[4] = '0' + (func % 10);
    buf[5] = '\0';
}

void format_bus_string(char* buf, uint8_t bus) {
    buf[0] = 'b'; buf[1] = 'u'; buf[2] = 's';
    if (bus < 10) {
        buf[3] = '0' + bus;
        buf[4] = '\0';
    } else {
        buf[3] = '0' + (bus / 10);
        buf[4] = '0' + (bus % 10);
        buf[5] = '\0';
    }
}

void format_hex32_string(char* buf, uint32_t val) {
    buf[0] = '0';
    buf[1] = 'x';
    u32tox(val, &buf[2]);
}

void format_hex16_string(char* buf, uint16_t val) {
    buf[0] = '0';
    buf[1] = 'x';
    u16tox(val, &buf[2]);
}

void format_hex8_string(char* buf, uint8_t val) {
    buf[0] = '0';
    buf[1] = 'x';
    u8tox(val, &buf[2]);
}

void format_dec8_string(char* buf, uint8_t val) {
    size_t len = 0;
    if (val >= 100) {
        buf[len++] = '0' + (val / 100);
        buf[len++] = '0' + ((val % 100) / 10);
        buf[len++] = '0' + (val % 10);
    } else if (val >= 10) {
        buf[len++] = '0' + (val / 10);
        buf[len++] = '0' + (val % 10);
    } else {
        buf[len++] = '0' + val;
    }
    buf[len] = '\0';
}

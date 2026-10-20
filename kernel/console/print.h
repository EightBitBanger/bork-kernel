#ifndef PRINT_H
#define PRINT_H

#include <stdint.h>

void print(const char* str);
// Erase the character before the cursor and move back onto it
void print_backspace(void);

// The cell the next printed character would land in. A pending wrap (a
// line printed exactly to the right edge) resolves to column 0 of the next
// line, which may be one past the last row. Ends the pending wrap: the
// caller takes over the cursor from here.
void print_next_position(int32_t* column, int32_t* line);

void print_int(int32_t value);

void print_hex(uint8_t value);
void print_hex16(uint16_t value);
void print_hex32(uint32_t value);

void print_bits(uint8_t value);
void print_bits16(uint16_t value);
void print_bits32(uint32_t value);

#endif

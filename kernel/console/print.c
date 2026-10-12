
#include <stdbool.h>

#include <kernel/console/display.h>
#include <kernel/console/print.h>

#include <kernel/util/string.h>

// The cursor lives in the display driver; print() only reads and moves it
// through the display API. (It used to declare the driver's uint16_t cursor
// variables as uint8_t, so columns past 255 wrapped.)
//
// Deferred wrap: after a character lands in the last column the cursor stays
// on that cell with a wrap pending, and the next printable character moves to
// the next line first. So a line that is exactly full followed by "\n" is one
// line break, not two. Anyone else moving the cursor (cls, a progress line
// returning to column 0) cancels the pending wrap automatically.

#define PRINT_TAB_WIDTH  4

static bool     wrap_pending = false;
static uint16_t wrap_line    = 0;

static bool print_wrap_pending(void) {
    if (!wrap_pending)
        return false;

    uint16_t columns = display_get_columns();
    if (columns == 0 ||
        display_cursor_get_position() != columns - 1 ||
        display_cursor_get_line() != wrap_line) {
        wrap_pending = false;   // The cursor was moved since
    }
    return wrap_pending;
}

static void print_line_feed(void) {
    wrap_pending = false;

    uint16_t rows = display_get_rows();
    uint16_t line = display_cursor_get_line();

    display_cursor_set_position(0);
    if (rows == 0)
        return;

    if (line + 1 < rows)
        display_cursor_set_line((uint16_t)(line + 1));
    else
        display_newline();      // Scroll; the cursor stays on the last line
}

static void print_char(uint8_t ch) {
    uint16_t columns = display_get_columns();
    if (columns == 0)
        return;

    switch (ch) {
        case '\n':
            print_line_feed();
            return;

        case '\r':
            wrap_pending = false;
            display_cursor_set_position(0);
            return;

        case '\t': {
            if (print_wrap_pending())
                print_line_feed();
            uint16_t spaces = PRINT_TAB_WIDTH - (display_cursor_get_position() % PRINT_TAB_WIDTH);
            for (uint16_t i = 0; i < spaces; i++)
                print_char(' ');
            return;
        }

        default:
            break;
    }

    if (print_wrap_pending())
        print_line_feed();

    uint16_t column = display_cursor_get_position();
    display_putc((char)ch);

    if (column + 1 < columns) {
        display_cursor_set_position((uint16_t)(column + 1));
    } else {
        wrap_pending = true;
        wrap_line    = display_cursor_get_line();
    }
}

void print(const char* str) {
    if (str == NULL)
        return;

    // One screen update for the whole string, however many lines it scrolls
    display_begin_update();
    while (*str)
        print_char((uint8_t)*str++);
    display_end_update();
}

void print_backspace(void) {
    display_begin_update();

    if (print_wrap_pending()) {
        // The last character is under the cursor (last column of the line)
        wrap_pending = false;
        display_putc(' ');
    } else {
        uint16_t position = display_cursor_get_position();
        uint16_t line     = display_cursor_get_line();

        if (position > 0) {
            position--;
        } else if (line > 0) {
            line--;
            position = (uint16_t)(display_get_columns() - 1);
        } else {
            display_end_update();
            return;
        }

        display_cursor_set_position(position);
        display_cursor_set_line(line);
        display_putc(' ');
    }

    display_end_update();
}

void print_int(int32_t value) {
    char number_string[12];

    // utos is unsigned: print the sign ourselves (INT32_MIN included)
    if (value < 0) {
        print("-");
        utos((uint32_t)0 - (uint32_t)value, number_string);
    } else {
        utos((uint32_t)value, number_string);
    }

    print(number_string);
}

void print_hex(uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char buffer[3] = { digits[value >> 4], digits[value & 0x0F], '\0' };
    print(buffer);
}

void print_hex16(uint16_t value) {
    print_hex((uint8_t)(value >> 8));
    print_hex((uint8_t)value);
}

void print_hex32(uint32_t value) {
    print_hex16((uint16_t)(value >> 16));
    print_hex16((uint16_t)value);
}

void print_bits(uint8_t value) {
    char buffer[9];
    for (int i = 0; i < 8; i++)
        buffer[i] = ((value >> (7 - i)) & 1) ? '1' : '0';
    buffer[8] = '\0';
    print(buffer);
}

void print_bits16(uint16_t value) {
    print_bits((uint8_t)(value >> 8));
    print_bits((uint8_t)value);
}

void print_bits32(uint32_t value) {
    print_bits16((uint16_t)(value >> 16));
    print_bits16((uint16_t)value);
}

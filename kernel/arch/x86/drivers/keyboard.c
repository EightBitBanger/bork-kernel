

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>

#include <kernel/arch/x86/io.h>

#include <kernel/console/virtual_key.h>
#include <kernel/console/display.h>
#include <kernel/console/keyboard.h>
#include <kernel/console/console.h>
#include <kernel/console/print.h>
#include <kernel/util/system.h>

static volatile char current_character  = 0x00;
static volatile char last_character     = 0x00;
static volatile uint8_t isr_key_ready   = 0;

// Modifier state, left and right keys tracked separately so releasing one
// while the other is still held doesn't clear the modifier
static volatile bool lctrl_down  = false, rctrl_down  = false;
static volatile bool lalt_down   = false, ralt_down   = false;
static volatile bool lshift_down = false, rshift_down = false;

bool kb_shift_down(void) { return lshift_down || rshift_down; }
bool kb_ctrl_down(void)  { return lctrl_down  || rctrl_down;  }
bool kb_alt_down(void)   { return lalt_down   || ralt_down;   }

// Native PS/2 Scan Code Set 1 Table
static const char scancode_to_ascii_set1[] = {
    0,  0x1B, '1', '2', '3', '4', '5', '6', '7', '8',  /* 0x00 - 0x09 */
  '9', '0', '-', '=', 0x01, '\t', 'q', 'w', 'e', 'r',  /* 0x0A - 0x13 (0x01 is Backspace) */
  't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0x02,   0,   /* 0x14 - 0x1D (0x02 is Enter) */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',    /* 0x1E - 0x27 */
 '\'', '`',   0, '\\', 'z', 'x', 'c', 'v', 'b', 'n',   /* 0x28 - 0x31 */
  'm', ',', '.', '/',   0, '*',   0, ' ',   0,   0,    /* 0x32 - 0x3B */
};

static const char scancode_to_ascii_shifted_set1[] = {
    0,  0, '!', '@', '#', '$', '%', '^', '&', '*',     /* 0x00 - 0x09 */
  '(', ')', '_', '+', 0x01, '\t', 'Q', 'W', 'E', 'R',  /* 0x0A - 0x13 */
  'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0x02,   0,   /* 0x14 - 0x1D */
  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',    /* 0x1E - 0x27 */
 '"', '~',   0, '|', 'Z', 'X', 'C', 'V', 'B', 'N',   /* 0x28 - 0x31 */
  'M', '<', '>', '?',   0, '*',   0, ' ',   0,   0,    /* 0x32 - 0x3B */
};

void kb_init(void) {
    current_character = kb_getc();
    last_character = current_character;
}

void kb_isr_callback(void) {
    current_character = kb_getc();
    if (last_character == current_character) 
        return;
    last_character = current_character;
    
    isr_key_ready = 1;
}

uint8_t kb_check_input_state(void) {
    return isr_key_ready;
}

uint8_t kb_get_current_char(void) {
    return current_character;
}

void kb_flush(void) {
    last_character = 0;
    current_character = 0;
    isr_key_ready = 0;
}

void kb_clear_input_state(void) {
    isr_key_ready = 0;
}

// Console line editor: one decoded key at a time. The editing itself
// (cursor movement, insert, F3, ...) lives in console/console.c.
void kb_console_key(uint16_t key) {
    console_input_key(key);
}

// Legacy entry point: handles whatever key the IRQ decoded last
void kb_event_handler(void) {
    if (current_character == 0x00) {
        last_character = 0x00;
        isr_key_ready = 0;
        return;
    }
    isr_key_ready = 0;
    
    last_character = current_character;
    kb_console_key((uint8_t)current_character);
}

// Multi-byte sequences are decoded one byte per IRQ. An E0 (extended) or
// E1 (Pause) prefix is remembered here until the next byte comes in its own
// IRQ, instead of being waited for. QEMU has the next byte ready at once,
// but on hardware it comes about 1 ms later (a real PS/2 keyboard) or only
// after this IRQ returns (a USB keyboard behind the BIOS's PS/2 emulation,
// where every status read also traps into SMM), so spinning for it here
// would stall the machine and lose the key.
static volatile bool    kb_e0_pending = false;
static volatile uint8_t kb_e1_skip    = 0;

uint16_t kb_getc(void) {
    // Check PS/2 Controller Status Register (Port 0x64)
    // Bit 0 (0x01) is set when the Output Buffer has a scan code waiting to be read.
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) {
        return 0; // No key data available right now
    }
    
    // Check if this data came from the mouse instead of the keyboard (Bit 5 / 0x20)
    if (status & 0x20) {
        inb(0x60); // Flush the mouse byte
        return 0;
    }
    
    // Read the scan code byte from the PS/2 Data Port (Port 0x60)
    return kb_decode_scancode(inb(0x60));
}

// One Scan Code Set 1 byte, from the PS/2 port or translated from a USB
// keyboard report (usb_hid.c). Prefix bytes are remembered between calls,
// so the bytes of one key must not interleave with another source's: USB
// feeds its keys with interrupts off.
uint16_t kb_decode_scancode(uint8_t scancode) {
    // Pause sends E1 1D 45 E1 9D C5 and nothing on release. Swallow it, so
    // its 1D / 9D don't press and release Left Ctrl.
    if (kb_e1_skip > 0) {
        kb_e1_skip--;
        return 0;
    }
    if (scancode == 0xE1) {
        kb_e1_skip = 5;
        kb_e0_pending = false;
        return 0;
    }
    
    // Extended prefix: the key byte follows in the next IRQ
    if (scancode == 0xE0) {
        kb_e0_pending = true;
        return 0;
    }
    bool is_extended = kb_e0_pending;
    kb_e0_pending = false;
    
    // Determine if this is a Make code (press) or Break code (release)
    // In Set 1, a break code has bit 7 (0x80) enabled.
    bool is_break = (scancode & 0x80) ? true : false;
    scancode &= 0x7F; // Strip bit 7 to isolate the core scancode identifier
    
    // Fake shifts (E0 2A / E0 AA / E0 36 / E0 B6) wrap the gray navigation
    // keys when NumLock is on. Holding Shift and pressing an arrow sends a
    // fake Shift *release* first, which would clear the real Shift state
    // right before the arrow is processed. They are not real key events.
    if (is_extended && (scancode == 0x2A || scancode == 0x36))
        return 0;
    
    // Modifier state tracking
    switch (scancode) {
        case 0x1D: if (is_extended) rctrl_down = !is_break; else lctrl_down = !is_break; break;
        case 0x38: if (is_extended) ralt_down  = !is_break; else lalt_down  = !is_break; break;
        case 0x2A: lshift_down = !is_break; break;
        case 0x36: rshift_down = !is_break; break;
    }
    
    // Handle Ctrl+Alt+Del Reset Sequence
    if (scancode == 0x53 && !is_break && kb_ctrl_down() && kb_alt_down()) {
        system_restart();
    }
    
    // Forward the key state updates down to virtual tracking subsystems
    kb_vkey_set(scancode, !is_break);
    
    if (is_break) {
        current_character = 0x00;
        // The last Alt key let go: reported (SYS_KEY_ALT_UP, Alt's scancode
        // << 8 like the other keys), so the desktop can end an Alt+Tab
        if (scancode == 0x38 && !kb_alt_down())
            return (uint16_t)(0x38 << 8);
        return 0;
    }
    
    if (is_extended) {
        switch (scancode) {
            case 0x47: case 0x48: case 0x49:   // Home, Up, PgUp
            case 0x4B: case 0x4D:              // Left, Right
            case 0x4F: case 0x50: case 0x51:   // End, Down, PgDn
            case 0x53:                         // Delete
                return (uint16_t)(scancode << 8);
        }
        return 0;
    }
    
    // Function keys, and the keypad's navigation keys (there is no NumLock
    // handling, so the keypad always navigates): same codes as the gray keys
    switch (scancode) {
        case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:   // F1 - F5
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44:   // F6 - F10
        case 0x57: case 0x58:                                    // F11, F12
        case 0x47: case 0x48: case 0x49:                         // Home, Up, PgUp
        case 0x4B: case 0x4D:                                    // Left, Right
        case 0x4F: case 0x50: case 0x51:                         // End, Down, PgDn
        case 0x53:                                               // Delete
            current_character = 0x00;
            return (uint16_t)(scancode << 8);
    }
    
    if (scancode < sizeof(scancode_to_ascii_set1)) {
        current_character = kb_shift_down() ? scancode_to_ascii_shifted_set1[scancode]
                                            : scancode_to_ascii_set1[scancode];
    } else {
        // Keypad / function keys: no character
        current_character = 0x00;
    }
    
    return (uint16_t)(uint8_t)current_character;
}

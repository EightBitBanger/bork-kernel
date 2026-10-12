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

extern char* keyboard_string;
extern uint8_t keyboard_length;
extern uint8_t keyboard_length_max;

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

// Console line editor: one decoded key at a time
void kb_console_key(uint16_t key) {
    // Extended keys (arrows, Home, ...) arrive as scancode << 8; the console
    // has no use for them yet
    if (key == 0 || key > 0xFF)
        return;
    
    char ch = (char)key;
    
    // Backspace. The input buffer always loses the character, even when
    // its cell has scrolled off the top and can no longer be erased (it used
    // to keep it, so the buffer and the screen disagreed).
    if (ch == 0x01) {
        if (keyboard_length == 0) return;
        
        keyboard_length--;
        keyboard_string[keyboard_length] = '\0';
        
        // print_backspace() also handles the character in the last column,
        // where the cursor has not advanced past it
        print_backspace();
        return;
    }
    
    // Enter
    if (ch == 0x02) {
        print("\n");
        if (keyboard_length > 0) {
            console_process_command(keyboard_string);
            if (display_cursor_get_position() != 0) print("\n");
        }
        keyboard_length = 0;
        keyboard_string[0] = '\0';
        console_prompt_print();
        return;
    }
    
    // Print printable characters
    if (ch < 0x20 || ch == 0x7f) 
        return;
    
    if (keyboard_length < keyboard_length_max - 1) { 
        keyboard_string[keyboard_length] = ch;
        keyboard_length++;
        keyboard_string[keyboard_length] = '\0';
        
        char input[2] = {ch, '\0'};
        print(input);
    }
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
    
    // Read the primary scan code byte from the PS/2 Data Port (Port 0x60)
    uint8_t scancode = inb(0x60);
    bool is_extended = false;
    
    // Handle extended scan codes (0xE0 prefix)
    if (scancode == 0xE0) {
        // Spin briefly until the second byte of the sequence lands in the buffer
        int timeout = 20000;
        while (!((inb(0x64) & 0x01)) && timeout > 0) {
            timeout--;
        }
        
        // If the second byte arrived, grab it; otherwise drop the trailing prefix safely
        if (timeout > 0) {
            scancode = inb(0x60);
            is_extended = true;
        } else {
            return 0; 
        }
    }
    
    // Determine if this is a Make code (press) or Break code (release)
    // In Set 1, a break code has bit 7 (0x80) enabled.
    bool is_break = (scancode & 0x80) ? true : false;
    scancode &= 0x7F; // Strip bit 7 to isolate the core scancode identifier
    
    // Fake shifts (E0 2A / E0 AA / E0 36 / E0 B6) wrap the gray navigation
    // keys when NumLock is on. Holding Shift and pressing an arrow sends a
    // fake Shift *release* first, which used to clear the real Shift state
    // right before the arrow was processed. They are not real key events.
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
    
    if (scancode < sizeof(scancode_to_ascii_set1)) {
        current_character = kb_shift_down() ? scancode_to_ascii_shifted_set1[scancode]
                                            : scancode_to_ascii_set1[scancode];
    } else {
        // Keypad / function keys: no character (previously returned a stale one)
        current_character = 0x00;
    }
    
    return (uint16_t)(uint8_t)current_character;
}


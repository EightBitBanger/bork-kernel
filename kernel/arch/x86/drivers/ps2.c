
#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/drivers/display/draw.h>
#include <kernel/arch/x86/drivers/keyboard.h>
#include <kernel/arch/x86/drivers/ps2.h>
#include <kernel/boot/x86/interrupt.h>
#include <kernel/console/keyboard.h>
#include <kernel/console/mouse.h>
#include <kernel/ipc/ipc.h>

// Longest the console sleeps with no input before looking again. Only a
// backstop: every keyboard and mouse IRQ signals input_event.
#define PS2_CONSOLE_IDLE_MS  50

bool ps2_check_keyboard(void) {
    uint8_t status = inb(PS2_STATUS_REG);
    if (status & PS2_STATUS_OUTPUT_FULL) {
        if (status & PS2_STATUS_MOUSE_DATA) {
            mouse_event_handler();
        } else {
            return true; // Keyboard data is ready
        }
    }
    return false;
}

// One pass of the boot console.
//
// The keyboard IRQ owns port 0x60: it reads every scancode, decodes it and
// queues the key (see keyboard_handler_c). This used to poll the port as
// well, racing the IRQ for the same byte. The IRQ nearly always won, so a
// key only registered when the poll happened to slip in first, which is why
// typing felt so slow. Now the console sleeps until the IRQ signals input,
// then drains the queue. Keys typed while a command runs are kept, too.
void ps2_route_console(void) {
    event_wait(&input_event, PS2_CONSOLE_IDLE_MS);

    // print() flushes each character cell as it draws, so no full-screen
    // flush is needed per key any more (that was ~5.7 MB to VRAM per key)
    // A process holding the input grab gets the keys instead
    if (ipc_input_grabbed())
        return;

    uint16_t key;
    while (input_key_pop(&key))
        kb_console_key(key);
}

void flush_keyboard_buffer(void) {
    while (inb(PS2_STATUS_REG) & PS2_STATUS_OUTPUT_FULL) {
        inb(PS2_DATA_REG);
    }
}


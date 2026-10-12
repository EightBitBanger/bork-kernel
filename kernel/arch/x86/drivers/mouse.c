#include <ctype.h>
#include <stdint.h>
#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/boot/x86/interrupt.h>
#include <kernel/console/display.h>
#include <kernel/console/mouse.h>
#include <kernel/util/timer.h>

static uint8_t mouse_packet[3];

bool mouse_left_clicked   = false;
bool mouse_right_clicked  = false;
bool mouse_middle_clicked = false;

int32_t fixed_mouse_x = 0;
int32_t fixed_mouse_y = 0;

int32_t horizontal_precision = 32;
int32_t vertical_precision   = 32;

int32_t horizontal_mul = 3;
int32_t vertical_mul   = 3;

int32_t acceleration_max = 2;

uint8_t mouse_cycle = 0;

int32_t mouse_x = 0;
int32_t mouse_y = 0;

static MouseEvent mouse_queue[MOUSE_QUEUE_SIZE];
static volatile uint32_t mouse_queue_head = 0; // Written by the IRQ
static volatile uint32_t mouse_queue_tail = 0; // Read by the DWM

Point mouse_get_position(void) {
    return (Point){mouse_x, mouse_y};
}

bool mouse_get_button(uint8_t button) {
    switch(button) {
        case MOUSE_BUTTON_LEFT:   return mouse_left_clicked;
        case MOUSE_BUTTON_MIDDLE: return mouse_middle_clicked;
        case MOUSE_BUTTON_RIGHT:  return mouse_right_clicked;
    }
    return false;
}

void mouse_set_position(uint32_t x, uint32_t y) {
    mouse_x = x;
    mouse_y = y;
    fixed_mouse_x = (int32_t)x << 8;
    fixed_mouse_y = (int32_t)y << 8;
}

void mouse_set_cursor_speed(int32_t horz, int32_t vert) {
    horizontal_mul = horz;
    vertical_mul   = vert;
}

void mouse_set_cursor_acceleration(int32_t acceleration) {
    acceleration_max = acceleration;
}

void mouse_wait(uint8_t type) {
    if (type == 0) {
        while ((inb(0x64) & 1) == 0); // Wait for data to be readable
    } else {
        while ((inb(0x64) & 2) != 0); // Wait for buffer to clear for writing
    }
}

void mouse_write(uint8_t write) {
    mouse_wait(1);
    outb(0x64, 0xD4); // Signal: Sending command directly to mouse
    mouse_wait(1);
    outb(0x60, write);
}

uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(0x60);
}

void mouse_initiate(void) {
    uint8_t status;
    
    mouse_wait(1);
    outb(0x64, 0xA8); 
    
    mouse_wait(1);
    outb(0x64, 0x20); // Command: Read Command Byte
    mouse_wait(0);
    status = inb(0x60);
    
    status |=  (1 << 0); // Bit 0 (Keyboard Interrupt / IRQ 1)
    status |=  (1 << 1); // Bit 1 (Mouse Interrupt / IRQ 12)
    status |=  (1 << 5); // Ensure bit 5 is set
    
    // Write the modified Configuration Byte back
    mouse_wait(1);
    outb(0x64, 0x60); // Command: Write Command Byte
    mouse_wait(1);
    outb(0x60, status);
    
    // Tell the mouse hardware to use default configuration
    mouse_write(0xF6);
    mouse_read(); // Clear the Acknowledge byte (0xFA)
    
    // Tell the mouse hardware to start streaming data packets
    mouse_write(0xF4);
    mouse_read(); // Clear the Acknowledge byte (0xFA)
    
    // Flush any leftover ACK/data bytes before enabling IRQs
    while (inb(0x64) & 1) {
        inb(0x60);
    }
}

void mouse_enqueue_event(int32_t x, int32_t y, bool left, bool right, bool middle) {
    uint32_t next_head = (mouse_queue_head + 1) % MOUSE_QUEUE_SIZE;

    // Queue full. The producer (this IRQ) must never write tail: the DWM
    // thread may be halfway through copying that slot in mouse_dequeue_event(),
    // and moving tail under it loses or tears an event. Instead, fold the new
    // state into the newest queued event so position and buttons stay current.
    if (next_head == mouse_queue_tail) {
        uint32_t newest = (mouse_queue_head + MOUSE_QUEUE_SIZE - 1) % MOUSE_QUEUE_SIZE;
        if (newest != mouse_queue_tail) {
            // Safe: the consumer only reads the slot at tail
            mouse_queue[newest].x = x;
            mouse_queue[newest].y = y;
            mouse_queue[newest].left_button = left;
            mouse_queue[newest].right_button = right;
            mouse_queue[newest].middle_button = middle;
            mouse_queue[newest].timestamp = timer_get_ms();
        }
        return;
    }

    mouse_queue[mouse_queue_head].x = x;
    mouse_queue[mouse_queue_head].y = y;
    mouse_queue[mouse_queue_head].left_button = left;
    mouse_queue[mouse_queue_head].right_button = right;
    mouse_queue[mouse_queue_head].middle_button = middle;
    mouse_queue[mouse_queue_head].timestamp = timer_get_ms();
    
    // Update head last so the consumer doesn't read incomplete data
    __asm__ volatile("" ::: "memory");
    mouse_queue_head = next_head;
}

bool mouse_dequeue_event(MouseEvent* out_event) {
    // If head == tail, the buffer is empty
    if (mouse_queue_head == mouse_queue_tail) {
        return false; 
    }
    
    // Copy with the IRQ masked: when the queue is full the producer coalesces
    // into the newest slot, which is the tail slot when only one is queued.
    uint32_t flags = irq_save();
    *out_event = mouse_queue[mouse_queue_tail];
    mouse_queue_tail = (mouse_queue_tail + 1) % MOUSE_QUEUE_SIZE;
    irq_restore(flags);
    
    return true;
}

void mouse_event_handler(void) {
    uint8_t status = inb(0x64);
    
    // Only process if data is ready (Bit 0) AND originated from the mouse (Bit 5)
    if (!(status & 0x01) || !(status & 0x20)) {
        return;
    }
    
    uint8_t input_byte = inb(0x60);
    
    // Stream Realignment Check:
    // Byte 0 MUST have bit 3 (0x08) set to 1.
    // If we are looking for Byte 0 (cycle 0) and bit 3 is 0, this is out-of-sync data. Discard it!
    if (mouse_cycle == 0 && !(input_byte & 0x08)) {
        return;
    }
    
    mouse_packet[mouse_cycle] = input_byte;
    mouse_cycle++;
    
    // Process complete 3-byte packet
    if (mouse_cycle == 3) {
        mouse_cycle = 0; // Reset state machine for next packet
        
        // Ignore invalid overflow packets (Bit 6 = X overflow, Bit 7 = Y overflow)
        if (mouse_packet[0] & 0xC0) {
            return;
        }
        
        // Extract button states
        mouse_left_clicked   = (mouse_packet[0] & 0x01) ? true : false;
        mouse_right_clicked  = (mouse_packet[0] & 0x02) ? true : false;
        mouse_middle_clicked = (mouse_packet[0] & 0x04) ? true : false;
        
        // Parse relative byte values
        int16_t rel_x = (uint8_t)mouse_packet[1];
        int16_t rel_y = (uint8_t)mouse_packet[2];
        
        // Apply 16-bit two's complement sign extension based on Byte 0 sign flags
        if (mouse_packet[0] & 0x10) rel_x |= 0xFF00; // Bit 4 = X Sign
        if (mouse_packet[0] & 0x20) rel_y |= 0xFF00; // Bit 5 = Y Sign
        
        // Update coordinates smoothly (1 pixel = 1 << 8 = 256 sub-pixel units)
        // Multiplier scaling factor for speed tuning (1 = standard 1:1 mouse speed)
        int32_t speed = 1; 
        fixed_mouse_x += ((int32_t)rel_x * speed) << 8;
        fixed_mouse_y -= ((int32_t)rel_y * speed) << 8; // Y axis is inverted in screen space
        
        // Screen boundary clamping
        int32_t max_x = ((int32_t)display_get_width() << 8) - 1;
        int32_t max_y = ((int32_t)display_get_height() << 8) - 1;
        
        if (fixed_mouse_x < 0) fixed_mouse_x = 0;
        if (fixed_mouse_y < 0) fixed_mouse_y = 0;
        if (fixed_mouse_x > max_x) fixed_mouse_x = max_x;
        if (fixed_mouse_y > max_y) fixed_mouse_y = max_y;
        
        mouse_x = fixed_mouse_x >> 8;
        mouse_y = fixed_mouse_y >> 8;
        
        mouse_enqueue_event(mouse_x, mouse_y, mouse_left_clicked, mouse_right_clicked, mouse_middle_clicked);
    }
}

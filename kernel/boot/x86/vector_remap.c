#include <stdint.h>
#include <kernel/arch/x86/io.h>
#include <kernel/boot/x86/interrupt.h>

#define PIC1          0x20    /* IO base address for master PIC */
#define PIC2          0xA0    /* IO base address for slave PIC */
#define PIC1_COMMAND  PIC1
#define PIC1_DATA     (PIC1+1)
#define PIC2_COMMAND  PIC2
#define PIC2_DATA     (PIC2+1)

#define ICW1_INIT     0x11    /* Initialization command bit */
#define ICW4_8086     0x01    /* 8086/88 (MCS-80/85) mode */

void pic_remap(void) {
    uint8_t a1, a2;
    
    // Get existing interrupt masks
    a1 = inb(PIC1_DATA);
    a2 = inb(PIC2_DATA);
    
    // Begin initialization sequence (in cascade mode)
    outb(PIC1_COMMAND, ICW1_INIT);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT);
    io_wait();
    
    // Set the Vector Offsets (Where they map to in the IDT)
    outb(PIC1_DATA, 0x20); // Master PIC vectors out to 32 (0x20)
    io_wait();
    outb(PIC2_DATA, 0x28); // Slave PIC vectors out to 40 (0x28)
    io_wait();
    
    // Tell Master PIC that there is a slave PIC at IRQ2 (00000100b)
    outb(PIC1_DATA, 4);
    io_wait();
    // Tell Slave PIC its cascade identity (00000010b)
    outb(PIC2_DATA, 2);
    io_wait();
    
    // Set the PICs into 8086 mode
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();
    
    // Restore masks (or clear them to 0x00 to enable all lines)
    outb(PIC1_DATA, 0x00); 
    outb(PIC2_DATA, 0x00);
}

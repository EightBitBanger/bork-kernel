#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>

#include <kernel/arch/x86/io.h>


void system_restart(void) {
    
    outb(0xCF9, 0x06);
}


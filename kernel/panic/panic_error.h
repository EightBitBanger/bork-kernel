#ifndef KERNEL_PANIC_HANDLER_H
#define KERNEL_PANIC_HANDLER_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/drivers/display/draw.h>
#include <kernel/console/display.h>
#include <kernel/console/print.h>

// Panic fault

#define PF_PRESENT                     0x01  // 0: Super non-present page,   1: Page-protection violation
#define PF_WRITE                       0x02  // 0: Read access,              1: Write access
#define PF_USER                        0x04  // 0: Kernel-mode fault,        1: User-mode fault
#define PF_RESERVED                    0x08  // 1: Overwritten reserved bit in the page table
#define PF_INSTRUCT                    0x10  // 1: Fault occurred during an instruction fetch

// Panic type

#define PT_GENERAL_PROTECTION_FAULT    0x00
#define PT_PAGE_FAULT                  0x01
#define PT_SEG_FAULT                   0x02
#define PT_OUT_OF_MEMORY               0x03
#define PT_CPU_EXCEPTION               0x04  // faulting_address = EIP, extra = exception name
#define PT_WATCHDOG                    0x05  // extra = preformatted multi-line report (drawn at 1x)

// Set by kernel_crashout before it draws (the NMI watchdog leaves the screen alone)
extern volatile bool kernel_panicking;

void kernel_crashout(uint32_t error_code, uint32_t faulting_address, uint8_t type, const char* extra);

#endif


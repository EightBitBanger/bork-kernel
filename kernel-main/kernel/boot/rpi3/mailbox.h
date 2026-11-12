#ifndef _KERNEL_BOOT_RPI3_MAILBOX_H_
#define _KERNEL_BOOT_RPI3_MAILBOX_H_

#include <stdint.h>
#include <stdbool.h>

// VideoCore property mailbox (channel 8)
//
// The GPU firmware owns the clocks, the framebuffer and the memory split, so
// the kernel asks it: this is where the Pi gets what x86 got from the
// Multiboot info block (memory size, framebuffer address and pitch).
//
// `buffer` must be 16-byte aligned and in memory the GPU can see (anything
// below 1 GB). It holds: total size, request code, tags..., end tag (0).
// Returns true when the firmware answered with "success".
bool mailbox_call(volatile uint32_t* buffer);

// Convenience wrappers over single tags. False if the firmware refused.
bool mailbox_get_board_revision(uint32_t* revision);
bool mailbox_get_firmware_revision(uint32_t* revision);
bool mailbox_get_arm_memory(uint32_t* base, uint32_t* size);

#endif

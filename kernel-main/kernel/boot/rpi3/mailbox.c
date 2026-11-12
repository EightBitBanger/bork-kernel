// VideoCore property mailbox on a Raspberry Pi 3

#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/aarch64/mmio.h>
#include <kernel/boot/rpi3/board.h>
#include <kernel/boot/rpi3/mailbox.h>

#define MBOX_READ       (MAILBOX_BASE + 0x00)
#define MBOX_STATUS     (MAILBOX_BASE + 0x18)
#define MBOX_WRITE      (MAILBOX_BASE + 0x20)

#define MBOX_FULL       0x80000000u
#define MBOX_EMPTY      0x40000000u

#define MBOX_CH_PROP    8u

#define MBOX_REQUEST    0x00000000u
#define MBOX_SUCCESS    0x80000000u

#define TAG_FIRMWARE_REVISION   0x00000001u
#define TAG_BOARD_REVISION      0x00010002u
#define TAG_ARM_MEMORY          0x00010005u

// One buffer for the wrappers below. Single core, no threads yet.
static volatile uint32_t mbox[36] __attribute__((aligned(16)));

bool mailbox_call(volatile uint32_t* buffer) {
    // The low 4 bits of the message carry the channel, hence the alignment
    uint32_t message = (uint32_t)((uintptr_t)buffer & ~0xFu) | MBOX_CH_PROP;

    while (mmio_read32(MBOX_STATUS) & MBOX_FULL) {}
    mmio_write32(MBOX_WRITE, message);

    for (;;) {
        while (mmio_read32(MBOX_STATUS) & MBOX_EMPTY) {}
        if (mmio_read32(MBOX_READ) == message)
            return buffer[1] == MBOX_SUCCESS;
    }
}

// Ask one tag whose answer is `words` 32-bit values, and copy them to `out`
static bool mailbox_get_tag(uint32_t tag, uint32_t words, uint32_t* out) {
    uint32_t i = 0;
    mbox[i++] = 0;                      // Total size, filled in below
    mbox[i++] = MBOX_REQUEST;
    mbox[i++] = tag;
    mbox[i++] = words * 4;              // Value buffer size
    mbox[i++] = 0;                      // Request
    for (uint32_t w = 0; w < words; w++)
        mbox[i++] = 0;
    mbox[i++] = 0;                      // End tag
    mbox[0] = i * 4;

    if (!mailbox_call(mbox))
        return false;
    for (uint32_t w = 0; w < words; w++)
        out[w] = mbox[5 + w];
    return true;
}

bool mailbox_get_board_revision(uint32_t* revision) {
    return mailbox_get_tag(TAG_BOARD_REVISION, 1, revision);
}

bool mailbox_get_firmware_revision(uint32_t* revision) {
    return mailbox_get_tag(TAG_FIRMWARE_REVISION, 1, revision);
}

bool mailbox_get_arm_memory(uint32_t* base, uint32_t* size) {
    uint32_t answer[2];
    if (!mailbox_get_tag(TAG_ARM_MEMORY, 2, answer))
        return false;
    *base = answer[0];
    *size = answer[1];
    return true;
}

#include <stdint.h>

#include <kernel/user/user_test.h>
#include <kernel/scheduler/process.h>
#include <kernel/arch/x86/drivers/serial.h>

// Defined in user_test.asm
extern const uint8_t user_hello_start[];
extern const uint8_t user_hello_end[];
extern const uint8_t user_crash_start[];
extern const uint8_t user_crash_end[];

static void launch(const char* name, const uint8_t* start, const uint8_t* end) {
    ProcessImage image = {
        .data         = start,
        .size         = (uint32_t)(end - start),
        .memory_size  = 0,      // No .bss
        .entry_offset = 0,
    };

    ProcessId pid = CreateProcessFromImage(name, &image, PRIORITY_NORMAL);
    if (pid <= 0) {
        serial_puts("[user_test] could not start ");
        serial_puts(name);
        serial_puts(", error ");
        serial_put_int(pid);
        serial_puts("\n");
    }
}

void user_test_launch(void) {
    serial_puts("[user_test] starting ring 3 test programs\n");
    launch("hello", user_hello_start, user_hello_end);
    launch("crash", user_crash_start, user_crash_end);
}

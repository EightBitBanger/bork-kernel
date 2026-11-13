#ifndef KERNEL_UTIL_SYSTEM_H
#define KERNEL_UTIL_SYSTEM_H

// Reset the machine: the ACPI reset register, the chipset's reset control
// (0xCF9), the keyboard controller, a triple fault. Never returns.
// Ctrl+Alt+Del and SYS_POWER both come here; neither writes anything out
// first (SYS_POWER syncs before it calls this).
void system_restart(void);

// Turn the machine off (ACPI S5). Interrupts are off when it returns, which
// it does only if the machine is still on: the caller says so and halts.
void system_poweroff(void);

#endif

#ifndef _KERNEL_BOOT_RPI3_BOARD_H_
#define _KERNEL_BOOT_RPI3_BOARD_H_

// Raspberry Pi 3 Model B+ (BCM2837B0, 4x Cortex-A53, 1 GB)
//
// Physical addresses as the ARM cores see them. The same SoC layout covers
// the Pi 3B, 3A+ and Zero 2 W; a Pi 4 (BCM2711) moves everything (see
// boot/rpi4/ for notes on that board).

#define BOARD_NAME              "Raspberry Pi 3 Model B+"

// Where the firmware loads kernel8.img (config.txt kernel_address)
#define BOARD_KERNEL_BASE       0x80000

// VideoCore peripherals (the "0x7E......" bus addresses in the datasheet)
#define PERIPHERAL_BASE         0x3F000000UL

// System timer, free-running 1 MHz (also has 4 compare channels)
#define SYSTIMER_BASE           (PERIPHERAL_BASE + 0x003000)

// Legacy ARM interrupt controller: GPU and peripheral IRQs (no GIC on a Pi 3)
#define IRQ_BASE                (PERIPHERAL_BASE + 0x00B200)

// VideoCore mailbox 0 (ARM <- VC) / 1 (ARM -> VC)
#define MAILBOX_BASE            (PERIPHERAL_BASE + 0x00B880)

// Power management, watchdog (used for reboot)
#define PM_BASE                 (PERIPHERAL_BASE + 0x100000)

// Hardware random number generator
#define RNG_BASE                (PERIPHERAL_BASE + 0x104000)

#define GPIO_BASE               (PERIPHERAL_BASE + 0x200000)

// PL011 UART0. On the 3B+ it drives Bluetooth unless config.txt says
// dtoverlay=disable-bt, which puts it on GPIO 14 (TX) / 15 (RX)
#define UART0_BASE              (PERIPHERAL_BASE + 0x201000)

// SD card (Arasan SDHCI; "EMMC") and the DWC2 USB OTG controller, behind
// which sit the LAN7515 hub, Ethernet and the four USB ports
#define EMMC_BASE               (PERIPHERAL_BASE + 0x300000)
#define USB_DWC2_BASE           (PERIPHERAL_BASE + 0x980000)

// ARM local peripherals (BCM2836 "QA7"): core timers, per-core IRQ routing,
// mailboxes between the four cores. Not a GIC: the Pi 3 has none.
#define LOCAL_PERIPHERAL_BASE   0x40000000UL

#endif

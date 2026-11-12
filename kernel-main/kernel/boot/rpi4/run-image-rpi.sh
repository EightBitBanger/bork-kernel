#!/bin/bash
# Run the Pi build under QEMU. raspi4b exists in QEMU 9.0+; raspi3b is a
# reasonable fallback (different peripheral base 0x3F000000, no GIC, no PCIe).

qemu-system-aarch64 \
    -M raspi4b \
    -kernel bin/kernel8.img \
    -dtb bcm2711-rpi-4-b.dtb \
    -serial stdio \
    -usb -device usb-kbd -device usb-mouse

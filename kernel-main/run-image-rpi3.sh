#!/bin/bash
#
# Boot the Raspberry Pi 3 build under QEMU. The PL011 serial console is on
# stdio (Ctrl-A X quits). Build it first:
#
#   cmake -S . -B build-rpi3 -DKERNEL_TARGET=rpi3 && cmake --build build-rpi3
#
# QEMU's raspi3b is a Pi 3B: same SoC family as the 3B+, so the same kernel.
set -e

KERNEL_IMG="bin/kernel8.img"

if [ ! -f "$KERNEL_IMG" ]; then
    echo "Error: $KERNEL_IMG not found. Build with KERNEL_TARGET=rpi3 first." >&2
    exit 1
fi

qemu-system-aarch64 \
    -M raspi3b \
    -kernel "$KERNEL_IMG" \
    -serial stdio \
    -display none \
    "$@"

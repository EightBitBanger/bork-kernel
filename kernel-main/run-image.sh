#!/bin/bash
#
# Initialize test storage devices and run the kernel in QEMU.
#
#   ./run-image.sh         boot from a virtual USB stick (requires build_kernel.sh first)
#   ./run-image.sh disk    boot from disk.img with no stick attached
#
set -e

MODE="${1:-usb}"

NVME_NAME="../nvme_disk.img"
NVME_SIZE="128M"

STICK_NAME="boot-disk.img"

# Create a raw disk image if it doesn't already exist
create_disk() {
    local name="$1" size="$2"
    if [ ! -f "$name" ]; then
        echo "Initializing storage device: $name ($size)..."
        qemu-img create -f raw "$name" "$size"
    fi
}

# 1. Initiate test storage devices
create_disk "$NVME_NAME" "$NVME_SIZE"    # NVMe namespace 1     -> nvme0

STICK_ARGS=()
if [ "$MODE" = "usb" ]; then
    if [ ! -f "$STICK_NAME" ]; then
        echo "Error: $STICK_NAME not found. Please run ./build_kernel.sh first." >&2
        exit 1
    fi

    # USB mass storage on an EHCI controller, first in the boot order
    STICK_ARGS=(
        -device usb-ehci,id=ehci
        -drive file="$STICK_NAME",if=none,id=stick,format=raw
        -device usb-storage,bus=ehci.0,drive=stick,bootindex=0
    )
elif [ "$MODE" != "disk" ]; then
    echo "usage: $0 [usb|disk]" >&2
    exit 1
fi

# 2. Launch QEMU with the initialized storage
echo "Launching QEMU in '$MODE' mode..."
qemu-system-i386 \
    -m 4G \
    -display gtk,zoom-to-fit=on,full-screen=on \
    -vga std \
    -rtc base=localtime \
    -drive file="$NVME_NAME",if=none,id=nvmedisk,format=raw \
    -device nvme,serial=nvme0,drive=nvmedisk \
    "${STICK_ARGS[@]}"

#!/bin/bash
#
# Build the bootable kernel/USB image from the kernel binary that is
# already built (Code::Blocks post-build step). It does not compile
# anything: building the kernel is Code::Blocks' job, or build.sh's for a
# command-line build.
#
set -e

cd "$(dirname "$0")"

STICK_NAME="boot-disk.img"
KERNEL_BIN="bin/x86-kernel.bin"
ASSETS="bin/assets.tar"

echo "Checking for kernel binary..."
if [ ! -f "$KERNEL_BIN" ]; then
    echo "Error: $KERNEL_BIN not found. Please compile your kernel first." >&2
    exit 1
fi

echo "Building bootable USB stick ($STICK_NAME)..."
# Fresh stick every run: MBR + stage 2 + kernel + asset archive, 64 MB.
# The archive is made by the pre-build step (compile-asset-tar.sh); without
# one the stick still boots, but boot install / repair have no system files.
if [ -f "$ASSETS" ]; then
    ./build-bootloader.sh "$KERNEL_BIN" "$STICK_NAME" "$ASSETS"
else
    echo "Warning: $ASSETS not found, building the stick without it." >&2
    ./build-bootloader.sh "$KERNEL_BIN" "$STICK_NAME" ""
fi

echo "Kernel build complete: $STICK_NAME is ready."

#!/usr/bin/env bash
#
# Build a raw, BIOS-bootable hard disk image
#
#   ./build-bootloader.sh [kernel] [output] [assets]
#   ./build-bootloader.sh bin/x86-kernel.bin boot-disk.img bin/assets.tar
#
# Disk layout (512-byte sectors):
#
#   LBA 0                 stage 1 (MBR) + partition table
#   LBA 1 .. 62           stage 2 (as many sectors as it needs)
#   LBA 64 ..             kernel ELF file, raw
#   right after it        asset archive (ustar, build-assets.sh), if any
#   LBA RESERVED ..       partition 1 (type 0xDA, active) to the end of disk:
#                         the area the kernel's own filesystem may use
#
# Stage 2 loads everything in front of the partition that is used (MBR to
# the end of the archive) and hands it to the kernel, the archive as a
# module of its own. That is how the archive gets into memory from a USB
# stick: the BIOS reads it, the kernel never has to.
#
# The archive's length is in stage 2's header (stage2.asm), not only in its
# code, so "boot install" can write stage 2 with no archive: an installed
# disk gets the loader and kernel, the archive stays on the stick.
#
# [assets] defaults to bin/assets.tar when that exists; pass "" (or
# ASSETS=) for an image without one.
#
# Environment:
#   DISK_MB                image size in MB            (default 64)
#   BOOT_RESERVED_SECTORS  sectors kept for the loader (default 32768 = 16 MB,
#                          VFS_PARTITION_START_BOOT in kernel/vfs/vfs.h);
#                          fixed so the data area does not move when the
#                          kernel grows
#   BUILD_DIR              where the loader binaries go (default bin/loader)
#
# Run it:  qemu-system-i386 -drive file=boot-disk.img,format=raw -vga std
#
set -euo pipefail

KERNEL=${1:-bin/x86-kernel.bin}
OUT=${2:-boot-disk.img}
if [ $# -ge 3 ]; then
    ASSETS=$3
elif [ -z "${ASSETS+set}" ]; then
    ASSETS=bin/assets.tar
    [ -f "$ASSETS" ] || ASSETS=
fi
DISK_MB=${DISK_MB:-64}
RESERVED=${BOOT_RESERVED_SECTORS:-32768}
BUILD_DIR=${BUILD_DIR:-bin/loader}
LOADER_SRC=${LOADER_SRC:-$(dirname "$0")/kernel/boot/x86/loader}

KERNEL_LBA=64

die() { echo "build-disk: $*" >&2; exit 1; }

[ -f "$KERNEL" ] || die "kernel not found: $KERNEL"
command -v nasm >/dev/null || die "nasm is required"

# GRUB loaded this kernel as ELF (its Multiboot header has no a.out fields)
if [ "$(head -c 4 "$KERNEL" | od -An -tx1 | tr -d ' \n')" != "7f454c46" ]; then
    echo "build-disk: warning: $KERNEL is not ELF; stage 2 will need the a.out kludge (header flag bit 16)" >&2
fi

kernel_bytes=$(stat -c %s "$KERNEL")
kernel_sectors=$(( (kernel_bytes + 511) / 512 ))

assets_lba=$(( KERNEL_LBA + kernel_sectors ))
assets_bytes=0
if [ -n "$ASSETS" ]; then
    [ -f "$ASSETS" ] || die "asset archive not found: $ASSETS"
    [ "$(head -c 262 "$ASSETS" | tail -c 5)" = "ustar" ] || die "$ASSETS is not a ustar archive"
    assets_bytes=$(stat -c %s "$ASSETS")
fi
assets_sectors=$(( (assets_bytes + 511) / 512 ))

(( assets_lba + assets_sectors <= RESERVED )) || \
    die "kernel and assets need $(( kernel_sectors + assets_sectors )) sectors, only $(( RESERVED - KERNEL_LBA )) reserved (raise BOOT_RESERVED_SECTORS)"

total_sectors=$(( DISK_MB * 2048 ))
(( total_sectors > RESERVED )) || die "DISK_MB too small for the reserved boot area"

mkdir -p "$BUILD_DIR"

nasm -f bin -DKERNEL_LBA=$KERNEL_LBA -DKERNEL_SECTORS=$kernel_sectors -DASSETS_SECTORS=$assets_sectors \
     "$LOADER_SRC/stage2.asm" -o "$BUILD_DIR/stage2.bin"
stage2_sectors=$(( ($(stat -c %s "$BUILD_DIR/stage2.bin") + 511) / 512 ))
(( 1 + stage2_sectors <= KERNEL_LBA )) || die "stage 2 overlaps the kernel"

nasm -f bin -DSTAGE2_SECTORS=$stage2_sectors \
     "$LOADER_SRC/stage1.asm" -o "$BUILD_DIR/stage1.bin"

# Blank image, then the pieces
rm -f "$OUT"
truncate -s $(( total_sectors * 512 )) "$OUT"
dd if="$BUILD_DIR/stage1.bin" of="$OUT" bs=512 seek=0           conv=notrunc status=none
dd if="$BUILD_DIR/stage2.bin" of="$OUT" bs=512 seek=1           conv=notrunc status=none
dd if="$KERNEL"               of="$OUT" bs=512 seek=$KERNEL_LBA conv=notrunc status=none
if (( assets_sectors > 0 )); then
    dd if="$ASSETS"           of="$OUT" bs=512 seek=$assets_lba conv=notrunc status=none
fi

# Little-endian 32-bit value as raw bytes
le32() {
    printf "\\x$(printf %02x $(( $1 & 255 )))\\x$(printf %02x $(( ($1 >> 8) & 255 )))"
    printf "\\x$(printf %02x $(( ($1 >> 16) & 255 )))\\x$(printf %02x $(( ($1 >> 24) & 255 )))"
}

# Disk signature (some firmware and OSes like a non-zero one)
dd if=/dev/urandom of="$OUT" bs=1 count=4 seek=$(( 0x1B8 )) conv=notrunc status=none

# Partition entry 1 at 0x1BE: active, LBA-only CHS markers, type 0xDA.
# Some BIOSes refuse to boot a disk whose MBR has no active partition.
{
    printf '\x80\xFE\xFF\xFF'           # status (active), CHS start
    printf '\xDA\xFE\xFF\xFF'           # type, CHS end
    le32 "$RESERVED"                    # first LBA
    le32 $(( total_sectors - RESERVED )) # sector count
} | dd of="$OUT" bs=1 seek=$(( 0x1BE )) conv=notrunc status=none

echo "built $OUT: ${DISK_MB} MB"
echo "  stage 2 : LBA 1..$stage2_sectors"
echo "  kernel  : LBA $KERNEL_LBA..$(( KERNEL_LBA + kernel_sectors - 1 )) ($kernel_bytes bytes)"
if (( assets_sectors > 0 )); then
    echo "  assets  : LBA $assets_lba..$(( assets_lba + assets_sectors - 1 )) ($assets_bytes bytes, $ASSETS)"
else
    echo "  assets  : none (boot install / repair will not have the system files)"
fi
echo "  data    : LBA $RESERVED..$(( total_sectors - 1 )) (partition 1)"

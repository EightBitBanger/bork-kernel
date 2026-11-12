#!/usr/bin/env bash
#
# Build the loadable drivers: every folder under drivers/ except sdk/
# becomes one driver file, bin/drivers/<folder name>, which starts with
# "kdrv" (kernel/driver/kdrv.h). assets.txt puts them in sys/drivers/.
#
#   ./build-drivers.sh            all of them
#   ./build-drivers.sh hello      just drivers/hello
#
# Each driver's .c files are compiled with the kernel's code flags, linked
# with drivers/sdk/kdrv.ld at address 0 (relocations kept), and packed by
# tools/mkkdrv, which is built for this machine first.
#
# Environment:
#   CC        i386 compiler (default: i686-elf-gcc if found, else gcc -m32)
#   LD        its linker    (default: i686-elf-ld if found, else ld -m elf_i386)
#   HOSTCC    compiler for tools/mkkdrv (default: cc)
#
set -euo pipefail

cd "$(dirname "$0")"

die() { echo "build-drivers: $*" >&2; exit 1; }

if [ -z "${CC:-}" ]; then
    if command -v i686-elf-gcc >/dev/null 2>&1; then CC=i686-elf-gcc; else CC="gcc -m32"; fi
fi
if [ -z "${LD:-}" ]; then
    if command -v i686-elf-ld >/dev/null 2>&1; then LD=i686-elf-ld; else LD="ld -m elf_i386"; fi
fi
HOSTCC=${HOSTCC:-cc}

OBJ=build/drivers
OUT=bin/drivers
mkdir -p "$OBJ" "$OUT" build/tools

# The packer, for this machine
if [ ! -x build/tools/mkkdrv ] || [ tools/mkkdrv.c -nt build/tools/mkkdrv ] ||
   [ kernel/driver/kdrv.h -nt build/tools/mkkdrv ] || [ kernel/driver/kapi.h -nt build/tools/mkkdrv ]; then
    $HOSTCC -O2 -std=c99 -I. -o build/tools/mkkdrv tools/mkkdrv.c
fi

# The kernel's code flags (cmake/x86.cmake), plus what keeps a driver free
# of imports: no PIC, no stack protector, no unwind tables
CFLAGS=(-std=gnu99 -O2 -ffreestanding -fno-pie -fno-pic -msse2 -fno-omit-frame-pointer
        -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin
        -Wall -Wextra -I. -Idrivers/sdk -DKERNEL_PLATFORM_X86)

if [ $# -gt 0 ]; then
    names=("$@")
else
    names=()
    for dir in drivers/*/; do
        name=$(basename "$dir")
        [ "$name" = sdk ] || names+=("$name")
    done
fi

# Newer binutils warn about the one read-write-execute image; it is meant
LDFLAGS=(-T drivers/sdk/kdrv.ld --emit-relocs -nostdlib)
if $LD --help 2>/dev/null | grep -q -- --no-warn-rwx-segments; then
    LDFLAGS+=(--no-warn-rwx-segments)
fi

$CC "${CFLAGS[@]}" -c drivers/sdk/kdrv_runtime.c -o "$OBJ/kdrv_runtime.o"

for name in "${names[@]}"; do
    [ -d "drivers/$name" ] || die "no folder drivers/$name"
    mkdir -p "$OBJ/$name"
    objects=()
    for src in drivers/"$name"/*.c; do
        [ -f "$src" ] || die "drivers/$name has no .c files"
        obj="$OBJ/$name/$(basename "${src%.c}").o"
        $CC "${CFLAGS[@]}" -c "$src" -o "$obj"
        objects+=("$obj")
    done
    $LD "${LDFLAGS[@]}" -o "$OBJ/$name.elf" "${objects[@]}" "$OBJ/kdrv_runtime.o"
    build/tools/mkkdrv "$OBJ/$name.elf" "$OUT/$name" "$name"
done

#!/usr/bin/env bash
#
# Build a user library (os/ulib.h) from C files.
#
#   build-ulib.sh <output> <name> <abi> <file.c>... [-- <extra compiler flags>]
#
# Each file is compiled with libc's flags and -fno-pic, linked at address 0
# with ulib.ld (relocations kept) together with ulib/runtime.c (memcpy,
# strlen, abs, ...) and libc's src/math.c, and packed by tools/mkulib into
# <output>, a file starting with "ulib". The library's entry point is
# ulib_entry().
#
# libc.a is not linked: its objects keep state (the heap, stdio, errno) and
# make system calls, which belong to the program. Anything else a library
# needs from the program comes through ulib_host_t (os/ulib.h). A library
# that references a function none of this has fails to link, by name.
#
# Environment:
#   CC        i386 compiler (default: i686-elf-gcc if found, else gcc)
#   HOSTCC    compiler for tools/mkulib (default: cc)
#
set -euo pipefail

LIBC=$(cd "$(dirname "$0")/.." && pwd)

die() { echo "build-ulib: $*" >&2; exit 1; }

[ $# -ge 4 ] || die "usage: build-ulib.sh <output> <name> <abi> <file.c>... [-- <flags>]"
OUTPUT=$1; NAME=$2; ABI=$3; shift 3
SOURCES=()
EXTRA=()
while [ $# -gt 0 ]; do
    if [ "$1" = "--" ]; then shift; EXTRA=("$@"); break; fi
    SOURCES+=("$1"); shift
done
[ ${#SOURCES[@]} -gt 0 ] || die "no source files"

if [ -z "${CC:-}" ]; then
    if command -v i686-elf-gcc >/dev/null 2>&1; then CC=i686-elf-gcc; else CC=gcc; fi
fi
HOSTCC=${HOSTCC:-cc}

OBJ=$(dirname "$OUTPUT")/build-ulib/$NAME
mkdir -p "$OBJ" "$LIBC/build"

# The packer, for this machine
MKULIB=$LIBC/build/mkulib
if [ ! -x "$MKULIB" ] || [ "$LIBC/tools/mkulib.c" -nt "$MKULIB" ] || [ "$LIBC/include/os/ulib.h" -nt "$MKULIB" ]; then
    $HOSTCC -O2 -std=c99 -o "$MKULIB" "$LIBC/tools/mkulib.c"
fi

# libc's flags (CMakeLists.txt), and nothing that needs a GOT or imports
CFLAGS=(-m32 -march=i686 -std=gnu11 -O2 -ffreestanding -nostdinc
        -isystem "$($CC -print-file-name=include)" -I"$LIBC/include" -I"$LIBC/src"
        -fno-pic -fno-pie -fno-stack-protector -fcf-protection=none
        -fno-asynchronous-unwind-tables -Wall -Wextra)

LDFLAGS=(-m32 -nostdlib -static -T "$LIBC/ulib/ulib.ld" -Wl,--emit-relocs -Wl,--build-id=none
         -Wl,-Map,"$OBJ/$NAME.map")
if [ "$($CC -dumpmachine 2>/dev/null)" != "i686-elf" ]; then
    LDFLAGS+=(-no-pie)
fi
if ld --help 2>/dev/null | grep -q -- --no-warn-rwx-segments; then
    LDFLAGS+=(-Wl,--no-warn-rwx-segments)
fi

objects=()
for src in "${SOURCES[@]}"; do
    obj=$OBJ/$(basename "${src%.c}").o
    $CC "${CFLAGS[@]}" "${EXTRA[@]}" -c "$src" -o "$obj"
    objects+=("$obj")
done

# The runtime and libc's math, as libc builds its own: no builtins, and no
# turning byte loops into calls to memcpy / memset
RTFLAGS=(-fno-builtin)
if $CC --version 2>/dev/null | grep -qi gcc; then
    RTFLAGS+=(-fno-tree-loop-distribute-patterns)
fi
$CC "${CFLAGS[@]}" "${RTFLAGS[@]}" -c "$LIBC/ulib/runtime.c" -o "$OBJ/ulib_runtime.o"
$CC "${CFLAGS[@]}" "${RTFLAGS[@]}" -c "$LIBC/src/math.c" -o "$OBJ/ulib_math.o"

# --gc-sections is not used: every object here is the library's own
$CC "${LDFLAGS[@]}" -o "$OBJ/$NAME.elf" "${objects[@]}" "$OBJ/ulib_runtime.o" "$OBJ/ulib_math.o"
"$MKULIB" "$OBJ/$NAME.elf" "$OUTPUT" "$NAME" "$ABI"

#!/bin/sh
# Builds TinyCC for the kernel and stages the files it needs.
#
#   ports/tcc/build.sh <tinycc checkout> [out dir]
#
# Needs: a libc build in ../../build (cmake -S . -B build, see the libc's
# CMakeLists.txt), a gcc that can do -m32, make. Tested with tinycc mob
# (0.9.28rc). Produces <out>/tcc (the compiler, ELF for the kernel) and
# <out>/sys/tcc/..., the tree to copy to <home>/sys/tcc on the disk.
set -eu

TCC_SRC=$(cd "$1" && pwd)
OUT=$(mkdir -p "${2:-out}" && cd "${2:-out}" && pwd)
PORT=$(cd "$(dirname "$0")" && pwd)
LIBC=$(cd "$PORT/../.." && pwd)
BUILD=$LIBC/build
CRT0=$BUILD/CMakeFiles/libc_crt0.dir/src/crt0.S.obj
CC=${CC:-gcc}

[ -f "$BUILD/libc.a" ] && [ -f "$CRT0" ] || { echo "build the libc first (cmake --build $BUILD)" >&2; exit 1; }

# 1. A cross i386-tcc on the host, only to build libtcc1.a and runmain.o
HOST=$OUT/host-tcc
rm -rf "$HOST" && cp -r "$TCC_SRC" "$HOST"
(cd "$HOST" && ./configure >/dev/null && make cross-i386 >/dev/null)

# 2. tcc itself, compiled by gcc against this libc. In a copy of the
#    sources: tcc.h includes "config.h" from its own directory first, so a
#    configure-generated one there would win over ours.
SRC=$OUT/tcc-src
rm -rf "$SRC" && cp -r "$TCC_SRC" "$SRC"
cp "$PORT/config.h" "$SRC/config.h"
printf '#define TCC_VERSION "%s"\n' "$(cat "$TCC_SRC/VERSION")" >> "$SRC/config.h"
INC=$($CC -m32 -print-file-name=include)
$CC -m32 -march=i686 -O2 -std=gnu11 -ffreestanding -nostdinc -isystem "$INC" \
    -I"$LIBC/include" -I"$SRC" -DONE_SOURCE=1 \
    -fno-pic -fno-pie -fno-stack-protector -fcf-protection=none -w \
    -c "$SRC/tcc.c" -o "$OUT/tcc.o"
$CC -m32 -nostdlib -static -no-pie -T"$LIBC/user.ld" -Wl,--build-id=none \
    "$CRT0" "$OUT/tcc.o" "$BUILD/libc.a" -o "$OUT/tcc"

# 3. The tree tcc reads at run time (see config.h)
SYS=$OUT/sys/tcc
rm -rf "$SYS" && mkdir -p "$SYS/lib" "$SYS/libc"
cp -r "$TCC_SRC/include" "$SYS/include"
cp -r "$LIBC/include" "$SYS/libc/include"
cp "$CRT0" "$SYS/lib/crt1.o"
cp "$BUILD/libc.a" "$SYS/lib/libc.a"
: > "$OUT/empty.c"
"$HOST/i386-tcc" -c "$OUT/empty.c" -o "$SYS/lib/crti.o"
cp "$SYS/lib/crti.o" "$SYS/lib/crtn.o"
cp "$HOST/i386-libtcc1.a" "$SYS/lib/libtcc1.a"
"$HOST/i386-tcc" -nostdinc -I"$HOST/include" -I"$LIBC/include" -c "$HOST/lib/runmain.c" -o "$SYS/lib/runmain.o"

echo "$OUT/tcc"
echo "$SYS -> copy to <home>/sys/tcc"

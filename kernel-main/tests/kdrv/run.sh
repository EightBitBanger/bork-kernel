#!/usr/bin/env bash
#
# Build bin/drivers/hello and demolib and run the loader test against them on this
# machine (32-bit: needs gcc -m32, e.g. Ubuntu's gcc-multilib)
#
#   tests/kdrv/run.sh
#
set -euo pipefail
cd "$(dirname "$0")/../.."

./build-drivers.sh hello demolib

out=build/tests/kdrv
mkdir -p "$out"

# The loader exactly as the kernel compiles it
gcc -m32 -std=gnu99 -O2 -ffreestanding -fno-pie -msse2 -fno-omit-frame-pointer \
    -DKERNEL_PLATFORM_X86 -I. -Ikernel -Iinclude -Wall -Wextra \
    -c kernel/driver/kdrv_loader.c -o "$out/kdrv_loader.o"
gcc -m32 -std=gnu99 -O2 -ffreestanding -fno-pie -msse2 -fno-omit-frame-pointer \
    -DKERNEL_PLATFORM_X86 -I. -Ikernel -Iinclude -Wall -Wextra \
    -c kernel/driver/drvcall.c -o "$out/drvcall.o"

gcc -m32 -std=gnu99 -O1 -g -I. -Wall -Wno-unused-parameter \
    -c tests/kdrv/kdrv_test.c -o "$out/kdrv_test.o"
gcc -m32 -no-pie -o "$out/kdrv_test" "$out/kdrv_test.o" "$out/kdrv_loader.o" "$out/drvcall.o"

"$out/kdrv_test" bin/drivers/hello bin/drivers/demolib

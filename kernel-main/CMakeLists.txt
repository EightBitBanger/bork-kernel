# CMake build for the kernel. One build directory per target machine.
#
# x86 (the default), mirroring x86-kernel.cbp (Code::Blocks):
#
#   cmake -S . -B build                # Release (-O2, stripped) by default
#   cmake --build build                # bin/x86-kernel.bin, bin/assets.tar, boot-disk.img
#   cmake --build build --target run   # boot it in QEMU (run-image.sh)
#
# Raspberry Pi 3 Model B+ (aarch64), a serial boot stub so far
# (see kernel/boot/rpi3/README.md):
#
#   cmake -S . -B build-rpi3 -DKERNEL_TARGET=rpi3
#   cmake --build build-rpi3                # bin/kernel8.img, bin/rpi3-boot/
#   cmake --build build-rpi3 --target run   # QEMU raspi3b, serial on stdio
#
# Outputs go to bin/ and the project root, the same places Code::Blocks and
# the shell scripts use, so build.sh / run-image.sh keep working unchanged.

cmake_minimum_required(VERSION 3.16)

set(KERNEL_TARGET x86 CACHE STRING "Machine to build the kernel for: x86 or rpi3")
set_property(CACHE KERNEL_TARGET PROPERTY STRINGS x86 rpi3)

if(KERNEL_TARGET STREQUAL "rpi3")
    # A cross compiler, picked before project() looks for one
    if(NOT CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-aarch64.cmake)
    endif()
elseif(NOT KERNEL_TARGET STREQUAL "x86")
    message(FATAL_ERROR "KERNEL_TARGET must be x86 or rpi3, not '${KERNEL_TARGET}'")
endif()

# The kernel is linked with its own linker script, so skip the compiler
# check's attempt to link a hosted test program.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# nasm -f elf32, as in each .asm file's custom build command in the .cbp.
# Must be set before the ASM_NASM language is enabled (cmake/x86.cmake).
set(CMAKE_ASM_NASM_OBJECT_FORMAT elf32)

project(bork-kernel C)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Code::Blocks Release target: -O2 to compile, -s to link
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-s")

# Sources that build unchanged for every target. A file moves here from a
# target's list once it no longer depends on that CPU (see kernel/arch/cpu.h
# and the *_platform.h headers for how the CPU-specific parts are split off).
set(KERNEL_PORTABLE_C_SOURCES
    kernel/util/parser.c
    kernel/util/snprint.c
    kernel/util/string.c
    kernel/util/tar.c
    kernel/util/tok.c
)

include(${CMAKE_SOURCE_DIR}/cmake/${KERNEL_TARGET}.cmake)

# Raspberry Pi 3 Model B+ kernel (KERNEL_TARGET=rpi3): BCM2837B0, Cortex-A53,
# aarch64. Included from the top-level CMakeLists.txt with the cross
# toolchain from cmake/toolchain-aarch64.cmake.
#
# Only the boot stub, the aarch64 CPU layer and the portable sources build
# so far. Each piece of the kernel joins KERNEL_PORTABLE_C_SOURCES (or this
# list) as it stops depending on x86; kernel/boot/rpi3/boot.c has the order.

enable_language(ASM)

set(RPI3_LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/boot/rpi3/linker.ld)

set(KERNEL_RPI3_C_SOURCES
    kernel/arch/aarch64/exception.c
    kernel/arch/aarch64/scheduler/sched_aarch64.c
    kernel/boot/rpi3/boot.c
    kernel/boot/rpi3/mailbox.c
    kernel/boot/rpi3/uart.c
)

set(KERNEL_RPI3_ASM_SOURCES
    kernel/boot/rpi3/start.S
    kernel/arch/aarch64/vectors.S
)

add_executable(rpi3-kernel ${KERNEL_PORTABLE_C_SOURCES} ${KERNEL_RPI3_C_SOURCES} ${KERNEL_RPI3_ASM_SOURCES})

set_target_properties(rpi3-kernel PROPERTIES
    OUTPUT_NAME kernel8
    SUFFIX .elf
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
    LINKER_LANGUAGE C
    LINK_DEPENDS ${RPI3_LINKER_SCRIPT}
)

target_include_directories(rpi3-kernel PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}
)

# -mgeneral-regs-only: the exception frame does not hold the FP/SIMD
#   registers, so kernel C must not touch them (the x86 kernel saves them
#   with FXSAVE instead).
# -mstrict-align: with the MMU off all memory is Device memory, where an
#   unaligned access faults. Can go once mmu_init() maps RAM as Normal.
target_compile_options(rpi3-kernel PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=gnu99 -ffreestanding -fno-pie -fno-stack-protector
                            -mcpu=cortex-a53 -mgeneral-regs-only -mstrict-align
                            -fno-omit-frame-pointer>
    $<$<COMPILE_LANGUAGE:ASM>:-mcpu=cortex-a53>
)
target_compile_definitions(rpi3-kernel PRIVATE
    KERNEL_PLATFORM_AARCH64
    KERNEL_BOARD_RPI3
)

target_link_options(rpi3-kernel PRIVATE
    ${AARCH64_LINKER_FLAGS}
    -T ${RPI3_LINKER_SCRIPT}
    -nostdlib
    -static
    -Wl,--build-id=none
)

# The firmware boots a flat binary, not an ELF
set(RPI3_KERNEL_IMAGE ${CMAKE_SOURCE_DIR}/bin/kernel8.img)
add_custom_command(TARGET rpi3-kernel POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:rpi3-kernel> ${RPI3_KERNEL_IMAGE}
    COMMENT "Building bin/kernel8.img"
    VERBATIM
)

# What goes on the SD card's boot partition, minus the firmware files
# (bootcode.bin, start.elf, fixup.dat), which come from the Raspberry Pi
# firmware repository; see kernel/boot/rpi3/README.md
set(RPI3_BOOT_DIR ${CMAKE_SOURCE_DIR}/bin/rpi3-boot)
add_custom_target(rpi3-boot ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RPI3_BOOT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${RPI3_KERNEL_IMAGE} ${RPI3_BOOT_DIR}/kernel8.img
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/kernel/boot/rpi3/config.txt ${RPI3_BOOT_DIR}/config.txt
    DEPENDS rpi3-kernel ${CMAKE_SOURCE_DIR}/kernel/boot/rpi3/config.txt
    COMMENT "Collecting bin/rpi3-boot/"
    VERBATIM
)

# Boot it under QEMU's raspi3b (a Pi 3B, close enough to the B+ for this)
add_custom_target(run
    COMMAND bash ${CMAKE_SOURCE_DIR}/run-image-rpi3.sh
    DEPENDS rpi3-kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    VERBATIM
)

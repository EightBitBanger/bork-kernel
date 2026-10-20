# CMake build for the x86 kernel, mirroring x86-kernel.cbp (Code::Blocks).
#
#   cmake -S . -B build                # Release (-O2, stripped) by default
#   cmake --build build                # bin/x86-kernel.bin + boot-disk.img
#   cmake --build build --target run   # boot it in QEMU (run-image.sh)
#
# Outputs go to bin/ and the project root, the same places Code::Blocks and
# the shell scripts use, so build.sh / run-image.sh keep working unchanged.

cmake_minimum_required(VERSION 3.16)

# The kernel is linked by gcc with its own linker script, so skip the
# compiler check's attempt to link a hosted test program.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# nasm -f elf32, as in each .asm file's custom build command in the .cbp.
# Must be set before the ASM_NASM language is enabled.
set(CMAKE_ASM_NASM_OBJECT_FORMAT elf32)

project(x86-kernel C ASM_NASM)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(KERNEL_BUILD_IMAGE "Build boot-disk.img after the kernel" ON)

# Code::Blocks Release target: -O2 to compile, -s to link
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-s")

set(KERNEL_LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/boot/x86/linker.ld)

set(KERNEL_C_SOURCES
    kernel/arch/x86/bus/pci/pci.c
    kernel/arch/x86/bus/pci/pci_config.c
    kernel/arch/x86/bus/pci/pci_prop.c
    kernel/arch/x86/bus/pci/pci_scan.c
    kernel/arch/x86/bus/pci/pci_storage.c
    kernel/arch/x86/bus/pci/pci_strings.c
    kernel/arch/x86/bus/usb/hcd/ehci.c
    kernel/arch/x86/bus/usb/hcd/uhci.c
    kernel/arch/x86/bus/usb/hcd/xhci.c
    kernel/arch/x86/bus/usb/usb.c
    kernel/arch/x86/bus/usb/usb_enum.c
    kernel/arch/x86/bus/usb/usb_hub.c
    kernel/arch/x86/bus/usb/usb_prop.c
    kernel/arch/x86/drivers/ahci/ahci.c
    kernel/arch/x86/drivers/ata/ata.c
    kernel/arch/x86/drivers/display/display.c
    kernel/arch/x86/drivers/display/draw.c
    kernel/arch/x86/drivers/keyboard.c
    kernel/arch/x86/drivers/mouse.c
    kernel/arch/x86/drivers/ps2.c
    kernel/arch/x86/drivers/rng.c
    kernel/arch/x86/drivers/serial.c
    kernel/arch/x86/drivers/system.c
    kernel/arch/x86/drivers/timer.c
    kernel/arch/x86/heap.c
    kernel/arch/x86/malloc.c
    kernel/arch/x86/slab.c
    kernel/arch/x86/virtual/pmm.c
    kernel/arch/x86/virtual/vmm.c
    kernel/block/block.c
    kernel/block/partition.c
    kernel/boot/x86/boot.c
    kernel/boot/x86/boot_image.c
    kernel/boot/x86/gdt.c
    kernel/boot/x86/interrupt.c
    kernel/boot/x86/kernel.c
    kernel/boot/x86/kernel_lock.c
    kernel/boot/x86/mutex.c
    kernel/boot/x86/vector_remap.c
    kernel/certification/session.c
    kernel/console/command.c
    kernel/console/console.c
    kernel/console/print.c
    kernel/console/virtual_key.c
    kernel/events.c
    kernel/fs/basefs/backend/io_block.c
    kernel/fs/basefs/bitmap.c
    kernel/fs/basefs/cryptography.c
    kernel/fs/basefs/device.c
    kernel/fs/basefs/directory.c
    kernel/fs/basefs/file.c
    kernel/fs/basefs/fstype.c
    kernel/gfx/gfx_display.c
    kernel/gfx/gfx_draw.c
    kernel/gfx/gfx_surface.c
    kernel/gfx/gfx_text.c
    kernel/ipc/ipc.c
    kernel/ipc/ipc_x86.c
    kernel/kernel.c
    kernel/knode.c
    kernel/panic/panic_error.c
    kernel/registry/registry.c
    kernel/registry/registry_util.c
    kernel/resources/classes/elf.c
    kernel/resources/classes/image.c
    kernel/resources/classes/sprite.c
    kernel/resources/resource_io.c
    kernel/resources/resource_manager.c
    kernel/scheduler/process.c
    kernel/scheduler/scheduler.c
    kernel/scheduler/watchdog.c
    kernel/syscall/syscall.c
    kernel/util/div6432.c
    kernel/util/math.c
    kernel/util/parser.c
    kernel/util/random.c
    kernel/util/snprint.c
    kernel/util/string.c
    kernel/util/tok.c
    kernel/vfs/vfs.c
    kernel/vfs/vfs_copy.c
    kernel/vfs/vfs_cryptography.c
    kernel/vfs/vfs_device.c
    kernel/vfs/vfs_file.c
    kernel/vfs/vfs_lock.c
    kernel/vfs/vfs_metadata.c
    kernel/vfs/vfs_mount.c
    kernel/vfs/vfs_node.c
    kernel/vfs/vfs_parse.c
    kernel/vfs/vfs_security.c
    kernel/vfs/vfs_size.c
)

set(KERNEL_ASM_SOURCES
    kernel/boot/x86/multiboot.asm
    kernel/boot/x86/isr/isr_div_zero.asm
    kernel/boot/x86/isr/isr_dummy.asm
    kernel/boot/x86/isr/isr_exceptions.asm
    kernel/boot/x86/isr/isr_general_fault.asm
    kernel/boot/x86/isr/isr_keyboard.asm
    kernel/boot/x86/isr/isr_mouse.asm
    kernel/boot/x86/isr/isr_nmi.asm
    kernel/boot/x86/isr/isr_page_fault.asm
    kernel/boot/x86/isr/isr_syscall.asm
    kernel/boot/x86/isr/isr_timer.asm
    kernel/boot/x86/isr/scheduler_context.asm
    kernel/boot/x86/isr/scheduler_yield_context.asm
)

add_executable(x86-kernel ${KERNEL_C_SOURCES} ${KERNEL_ASM_SOURCES})

set_target_properties(x86-kernel PROPERTIES
    OUTPUT_NAME x86-kernel
    SUFFIX .bin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
    LINKER_LANGUAGE C
    LINK_DEPENDS ${KERNEL_LINKER_SCRIPT}
)

target_include_directories(x86-kernel PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}
)

# -std=c11 is also in the .cbp, but the later -std=gnu99 is the one gcc uses
target_compile_options(x86-kernel PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-m32 -std=gnu99 -ffreestanding -fno-pie -msse2 -fno-omit-frame-pointer>
)
target_compile_definitions(x86-kernel PRIVATE
    $<$<COMPILE_LANGUAGE:C>:KERNEL_PLATFORM_X86>
)

target_link_options(x86-kernel PRIVATE
    -m32
    -T ${KERNEL_LINKER_SCRIPT}
    -nostdlib
    -no-pie
)

# Code::Blocks pre-build step; skipped when the script is not present
add_custom_target(copy_local_cabinate
    COMMAND sh -c "if [ -f ./copy_local_cabinate.sh ]; then sh ./copy_local_cabinate.sh; fi"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running copy_local_cabinate.sh (if present)"
    VERBATIM
)
add_dependencies(x86-kernel copy_local_cabinate)

# Bootable raw disk image: MBR + stage 2 loader + kernel (build-bootloader.sh)
set(BOOT_DISK_IMAGE ${CMAKE_SOURCE_DIR}/boot-disk.img)
add_custom_command(
    OUTPUT ${BOOT_DISK_IMAGE}
    COMMAND bash ${CMAKE_SOURCE_DIR}/build-bootloader.sh $<TARGET_FILE:x86-kernel> ${BOOT_DISK_IMAGE}
    DEPENDS
        x86-kernel
        ${CMAKE_SOURCE_DIR}/build-bootloader.sh
        ${CMAKE_SOURCE_DIR}/kernel/boot/x86/loader/stage1.asm
        ${CMAKE_SOURCE_DIR}/kernel/boot/x86/loader/stage2.asm
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building boot-disk.img"
    VERBATIM
)
if(KERNEL_BUILD_IMAGE)
    add_custom_target(image ALL DEPENDS ${BOOT_DISK_IMAGE})
else()
    add_custom_target(image DEPENDS ${BOOT_DISK_IMAGE})
endif()

# Code::Blocks "Run" target: boot in QEMU from the USB stick image.
# For the installed-disk mode run "./run-image.sh disk" by hand.
add_custom_target(run
    COMMAND bash ${CMAKE_SOURCE_DIR}/run-image.sh
    DEPENDS x86-kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    VERBATIM
)

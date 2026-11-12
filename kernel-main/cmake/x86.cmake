# x86 kernel (KERNEL_TARGET=x86), mirroring x86-kernel.cbp (Code::Blocks).
# Included from the top-level CMakeLists.txt.

enable_language(ASM_NASM)

option(KERNEL_BUILD_IMAGE "Build boot-disk.img after the kernel" ON)

set(KERNEL_LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/boot/x86/linker.ld)

# Every .c and .asm under kernel/ builds into the x86 kernel except what
# belongs to another machine or is not built at all, so a new source file
# is picked up without editing this list (CONFIGURE_DEPENDS re-runs the
# glob on each build). Code::Blocks still needs new files added to
# x86-kernel.cbp by hand.
file(GLOB_RECURSE KERNEL_X86_C_SOURCES CONFIGURE_DEPENDS
    RELATIVE ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/kernel/*.c
)
list(FILTER KERNEL_X86_C_SOURCES EXCLUDE REGEX
    "^kernel/(arch/aarch64|boot/rpi[0-9]+|emulation)/"
)
# FatFs: ff.c only. kernel/fs/fat/fat_diskio.c replaces the stock
# diskio.c, and ffsystem.c is only needed for FF_FS_REENTRANT or
# FF_USE_LFN 3, neither of which ffconf.h sets.
list(FILTER KERNEL_X86_C_SOURCES EXCLUDE REGEX
    "^kernel/fs/fatfs/source/(diskio|ffsystem)\\.c$"
)
# Built for every machine from the top-level CMakeLists.txt
list(REMOVE_ITEM KERNEL_X86_C_SOURCES ${KERNEL_PORTABLE_C_SOURCES})

# The boot loader (kernel/boot/x86/loader) is built by build-bootloader.sh
file(GLOB_RECURSE KERNEL_ASM_SOURCES CONFIGURE_DEPENDS
    RELATIVE ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/kernel/*.asm
)
list(FILTER KERNEL_ASM_SOURCES EXCLUDE REGEX "^kernel/boot/x86/loader/")

add_executable(x86-kernel ${KERNEL_PORTABLE_C_SOURCES} ${KERNEL_X86_C_SOURCES} ${KERNEL_ASM_SOURCES})

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

# Asset archive "boot install" / "boot repair" extract: everything
# assets.txt lists (build-assets.sh), programs as built, fonts and images as
# the files they are (../fonts/*.ttf, ../bitmaps/**/*.png). Runs every build
# since the programs it packs are built elsewhere (build-asset-binaries.sh);
# the archive file is only rewritten when its content changes. The kernel itself
# no longer compiles in any asset, so nothing has to run before it.
# Loadable drivers (drivers/, build-drivers.sh): bin/drivers/<name>, which
# assets.txt puts in sys/drivers/. Built every time like the assets; the
# script only takes a moment.
add_custom_target(drivers ALL
    COMMAND bash ${CMAKE_SOURCE_DIR}/build-drivers.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building bin/drivers"
    VERBATIM
)

set(BOOT_ASSETS ${CMAKE_SOURCE_DIR}/bin/assets.tar)
add_custom_target(assets ALL
    COMMAND bash ${CMAKE_SOURCE_DIR}/build-assets.sh ${CMAKE_SOURCE_DIR}/assets.txt ${BOOT_ASSETS}
    BYPRODUCTS ${BOOT_ASSETS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building bin/assets.tar"
    VERBATIM
)
add_dependencies(assets drivers)

# Bootable raw disk image: MBR + stage 2 loader + kernel + asset archive
# (build-bootloader.sh)
set(BOOT_DISK_IMAGE ${CMAKE_SOURCE_DIR}/boot-disk.img)
add_custom_command(
    OUTPUT ${BOOT_DISK_IMAGE}
    COMMAND bash ${CMAKE_SOURCE_DIR}/build-bootloader.sh $<TARGET_FILE:x86-kernel> ${BOOT_DISK_IMAGE} ${BOOT_ASSETS}
    DEPENDS
        x86-kernel
        assets
        ${BOOT_ASSETS}
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

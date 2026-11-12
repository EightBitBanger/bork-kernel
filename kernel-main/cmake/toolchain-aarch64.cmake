# Cross toolchain for the aarch64 (Raspberry Pi) kernel.
#
# Used automatically for -DKERNEL_TARGET=rpi3. Takes the first of:
#   aarch64-none-elf-gcc     Arm GNU Toolchain (bare metal)
#   aarch64-linux-gnu-gcc    Debian / Ubuntu gcc-aarch64-linux-gnu
#   clang + ld.lld           any LLVM install (--target=aarch64-none-elf)
# Set AARCH64_TOOLCHAIN_PREFIX (e.g. /opt/arm/bin/aarch64-none-elf-) to pick
# a GNU one by hand.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT AARCH64_TOOLCHAIN_PREFIX)
    foreach(prefix aarch64-none-elf- aarch64-elf- aarch64-linux-gnu-)
        find_program(_aarch64_gcc ${prefix}gcc)
        if(_aarch64_gcc)
            set(AARCH64_TOOLCHAIN_PREFIX ${prefix})
            break()
        endif()
        unset(_aarch64_gcc CACHE)
    endforeach()
endif()
set(AARCH64_TOOLCHAIN_PREFIX "${AARCH64_TOOLCHAIN_PREFIX}" CACHE STRING "GNU aarch64 tool prefix (empty: clang)")

if(AARCH64_TOOLCHAIN_PREFIX)
    set(CMAKE_C_COMPILER   ${AARCH64_TOOLCHAIN_PREFIX}gcc)
    set(CMAKE_ASM_COMPILER ${AARCH64_TOOLCHAIN_PREFIX}gcc)
    set(CMAKE_OBJCOPY      ${AARCH64_TOOLCHAIN_PREFIX}objcopy CACHE FILEPATH "objcopy")
    set(AARCH64_LINKER_FLAGS "")
else()
    find_program(_clang clang REQUIRED)
    find_program(_lld ld.lld REQUIRED)
    find_program(_llvm_objcopy NAMES llvm-objcopy llvm-objcopy-18 llvm-objcopy-17 REQUIRED)
    set(CMAKE_C_COMPILER   ${_clang})
    set(CMAKE_ASM_COMPILER ${_clang})
    set(CMAKE_C_COMPILER_TARGET   aarch64-none-elf)
    set(CMAKE_ASM_COMPILER_TARGET aarch64-none-elf)
    set(CMAKE_OBJCOPY ${_llvm_objcopy} CACHE FILEPATH "objcopy")
    set(AARCH64_LINKER_FLAGS "-fuse-ld=lld")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Toolchain for user programs on the kernel: 32-bit x86, no host OS.
#
# Uses i686-elf-gcc when it is on PATH, otherwise the host gcc with -m32.
# Pick another compiler with -DCMAKE_C_COMPILER=... (e.g. clang).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR i686)

if(NOT CMAKE_C_COMPILER)
    find_program(_LIBC_CROSS_GCC i686-elf-gcc)
    if(_LIBC_CROSS_GCC)
        set(CMAKE_C_COMPILER ${_LIBC_CROSS_GCC})
    else()
        set(CMAKE_C_COMPILER gcc)
    endif()
endif()
if(NOT CMAKE_ASM_COMPILER)
    set(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER})
endif()

set(CMAKE_C_FLAGS_INIT   "-m32 -march=i686")
set(CMAKE_ASM_FLAGS_INIT "-m32 -march=i686")

# The compiler checks must not try to link against a host C library
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)


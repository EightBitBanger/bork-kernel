# User-space libc for the kernel.
#
# Standalone:
#   cmake -S . -B build -G Ninja
#   cmake --build build
#
# From your own project (any directory layout):
#   cmake_minimum_required(VERSION 3.16)
#   set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/libc/cmake/toolchain-i686.cmake)
#   project(myapp C ASM)
#   add_subdirectory(libc)
#   libc_add_program(myapp main.c other.c)      # -> myapp (ELF) and myapp.bin
#   libc_use_stblib(myapp)                      # optional: stb from sys/lib/stb
#
# Options:
#   LIBC_IMAGE_BASE        load address of programs (default 0x40000000)
#   LIBC_SHARED_BUILD      ON (default): a parent project links the one libc.a
#                          built in LIBC_SHARED_BUILD_DIR, so all programs share
#                          a single libc build instead of each compiling its
#                          own copy. OFF: build libc inside the parent's tree.
#   LIBC_SHARED_BUILD_DIR  where that libc.a is built (default: build/ here)

cmake_minimum_required(VERSION 3.16)

# Standalone builds (this is the top-level CMakeLists.txt, e.g. opened in an
# IDE) set up the i386 toolchain right here, so no separate toolchain file is
# needed. A parent project should instead set CMAKE_TOOLCHAIN_FILE to
# cmake/toolchain-i686.cmake before its project() call.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_TOOLCHAIN_FILE)
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
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
endif()

# Everything below lives next to this file; say so plainly if it doesn't
foreach(_libc_required include src user.ld)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_libc_required})
        message(FATAL_ERROR
            "libc: ${CMAKE_CURRENT_SOURCE_DIR}/${_libc_required} is missing.\n"
            "Unzip the whole libc folder (include/, src/, user.ld, cmake/, ...) "
            "and point CMake at the folder that holds this CMakeLists.txt.")
    endif()
endforeach()

project(kernel_libc C ASM)

set(_libc_standalone OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_libc_standalone ON)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
endif()

set(LIBC_IMAGE_BASE "0x40000000" CACHE STRING "Address user programs are linked at")

if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "libc needs GCC or Clang (found ${CMAKE_C_COMPILER_ID})")
endif()

#
# Flags
#

# The compiler's own freestanding headers (stddef.h, stdint.h, stdarg.h,
# stdbool.h, float.h); every other header comes from include/
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
    OUTPUT_VARIABLE LIBC_COMPILER_INCLUDE
    OUTPUT_STRIP_TRAILING_WHITESPACE)

# Cached so libc_add_program() sees it when called from a parent project
set(LIBC_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/user.ld CACHE INTERNAL "")
set(LIBC_STBLIB_CLIENT ${CMAKE_CURRENT_SOURCE_DIR}/stblib/stblib_client.c CACHE INTERNAL "")

# Shared by the library and the programs that use it
set(_libc_common_options
    -std=gnu11
    -ffreestanding
    -nostdinc
    "SHELL:-isystem ${LIBC_COMPILER_INCLUDE}"
    -fno-pic
    -fno-pie
    -fno-stack-protector
    -fcf-protection=none
    -fno-asynchronous-unwind-tables)

# The library itself: no builtins, and (GCC) no turning the byte loops in
# string.c into calls to memcpy/memset, which would recurse
set(_libc_build_options -fno-builtin -Wall -Wextra)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND _libc_build_options -fno-tree-loop-distribute-patterns)
endif()

set(_libc_link_options
    -nostdlib
    -static
    "-T${LIBC_LINKER_SCRIPT}"
    "-Wl,--defsym=__image_base=${LIBC_IMAGE_BASE}"
    -Wl,--build-id=none
    -Wl,-z,noexecstack
    -Wl,-z,max-page-size=4096)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND _libc_link_options -no-pie)
endif()

# Every .c file in src/ (crt0.S is its own target, below). CONFIGURE_DEPENDS
# re-checks the folder on each build, so a file added or removed there is
# picked up without touching this list.
file(GLOB _libc_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

#
# Targets
#
#   libc_crt0    program entry point (object library)
#   c            libc.a
#   libc_build   (parent project, LIBC_SHARED_BUILD) brings the shared
#                libc.a / crt0.o up to date; libc_crt0 and c are then
#                imported from it
#   libc::libc   what programs link: libc.a + crt0 + headers + flags + user.ld
#

option(LIBC_SHARED_BUILD "Programs link one shared libc.a instead of each building its own" ON)

if(_libc_standalone OR NOT LIBC_SHARED_BUILD)
    add_library(libc_crt0 OBJECT src/crt0.S)

    add_library(c STATIC ${_libc_sources})
    target_include_directories(c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(c PRIVATE ${_libc_common_options} ${_libc_build_options})
    set_target_properties(c PROPERTIES POSITION_INDEPENDENT_CODE OFF)

    # crt0.o next to libc.a, where programs sharing this build find it
    if(_libc_standalone)
        add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/crt0.o
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_OBJECTS:libc_crt0> ${CMAKE_BINARY_DIR}/crt0.o
            DEPENDS libc_crt0 $<TARGET_OBJECTS:libc_crt0>
            VERBATIM)
        add_custom_target(libc_crt0_copy ALL DEPENDS ${CMAKE_BINARY_DIR}/crt0.o)
        # The compiler this libc.a is built with (cmake/build-shared.cmake
        # reconfigures when a program asks for another one)
        file(WRITE ${CMAKE_BINARY_DIR}/libc-compiler.txt "${CMAKE_C_COMPILER}")
    endif()
else()
    # A program's build: libc.a and crt0.o come from the one shared libc
    # build (standalone, in LIBC_SHARED_BUILD_DIR). libc_build brings it up
    # to date before the program links: the first time it configures and
    # compiles the libc, after that it only recompiles libc files that
    # changed, and does nothing when none did.
    set(LIBC_SHARED_BUILD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build CACHE PATH "Where the shared libc.a is built")
    set(_libc_shared_lib  ${LIBC_SHARED_BUILD_DIR}/libc.a)
    set(_libc_shared_crt0 ${LIBC_SHARED_BUILD_DIR}/crt0.o)

    add_custom_target(libc_build
        COMMAND ${CMAKE_COMMAND}
                -DLIBC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DBUILD_DIR=${LIBC_SHARED_BUILD_DIR}
                -DGENERATOR=${CMAKE_GENERATOR}
                -DC_COMPILER=${CMAKE_C_COMPILER}
                -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build-shared.cmake
        BYPRODUCTS ${_libc_shared_lib} ${_libc_shared_crt0}
        COMMENT "Checking the shared libc in ${LIBC_SHARED_BUILD_DIR}"
        VERBATIM)

    add_library(libc_crt0 OBJECT IMPORTED GLOBAL)
    set_target_properties(libc_crt0 PROPERTIES IMPORTED_OBJECTS ${_libc_shared_crt0})
    add_dependencies(libc_crt0 libc_build)

    add_library(c STATIC IMPORTED GLOBAL)
    set_target_properties(c PROPERTIES
        IMPORTED_LOCATION ${_libc_shared_lib}
        INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_dependencies(c libc_build)
endif()

add_library(libc_runtime INTERFACE)
add_library(libc::libc ALIAS libc_runtime)
target_link_libraries(libc_runtime INTERFACE c)
target_include_directories(libc_runtime INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(libc_runtime INTERFACE ${_libc_common_options})
target_link_options(libc_runtime INTERFACE ${_libc_link_options})
target_sources(libc_runtime INTERFACE $<TARGET_OBJECTS:libc_crt0>)

#
# libc_add_program(<name> <sources>...)
#
# An executable linked for the kernel (crt0 + user.ld + libc), plus
# <name>.bin, a flat binary of the same image for a loader that copies the
# file to the load address instead of reading ELF segments.
#
function(libc_add_program name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE libc::libc)
    if(TARGET libc_build)
        add_dependencies(${name} libc_build)
    else()
        add_dependencies(${name} libc_crt0)
    endif()
    set_target_properties(${name} PROPERTIES
        POSITION_INDEPENDENT_CODE OFF
        LINK_DEPENDS ${LIBC_LINKER_SCRIPT})

    if(CMAKE_OBJCOPY)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${name}> $<TARGET_FILE:${name}>.bin
            COMMENT "Flat binary ${name}.bin"
            VERBATIM)
    endif()
endfunction()

#
# libc_use_stblib(<name>)
#
# The program gets stb_image, stb_image_write and stb_truetype from the stb
# library (<home>/sys/lib/stb, programs/stblib) at run time instead of
# linking ~140 KB of their code in. os/image.h and os/font.h work as
# before; see os/stblib.h.
#
function(libc_use_stblib name)
    target_sources(${name} PRIVATE ${LIBC_STBLIB_CLIENT})
endfunction()

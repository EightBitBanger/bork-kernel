#
# User application template for the kernel's libc.
#
# Copy this folder, rename the project below, put your code in src/, build:
#
#   cmake -S . -B build -G Ninja -DLIBC_DIR=/path/to/libc
#   cmake --build build
#
# Output (in build/):
#   <app>        ELF image, linked at LIBC_IMAGE_BASE (0x40000000 by default)
#   <app>.bin    flat binary of the same image, for a loader that copies it in
#   <app>.map    linker map
#
# Settings (all optional, -D on the command line or in a CMake preset):
#   LIBC_DIR          the libc folder (the one holding include/, src/, user.ld).
#                     Default: ./libc next to this file, else $ENV{LIBC_DIR}
#   APP_NAME          program name (default: the project name)
#   LIBC_IMAGE_BASE   load address, if your loader uses a different one
#   APP_INSTALL_DIR   copy <app> / <app>.bin here after each build, e.g. the
#                     staging folder you turn into a disk image
#   APP_FLAT_BINARY   install the .bin instead of the ELF (default OFF)
#   CMAKE_C_COMPILER  another i386 compiler (clang, i686-elf-gcc ...)
#

cmake_minimum_required(VERSION 3.16)

#
# Where the libc lives. Resolved before project() so the toolchain file in it
# can be used.
#
if(NOT LIBC_DIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libc/CMakeLists.txt")
        set(LIBC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libc")
    elseif(DEFINED ENV{LIBC_DIR})
        set(LIBC_DIR "$ENV{LIBC_DIR}")
    endif()
endif()
set(LIBC_DIR "${LIBC_DIR}" CACHE PATH "The kernel libc folder (holds include/, src/, user.ld)")

if(NOT LIBC_DIR OR NOT EXISTS "${LIBC_DIR}/CMakeLists.txt" OR NOT EXISTS "${LIBC_DIR}/user.ld")
    message(FATAL_ERROR
        "libc not found (LIBC_DIR = '${LIBC_DIR}').\n"
        "Put the libc folder next to this CMakeLists.txt as ./libc, set the\n"
        "LIBC_DIR environment variable, or configure with -DLIBC_DIR=/path/to/libc.")
endif()
get_filename_component(LIBC_DIR "${LIBC_DIR}" ABSOLUTE)

# The i386 freestanding toolchain, unless one was chosen already
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${LIBC_DIR}/cmake/toolchain-i686.cmake")
endif()

project(explorer VERSION 0.1.0 LANGUAGES C ASM)

set(APP_NAME "${PROJECT_NAME}" CACHE STRING "Name of the program")
set(APP_INSTALL_DIR "" CACHE PATH "Copy the program here after each build (empty: don't)")
option(APP_FLAT_BINARY "Install the flat .bin instead of the ELF" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug, Release, MinSizeRel, RelWithDebInfo)" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)      # compile_commands.json for clangd / IDEs

#
# The libc: built from source into this build tree, without its examples
#
set(LIBC_BUILD_EXAMPLES OFF)
set(LIBC_BUILD_TESTS OFF)
add_subdirectory("${LIBC_DIR}" "${CMAKE_BINARY_DIR}/libc" EXCLUDE_FROM_ALL)

#
# The program: every .c / .S under src/ (new files are picked up on the next
# build). List them by hand instead if you prefer.
#
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.S")
if(NOT APP_SOURCES)
    message(FATAL_ERROR "No sources in ${CMAKE_CURRENT_SOURCE_DIR}/src (expected at least src/main.c)")
endif()

libc_add_program(${APP_NAME} ${APP_SOURCES})

target_include_directories(${APP_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(${APP_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra>)
target_compile_definitions(${APP_NAME} PRIVATE
    APP_NAME="${APP_NAME}"
    APP_VERSION="${PROJECT_VERSION}")
target_link_options(${APP_NAME} PRIVATE
    "-Wl,-Map=$<TARGET_FILE:${APP_NAME}>.map"
    -Wl,--gc-sections)
# Let --gc-sections drop unused functions and data
target_compile_options(${APP_NAME} PRIVATE -ffunction-sections -fdata-sections)

# Size report after each link (i686-elf-size next to a cross gcc, else size)
get_filename_component(_app_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
find_program(CMAKE_SIZE NAMES i686-elf-size size HINTS "${_app_cc_dir}")
if(CMAKE_SIZE)
    add_custom_command(TARGET ${APP_NAME} POST_BUILD
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${APP_NAME}>
        VERBATIM)
endif()

#
# Optional: copy the result to where the disk image is put together
#
if(APP_INSTALL_DIR)
    if(APP_FLAT_BINARY)
        set(_app_out "$<TARGET_FILE:${APP_NAME}>.bin")
    else()
        set(_app_out "$<TARGET_FILE:${APP_NAME}>")
    endif()
    add_custom_command(TARGET ${APP_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${APP_INSTALL_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "${_app_out}" "${APP_INSTALL_DIR}/"
        COMMENT "Copying ${APP_NAME} to ${APP_INSTALL_DIR}"
        VERBATIM)
endif()

message(STATUS "${APP_NAME}: libc ${LIBC_DIR}, image base ${LIBC_IMAGE_BASE}, ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")

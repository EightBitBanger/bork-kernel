# Host tests: the libc rebuilt with every syscall going through
# linux_shim.c, which emulates the kernel's syscalls on 32-bit Linux.
# Programs link exactly like real ones (crt0, user.ld, libc).

list(TRANSFORM _libc_sources PREPEND ${PROJECT_SOURCE_DIR}/ OUTPUT_VARIABLE _test_sources)
# Graphics: the kernel's own gfx code and command interpreter run inside the
# shim, so they need the kernel sources (default: the folder above libc/)
set(LIBC_KERNEL_SRC "${PROJECT_SOURCE_DIR}/.." CACHE PATH "Kernel source tree, for the graphics test")
set(_gfx_test OFF)
if(EXISTS "${LIBC_KERNEL_SRC}/gfx/gfx.h" AND EXISTS "${LIBC_KERNEL_SRC}/syscall/calls/sys_gfx_core.h")
    set(_gfx_test ON)
    # The kernel headers, laid out as <kernel/...>
    set(_kinc ${CMAKE_CURRENT_BINARY_DIR}/kinc/kernel)
    file(GLOB _kernel_gfx_headers "${LIBC_KERNEL_SRC}/gfx/*.h")
    file(COPY ${_kernel_gfx_headers} DESTINATION ${_kinc}/gfx)
    file(COPY "${LIBC_KERNEL_SRC}/syscall/calls/sys_gfx_core.h" DESTINATION ${_kinc}/syscall/calls)
    file(COPY "${LIBC_KERNEL_SRC}/arch/x86/drivers/display/char_rom.h" DESTINATION ${_kinc}/arch/x86/drivers/display)

    add_library(kgfx_parts OBJECT gfx_shim.c
        "${LIBC_KERNEL_SRC}/gfx/gfx_draw.c" "${LIBC_KERNEL_SRC}/gfx/gfx_surface.c" "${LIBC_KERNEL_SRC}/gfx/gfx_text.c")
    target_compile_definitions(kgfx_parts PRIVATE KERNEL_PLATFORM_X86)
    target_include_directories(kgfx_parts PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kstub
        ${CMAKE_CURRENT_BINARY_DIR}/kinc ${PROJECT_SOURCE_DIR}/include)
    target_compile_options(kgfx_parts PRIVATE ${_libc_common_options} ${_libc_build_options} -Wno-sign-compare)

    # The kernel's gfx_* share their names with the libc's: link the kernel
    # side into one object and keep only the shim's entry points global
    set(_kgfx ${CMAKE_CURRENT_BINARY_DIR}/kgfx.o)
    add_custom_command(OUTPUT ${_kgfx}
        COMMAND ${CMAKE_LINKER} -m elf_i386 -r $<TARGET_OBJECTS:kgfx_parts> -o ${_kgfx}
        COMMAND ${CMAKE_OBJCOPY} --wildcard "--keep-global-symbol=__test_gfx_*" ${_kgfx}
        DEPENDS kgfx_parts $<TARGET_OBJECTS:kgfx_parts>
        COMMAND_EXPAND_LISTS VERBATIM)
    set_source_files_properties(${_kgfx} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    list(APPEND _test_sources ${_kgfx})
else()
    message(STATUS "libc tests: kernel sources not found at ${LIBC_KERNEL_SRC}, graphics test skipped")
endif()

# IPC: the kernel's ipc/ipc.c runs inside the shim the same way
set(_ipc_test OFF)
if(EXISTS "${LIBC_KERNEL_SRC}/ipc/ipc.c")
    set(_ipc_test ON)
    set(_kinc ${CMAKE_CURRENT_BINARY_DIR}/kinc/kernel)
    file(GLOB _kernel_ipc_headers "${LIBC_KERNEL_SRC}/ipc/*.h")
    file(COPY ${_kernel_ipc_headers} DESTINATION ${_kinc}/ipc)

    add_library(kipc_parts OBJECT ipc_shim.c "${LIBC_KERNEL_SRC}/ipc/ipc.c")
    target_include_directories(kipc_parts PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kstub
        ${CMAKE_CURRENT_BINARY_DIR}/kinc ${PROJECT_SOURCE_DIR}/include)
    target_compile_options(kipc_parts PRIVATE ${_libc_common_options} ${_libc_build_options})
    # ipc.c's allocations are counted, so the test can check nothing leaks
    set_source_files_properties("${LIBC_KERNEL_SRC}/ipc/ipc.c" PROPERTIES
        COMPILE_DEFINITIONS "malloc=__test_ipc_malloc;free=__test_ipc_free")

    set(_kipc ${CMAKE_CURRENT_BINARY_DIR}/kipc.o)
    add_custom_command(OUTPUT ${_kipc}
        COMMAND ${CMAKE_LINKER} -m elf_i386 -r $<TARGET_OBJECTS:kipc_parts> -o ${_kipc}
        COMMAND ${CMAKE_OBJCOPY} --wildcard "--keep-global-symbol=__test_*" ${_kipc}
        DEPENDS kipc_parts $<TARGET_OBJECTS:kipc_parts>
        COMMAND_EXPAND_LISTS VERBATIM)
    set_source_files_properties(${_kipc} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    list(APPEND _test_sources ${_kipc})
else()
    message(STATUS "libc tests: ${LIBC_KERNEL_SRC}/ipc/ipc.c not found, IPC test skipped")
endif()

add_library(c_test STATIC ${_test_sources} linux_shim.c)
target_include_directories(c_test PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_options(c_test PRIVATE ${_libc_common_options} ${_libc_build_options})
target_compile_definitions(c_test PUBLIC LIBC_SYSCALL_HOOK=__test_syscall)

function(libc_add_test_program name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE c_test)
    target_compile_options(${name} PRIVATE ${_libc_common_options} -Wno-format)
    target_link_options(${name} PRIVATE ${_libc_link_options})
    target_sources(${name} PRIVATE $<TARGET_OBJECTS:libc_crt0>)
    add_dependencies(${name} libc_crt0)
    set_target_properties(${name} PROPERTIES LINK_DEPENDS ${LIBC_LINKER_SCRIPT})
endfunction()

libc_add_test_program(run_test run_test.c)
libc_add_test_program(fmt_cases fmt_cases.c)
if(_gfx_test)
    libc_add_test_program(gfx_test gfx_test.c)
    add_test(NAME graphics COMMAND gfx_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    libc_add_test_program(resource_test resource_test.c)
    add_test(NAME resources
        COMMAND sh -c "rm -rf res && \"$<TARGET_FILE:resource_test>\""
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(_ipc_test)
    libc_add_test_program(ipc_test ipc_test.c)
    add_test(NAME ipc COMMAND ipc_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# Functional tests, in an empty scratch directory
add_test(NAME functional
    COMMAND ${CMAKE_COMMAND} -E env sh -c
        "rm -rf sandbox && mkdir sandbox && cd sandbox && \"$<TARGET_FILE:run_test>\"")

# Formatting identical to the host C library: the same cases built natively
find_program(LIBC_HOST_CC NAMES cc gcc clang)
if(LIBC_HOST_CC)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fmt_cases_host
        COMMAND ${LIBC_HOST_CC} -O2 -w ${CMAKE_CURRENT_SOURCE_DIR}/fmt_cases.c -o fmt_cases_host
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fmt_cases.c
        COMMENT "Host reference build of fmt_cases"
        VERBATIM)
    add_custom_target(fmt_cases_host_build ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fmt_cases_host)

    add_test(NAME printf_matches_host
        COMMAND sh -c "\"$<TARGET_FILE:fmt_cases>\" > ours.txt && ./fmt_cases_host > host.txt && diff -u host.txt ours.txt")
endif()



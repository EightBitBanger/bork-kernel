# Brings the shared libc build up to date (cmake -P script, run by each
# program's build before it links):
#
#   cmake -DLIBC_DIR=<libc> -DBUILD_DIR=<dir> [-DGENERATOR=...]
#         [-DC_COMPILER=...] [-DBUILD_TYPE=...] -P build-shared.cmake
#
# Configures BUILD_DIR the first time, or again from scratch when the
# compiler or build type asked for differs from the one it was set up with,
# then builds it. An up-to-date libc builds nothing.

foreach(_var LIBC_DIR BUILD_DIR)
    if(NOT ${_var})
        message(FATAL_ERROR "build-shared.cmake: ${_var} is not set")
    endif()
endforeach()
if(NOT BUILD_TYPE)
    set(BUILD_TYPE Release)
endif()

# Programs built at the same time take turns
file(LOCK ${BUILD_DIR}.lock GUARD PROCESS TIMEOUT 600)

set(_cache ${BUILD_DIR}/CMakeCache.txt)
set(_configure ON)
if(EXISTS ${_cache})
    set(_configure OFF)
    # The libc's CMakeLists.txt writes the compiler it ended up with here
    set(_cached_cc "")
    if(EXISTS ${BUILD_DIR}/libc-compiler.txt)
        file(READ ${BUILD_DIR}/libc-compiler.txt _cached_cc)
    endif()
    file(STRINGS ${_cache} _cached_type REGEX "^CMAKE_BUILD_TYPE:")
    string(REGEX REPLACE "^[^=]*=" "" _cached_type "${_cached_type}")
    if(C_COMPILER AND NOT _cached_cc STREQUAL C_COMPILER)
        message(STATUS "libc: compiler changed (${_cached_cc} -> ${C_COMPILER}), reconfiguring")
        set(_configure ON)
    elseif(NOT _cached_type STREQUAL BUILD_TYPE)
        message(STATUS "libc: build type changed (${_cached_type} -> ${BUILD_TYPE}), reconfiguring")
        set(_configure ON)
    endif()
    if(_configure)
        file(REMOVE_RECURSE ${BUILD_DIR})
    endif()
endif()

if(_configure)
    set(_args -S ${LIBC_DIR} -B ${BUILD_DIR} -DCMAKE_BUILD_TYPE=${BUILD_TYPE})
    if(GENERATOR)
        list(APPEND _args -G ${GENERATOR})
    endif()
    if(C_COMPILER)
        list(APPEND _args -DCMAKE_C_COMPILER=${C_COMPILER})
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} ${_args} RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "libc: configuring ${BUILD_DIR} failed")
    endif()
endif()

execute_process(COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR} RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "libc: building ${BUILD_DIR} failed")
endif()

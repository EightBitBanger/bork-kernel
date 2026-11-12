#!/bin/bash
#
# Command-line build without Code::Blocks: kernel, asset archive and
# boot-disk.img, all through CMake (see CMakeLists.txt). The CMake files
# go in build/, the outputs in bin/ and the project root as usual.
#
#   ./build.sh
#
set -e

cd "$(dirname "$0")"

cmake -S . -B build
cmake --build build

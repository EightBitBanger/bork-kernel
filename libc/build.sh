# Build the shared libc (build/libc.a + build/crt0.o) that every program
# links. Only libc files that changed are recompiled; programs also run
# this check themselves before they link, so this script is optional.
cmake -DLIBC_DIR=. -DBUILD_DIR=build -DGENERATOR="Unix Makefiles" -P cmake/build-shared.cmake

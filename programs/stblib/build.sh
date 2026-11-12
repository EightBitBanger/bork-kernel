# Build the stb library: stblib/stb, installed as <home>/sys/lib/stb
# (assets.txt). Its abi is STBLIB_ABI in libc/include/os/stblib.h.
ABI=$(sed -n 's/^#define STBLIB_ABI[[:space:]]*\([0-9]*\).*/\1/p' ../../libc/include/os/stblib.h)
bash ../../libc/ulib/build-ulib.sh ./stb stb "$ABI" src/stblib.c

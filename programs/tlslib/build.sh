# Build the tls library: tlslib/tls, installed as <home>/sys/lib/tls
# (assets.txt). Its abi is TLSLIB_ABI in libc/include/os/tlslib.h.
#
# BearSSL (bearssl/, MIT licence, LICENSE.txt there) is built as plain C:
# no AES-NI, SSE2 or RDRAND code, no /dev/urandom or time(), so nothing in
# it needs the OS or more than an i686.
ABI=$(sed -n 's/^#define TLSLIB_ABI[[:space:]]*\([0-9]*\).*/\1/p' ../../libc/include/os/tlslib.h)
bash ../../libc/ulib/build-ulib.sh ./tls tls "$ABI" src/tlslib.c src/roots.c $(find bearssl/src -name '*.c' | sort) -- \
    -Ibearssl/inc -Ibearssl/src \
    -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_RDRAND=0 -DBR_POWER8=0 \
    -DBR_USE_URANDOM=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 \
    -DBR_64=0 -DBR_LE_UNALIGNED=1 \
    -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough

/*
 * TinyCC configuration for programs on the kernel. Replaces the config.h
 * that tinycc's ./configure writes (build.sh copies it into the checkout).
 *
 * tcc looks for its files under <home>/sys/tcc at run time:
 *
 *   include/           tcc's own headers (stddef.h, stdarg.h, float.h, ...)
 *   libc/include/      this libc's headers
 *   lib/crt1.o         libc crt0
 *   lib/crti.o crtn.o  empty, tcc links them by name
 *   lib/libc.a
 *   lib/libtcc1.a      tcc's runtime helpers
 *   lib/runmain.o      for tcc -run
 *
 * -B<dir> overrides the location.
 */

#define TCC_TARGET_I386         1

/* No dynamic linking: tcc's own dlopen/dlsym are stubs, and every program
   it writes is static, linked where the loader wants it (user.ld) */
#define CONFIG_TCC_STATIC       1
#define CONFIG_TCC_SWITCHES     "-static -Wl,-Ttext=0x40000000"

/* No threads, signals or debug-info walk on this kernel */
#define CONFIG_TCC_SEMLOCK      0
#define CONFIG_TCC_BACKTRACE    0
#define CONFIG_TCC_BCHECK       0

char* os_syspath(const char* sub, char* out, __SIZE_TYPE__ size);
static inline const char* kernel_tccdir(void) {
    static char dir[512];
    return os_syspath("tcc", dir, sizeof(dir)) ? dir : "/sys/tcc";
}
#define CONFIG_TCCDIR               kernel_tccdir()
#define CONFIG_TCC_SYSINCLUDEPATHS  "{B}/include:{B}/libc/include"
#define CONFIG_TCC_LIBPATHS         "{B}/lib"
#define CONFIG_TCC_CRTPREFIX        "{B}/lib"

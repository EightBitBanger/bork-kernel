/*
 * Program start and end: __libc_start (called by crt0), exit, atexit.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <os/os.h>
#include "internal.h"

#define ATEXIT_MAX  32

extern int main(int argc, char** argv, char** envp);

// Provided by the linker script
typedef void (*init_fn)(void);
extern init_fn __init_array_start[] __attribute__((weak));
extern init_fn __init_array_end[]   __attribute__((weak));
extern init_fn __fini_array_start[] __attribute__((weak));
extern init_fn __fini_array_end[]   __attribute__((weak));

static void (*atexit_fns[ATEXIT_MAX])(void);
static int atexit_count = 0;

/*
 * Arguments come from the kernel on the initial stack (crt0 passes where):
 * argc, then argv[0..argc-1] and a NULL. A kernel that predates this leaves
 * zeroed stack there, which reads as argc 0. Anything that does not look
 * like a valid block is ignored rather than handed to main().
 */
static char* empty_argv[] = { NULL };

// argv[0] for libc's own use (the name audio_open gives the sound mixer)
char* __libc_argv0 = NULL;

// HOME=<home> (os_gethome), so getenv("HOME") works in ported code
static char  home_env[5 + SYS_PATH_MAX] = "HOME=";
static char* start_environ[2] = { NULL, NULL };

__attribute__((noreturn)) void __libc_start(uint32_t* block) {
    int argc = 0;
    char** argv = empty_argv;
    if (block != NULL && block[0] > 0 && block[0] <= SYS_ARG_MAX && block[1 + block[0]] == 0) {
        argc = (int)block[0];
        argv = (char**)&block[1];
    }
    __libc_argv0 = argc > 0 ? argv[0] : NULL;

    // Before constructors, so they see it too. No home device: no HOME.
    if (os_gethome(home_env + 5, SYS_PATH_MAX) != NULL) {
        start_environ[0] = home_env;
        environ = start_environ;
    }
    errno = 0;

    for (init_fn* f = __init_array_start; f && f < __init_array_end; f++)
        (*f)();

    exit(main(argc, argv, environ));
}

int atexit(void (*fn)(void)) {
    if (atexit_count >= ATEXIT_MAX)
        return -1;
    atexit_fns[atexit_count++] = fn;
    return 0;
}

void exit(int status) {
    while (atexit_count > 0)
        atexit_fns[--atexit_count]();

    for (init_fn* f = __fini_array_end; f && f > __fini_array_start; )
        (*--f)();

    __stdio_exit();
    _exit(status);
}

char* getenv(const char* name) {
    size_t len = strlen(name);
    for (char** e = environ; e && *e; e++) {
        if (strncmp(*e, name, len) == 0 && (*e)[len] == '=')
            return *e + len + 1;
    }
    return NULL;
}



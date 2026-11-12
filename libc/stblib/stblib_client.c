/*
 * The program's side of stblib (os/stblib.h): the stb functions, each one
 * forwarding to the library <home>/sys/lib/stb, which is loaded the first
 * time any of them is called.
 *
 * Not part of libc.a. libc_use_stblib(<program>) in a program's
 * CMakeLists.txt compiles this file into the program, and since a
 * program's own objects come before libc.a on the link line, these
 * definitions are the ones libc's image.c, image_png.c and font.c get:
 * libc.a's own stb objects (stbimage.o, stbimagewrite.o, stbtruetype.o) are
 * then never pulled in.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <os/os.h>
#include <os/stblib.h>
#include <os/ulib.h>

// The real stb declarations, so the compiler checks every forwarder below
// against them: a mismatch in os/stblib.h is a build error here
#include "../src/stb_image.h"
#define STBI_WRITE_NO_STDIO
#include "../src/stb_image_write.h"

static const stblib_api_t* api;     // Set once, by the first call that loads it
static int                 tried;   // Loading failed: don't read the file again
static char                why[160];

const char* stblib_error(void) {
    return why;
}

const stblib_api_t* stblib(void) {
    const stblib_api_t* a = __atomic_load_n(&api, __ATOMIC_ACQUIRE);
    if (a != NULL || __atomic_load_n(&tried, __ATOMIC_ACQUIRE))
        return a;

    int err = errno;
    char path[256];
    ulib_t* lib = NULL;
    if (os_syspath(STBLIB_SYSPATH, path, sizeof(path)) == NULL) {
        snprintf(why, sizeof(why), "stb library: no home folder to find sys/" STBLIB_SYSPATH " in");
    } else if ((lib = ulib_open(path, STBLIB_NAME, STBLIB_ABI)) == NULL) {
        snprintf(why, sizeof(why), "stb library %s: %s", path, ulib_error());
    } else {
        const stblib_api_t* t = (const stblib_api_t*)ulib_exports(lib);
        if (t->size < offsetof(stblib_api_t, stbi_load_from_memory) + sizeof(void*)) {
            snprintf(why, sizeof(why), "stb library %s: its function table is empty", path);
            ulib_close(lib);
            lib = NULL;
        } else {
            const stblib_api_t* expected = NULL;
            if (!__atomic_compare_exchange_n(&api, &expected, t, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                ulib_close(lib);    // Another thread loaded it first: use that one
            // The library stays loaded for the rest of the program's life
        }
    }
    if (lib == NULL) {
        __atomic_store_n(&tried, 1, __ATOMIC_RELEASE);
        // Once, for whoever reads the program's error output: images and
        // fonts will be missing from it and this says why
        fprintf(stderr, "%s\n", why);
    }
    errno = err;
    return __atomic_load_n(&api, __ATOMIC_ACQUIRE);
}

// The library's table entry for `name`, or NULL when the library isn't
// there or is older than this program and doesn't have it yet
#define ENTRY(name) \
    const stblib_api_t* a = stblib(); \
    if (a == NULL || a->size < offsetof(stblib_api_t, name) + sizeof(a->name) || a->name == NULL)

#define FORWARD(ret, name, params, args, fail) \
    ret name params { \
        ENTRY(name) \
            return fail; \
        return a->name args; \
    }

#define FORWARD_V(name, params, args) \
    void name params { \
        ENTRY(name) \
            return; \
        a->name args; \
    }

STBLIB_FUNCTIONS(FORWARD, FORWARD_V)

#ifndef _OS_WM_APP_H
#define _OS_WM_APP_H

/*
 * For programs that only make sense on the desktop. Include this header
 * (in place of, or as well as, os/wm.h) and the program connects to the dwm
 * before main() runs:
 *
 *     #include <os/wm_app.h>
 *
 *     int main(int argc, char** argv) {
 *         wm_window_t win = wm_window_create(&desc);     // already connected
 *         ...
 *
 * Started from the kernel console with no dwm, it prints
 *
 *     This program cannot be run in console mode.
 *
 * and exits with status 1; main() never runs. Any other reason the dwm
 * can't be reached (EBUSY: it has no room, EPERM: no SEC_IPC, ENOTSUP: a
 * different protocol version) is printed the same way, with its errno.
 *
 * The check is a constructor, so the compiler puts it in .init_array and
 * __libc_start runs it before main(). Programs that don't include this
 * header (console tools, the dwm itself) never run it. Calling wm_connect()
 * again in main() is harmless: it returns 0 at once.
 *
 * Including it from several .c files is fine. The function is weak, so the
 * linker keeps one copy, but every such file still adds an .init_array
 * entry for it, so it remembers that it already ran.
 *
 * wm_connect(0) makes one attempt and doesn't wait for "sys.dwm" to show
 * up: the dwm creates its port before it starts any program, so a program
 * it started always finds it.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/wm.h>

#define WM_APP_CONSOLE_MESSAGE  "This program cannot be run in console mode.\n"

__attribute__((constructor, weak)) void __wm_app_require_dwm(void) {
    static bool checked = false;
    if (checked)
        return;
    checked = true;

    if (wm_connect(0) == 0)
        return;
    if (errno == ENOENT)
        fputs(WM_APP_CONSOLE_MESSAGE, stdout);
    else
        printf("This program needs the desktop, which can't be reached: %s\n", strerror(errno));
    exit(1);        // Not return: main() would run next
}

#endif

#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

/*
 * Device control. Only terminals answer: a program started on one
 * (spawn_tty(), e.g. from the prompt) can ask it about descriptors 0, 1
 * and 2. Anything else fails with ENOTTY.
 *
 *     struct winsize ws;
 *     if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
 *         columns = ws.ws_col;
 *
 * Request numbers are Linux's.
 */

#include <os/syscall.h>

#define TIOCGWINSZ  SYS_TIOCGWINSZ      // struct winsize*: the terminal's size
#define FIONREAD    SYS_FIONREAD        // int*: bytes stdin can read without blocking

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;           // Always 0
    unsigned short ws_ypixel;           // Always 0
};

int ioctl(int fd, unsigned long request, ...);

#endif

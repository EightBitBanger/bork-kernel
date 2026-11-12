/*
 * abort() and assert() failures. Apart from start.c, so code that can only
 * abort (malloc) does not pull in exit/atexit: a host that brings its own,
 * like tcc -run's runmain.o, links without duplicate symbols.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void abort(void) {
    static const char msg[] = "abort() called\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(134);     // 128 + SIGABRT, as a shell would report it
}

void __assert_fail(const char* expr, const char* file, int line, const char* func) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s:%d: %s: Assertion `%s' failed.\n", file, line, func, expr);
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    if (n > 0) write(2, buf, (size_t)n);
    abort();
}

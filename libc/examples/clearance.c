/*
 * Security clearance demo: show this process's mask, and if it is an
 * administrator, give up file writing and then administration for good
 * (the usual "drop what you don't need before doing the work" pattern).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <os/security.h>

static void show(const char* when) {
    char text[96];
    sec_format(sec_get(), text, sizeof(text));
    printf("%-14s %s\n", when, text);
}

int main(void) {
    printf("pid %d\n", getpid());
    show("clearance:");

    if (!sec_is_admin()) {
        // Without SEC_ADMIN a bit can be dropped, never taken back
        if (sec_drop(SEC_FILE_WRITE) < 0) {
            perror("sec_drop");
            return 1;
        }
        show("no writing:");
        printf("grant it back: %s\n", sec_grant(0, SEC_FILE_WRITE) < 0 ? strerror(errno) : "allowed?!");
        return 0;
    }

    if (sec_drop(SEC_FILE_WRITE) < 0) {
        perror("sec_drop");
        return 1;
    }
    show("no writing:");

    FILE* f = fopen("clearance.txt", "w");
    printf("fopen for writing: %s\n", f ? "allowed?!" : strerror(errno));
    if (f) fclose(f);

    sec_drop(SEC_ADMIN);
    show("no admin:");
    printf("grant it back: %s\n", sec_grant(0, SEC_FILE_WRITE) < 0 ? strerror(errno) : "allowed?!");
    return 0;
}


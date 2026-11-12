/*
 * Starts another program with arguments (os/process.h).
 *
 *     spawn_demo            starts "args" from the working directory
 *     spawn_demo <program>  starts <program> instead
 *
 * Both files must be marked executable on the device.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <os/process.h>
#include <os/security.h>

static void report(const char* what, pid_t pid) {
    if (pid < 0)
        printf("%-28s failed: %s\n", what, strerror(errno));
    else
        printf("%-28s pid %d\n", what, (int)pid);
}

int main(int argc, char** argv) {
    const char* target = (argc > 1) ? argv[1] : "args";

    char mask[96];
    sec_format(sec_get(), mask, sizeof(mask));
    printf("spawn_demo: clearance %s\n", mask);

    // Same clearance, working directory and priority as this program
    char* args[] = { (char*)target, "hello", "two words", "3", NULL };
    report("spawn", spawn(target, args));

    // Listed in the call
    report("spawnl", spawnl(target, target, "from", "spawnl", NULL));

    // A child with less clearance: console and reading files only
    report("spawn_ex (console|file_read)",
           spawn_ex(target, args, NULL, SEC_CONSOLE | SEC_FILE_READ));

    // Clearance this process doesn't hold is refused (EPERM)
    if (!sec_is_admin())
        report("spawn_ex (+admin)", spawn_ex(target, args, NULL, sec_get() | SEC_ADMIN));

    // No such file (ENOENT)
    report("spawn missing", spawnl("no_such_program", "no_such_program", NULL));
    return 0;
}

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <os/driver.h>
#include <os/security.h>

//
// loadlib: load and unload drivers from the console
//
//   loadlib <file>          load a driver file and start it
//   loadlib -u <driver>     stop a driver loadlib (or the kernel console) loaded
//   loadlib -c <file>       say whether a file is a driver file, load nothing
//
// Loading and unloading need admin: without it, loadlib asks the user
// (sec_elevate) and gives admin back when it is done.
//

static int usage(void) {
    fprintf(stderr, "usage: loadlib <driver file>\n"
                    "       loadlib -u <driver name>\n"
                    "       loadlib -c <file>\n");
    return 2;
}

static const char* load_error(int error) {
    switch (error) {
        case ENOEXEC: return "not a driver file, or one built for another kernel";
        case EEXIST:  return "a driver of that name is loaded already";
        case ENOSPC:  return "too many drivers loaded";
        case EIO:     return "the driver would not start (see COM1)";
        default:      return strerror(error);
    }
}

// Admin for the call that needs it: 0 once we hold it, -1 (said why) if we
// can't. *elevated is set when we had to ask, so the caller gives it back.
static int get_admin(const char* reason, int* elevated) {
    *elevated = 0;
    if (sec_is_admin())
        return 0;
    if (sec_elevate(reason) < 0) {
        switch (errno) {
            case EACCES:    break;      // Cancel at the prompt: the user just chose it
            case ETIMEDOUT: fprintf(stderr, "loadlib: nobody answered the admin prompt\n"); break;
            case EAGAIN:    fprintf(stderr, "loadlib: needs admin, and no desktop is running to ask\n"); break;
            case EPERM:     fprintf(stderr, "loadlib: needs admin, and may not ask for it\n"); break;
            default:        fprintf(stderr, "loadlib: admin: %s\n", strerror(errno)); break;
        }
        return -1;
    }
    *elevated = 1;
    return 0;
}

static int check(const char* path) {
    char name[DRV_NAME_MAX];
    int result = drv_check(path, name);
    if (result < 0) {
        fprintf(stderr, "loadlib: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (result == 0) {
        printf("%s: not a driver file\n", path);
        return 1;
    }
    printf("%s: driver file, driver \"%s\"\n", path, name);
    return 0;
}

static int load(const char* path) {
    // Refuse plain files before bothering the user with a prompt
    char name[DRV_NAME_MAX];
    int is_driver = drv_check(path, name);
    if (is_driver < 0) {
        fprintf(stderr, "loadlib: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (is_driver == 0) {
        fprintf(stderr, "loadlib: %s: not a driver file\n", path);
        return 1;
    }

    char reason[96];
    snprintf(reason, sizeof(reason), "Load the driver \"%s\" into the kernel", name);
    int elevated;
    if (get_admin(reason, &elevated) < 0)
        return 1;

    drv_info_t info;
    int result = LoadLibrary(path, &info);
    int error = errno;
    if (elevated)
        sec_drop(SEC_ADMIN);

    if (result < 0) {
        fprintf(stderr, "loadlib: %s: %s\n", path, load_error(error));
        return 1;
    }
    printf("Loaded %s%s%s (%u KB), %u device%s\n", info.name,
           info.version[0] ? " " : "", info.version,
           (unsigned)((info.image_size + 1023) / 1024),
           (unsigned)info.devices, info.devices == 1 ? "" : "s");
    return 0;
}

static int unload(const char* name) {
    char reason[96];
    snprintf(reason, sizeof(reason), "Unload the driver \"%s\"", name);
    int elevated;
    if (get_admin(reason, &elevated) < 0)
        return 1;

    int result = FreeLibrary(name);
    int error = errno;
    if (elevated)
        sec_drop(SEC_ADMIN);

    if (result < 0) {
        if (error == ENOENT)
            fprintf(stderr, "loadlib: %s: no loaded driver of that name\n", name);
        else if (error == EACCES)
            fprintf(stderr, "loadlib: %s: built into the kernel, can't be unloaded\n", name);
        else
            fprintf(stderr, "loadlib: %s: %s\n", name, strerror(error));
        return 1;
    }
    printf("Unloaded %s\n", name);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && argv[1][0] != '-')
        return load(argv[1]);
    if (argc == 3 && strcmp(argv[1], "-u") == 0)
        return unload(argv[2]);
    if (argc == 3 && strcmp(argv[1], "-c") == 0)
        return check(argv[2]);
    return usage();
}

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/driver.h>
#include <os/security.h>
#include <os/syscall.h>

//
// rundrv: call a library driver's functions from the command line
//
//   rundrv                               list the drivers and their types
//   rundrv <driver>                      list a library's functions
//   rundrv <driver> <function> [arg...]  call one, print what it answers
//
// <driver> is a loaded driver's name ("demolib"), or a driver file
// ("/mnt/ata0p1/sys/drivers/demolib", anything with a '/'), which is loaded
// first if it isn't yet (that needs admin: rundrv asks).
//
// Arguments are converted by the function's parameters: a number for
// <int> (42, -7, 0x2A), text as it is for <text> (quote it if it has
// spaces). rundrv prints the function's text answer if it gives one, else
// the number it returned.
//
//   -n   print the number it returned, even when there is text
//   -q   print nothing; the exit status is 0 if the call ran
//

#define DRV_ROOT    "/drv"
#define PATH_LEN    SYS_PATH_MAX

static bool opt_number;
static bool opt_quiet;

static int usage(void) {
    fprintf(stderr, "usage: rundrv                               list drivers\n"
                    "       rundrv <driver>                      list a library's functions\n"
                    "       rundrv [-n|-q] <driver> <function> [argument ...]\n"
                    "<driver> is a loaded driver's name or a driver file to load first\n");
    return 2;
}

// The value of /drv/<driver>/<property> (a folder holding one entry)
static bool drv_property(const char* driver, const char* property, char* out, size_t size) {
    char path[PATH_LEN];
    if ((size_t)snprintf(path, sizeof(path), DRV_ROOT "/%s/%s", driver, property) >= sizeof(path))
        return false;
    DIR* dir = opendir(path);
    if (dir == NULL)
        return false;
    struct dirent* e = readdir(dir);
    bool found = e != NULL;
    if (found)
        snprintf(out, size, "%s", e->d_name);
    closedir(dir);
    return found;
}

static bool driver_loaded(const char* driver) {
    char type[24];
    return drv_property(driver, "type", type, sizeof(type)) ||
           drv_property(driver, "version", type, sizeof(type));
}

static const char* param_name(char letter) {
    switch (letter) {
        case SYS_DRV_ARG_INT:    return "<int>";
        case SYS_DRV_ARG_STRING: return "<text>";
        default:                 return "<?>";
    }
}

// "add <int> <int>" into out
static void signature(const drv_export_t* e, char* out, size_t size) {
    size_t used = (size_t)snprintf(out, size, "%s", e->name);
    for (const char* p = e->params; *p != '\0' && used < size; p++)
        used += (size_t)snprintf(out + used, size - used, " %s", param_name(*p));
}

//
// Admin, the way loadlib asks for it
//

static int get_admin(const char* reason, int* elevated) {
    *elevated = 0;
    if (sec_is_admin())
        return 0;
    if (sec_elevate(reason) < 0) {
        switch (errno) {
            case EACCES:    break;      // Cancel at the prompt: the user just chose it
            case ETIMEDOUT: fprintf(stderr, "rundrv: nobody answered the admin prompt\n"); break;
            case EAGAIN:    fprintf(stderr, "rundrv: needs admin, and no desktop is running to ask\n"); break;
            case EPERM:     fprintf(stderr, "rundrv: needs admin, and may not ask for it\n"); break;
            default:        fprintf(stderr, "rundrv: admin: %s\n", strerror(errno)); break;
        }
        return -1;
    }
    *elevated = 1;
    return 0;
}

// <driver> as given: a name, or a file loaded now if its driver isn't.
// The driver's name into `name`. 0, or -1 (said why).
static int resolve_driver(const char* given, char* name, size_t size) {
    if (strchr(given, '/') == NULL) {
        if (strlen(given) >= size) {
            fprintf(stderr, "rundrv: no driver \"%s\" is loaded (driver names are at most %u letters)\n",
                    given, (unsigned)size - 1);
            return -1;
        }
        snprintf(name, size, "%s", given);
        return 0;
    }

    char header_name[DRV_NAME_MAX];
    int is_driver = drv_check(given, header_name);
    if (is_driver < 0) {
        fprintf(stderr, "rundrv: %s: %s\n", given, strerror(errno));
        return -1;
    }
    if (is_driver == 0) {
        fprintf(stderr, "rundrv: %s: not a driver file\n", given);
        return -1;
    }
    snprintf(name, size, "%s", header_name);
    if (driver_loaded(name))
        return 0;

    char reason[96];
    snprintf(reason, sizeof(reason), "Load the driver \"%s\" into the kernel", name);
    int elevated;
    if (get_admin(reason, &elevated) < 0)
        return -1;
    drv_info_t info;
    int result = LoadLibrary(given, &info);
    int error = errno;
    if (elevated)
        sec_drop(SEC_ADMIN);
    if (result < 0 && error != EEXIST) {
        fprintf(stderr, "rundrv: loading %s: %s\n", given,
                error == ENOEXEC ? "not a driver file, or one built for another kernel" : strerror(error));
        return -1;
    }
    if (!opt_quiet && result == 0)
        printf("Loaded %s %s\n", info.name, info.version);
    return 0;
}

//
// Listings
//

static int list_drivers(void) {
    DIR* dir = opendir(DRV_ROOT);
    if (dir == NULL) {
        fprintf(stderr, "rundrv: " DRV_ROOT ": %s\n", strerror(errno));
        return 1;
    }
    printf("%-16s %-8s %-8s %s\n", "Driver", "Type", "Version", "Origin");
    struct dirent* e;
    int libraries = 0;
    while ((e = readdir(dir)) != NULL) {
        char type[16] = "device", version[24] = "-", origin[16] = "-";
        drv_property(e->d_name, "type", type, sizeof(type));
        drv_property(e->d_name, "version", version, sizeof(version));
        drv_property(e->d_name, "origin", origin, sizeof(origin));
        printf("%-16s %-8s %-8s %s\n", e->d_name, type, version, origin);
        if (strcmp(type, "library") == 0)
            libraries++;
    }
    closedir(dir);
    if (libraries == 0)
        printf("No library drivers loaded (try: loadlib <home>/sys/drivers/demolib)\n");
    else
        printf("\"rundrv <driver>\" lists a library's functions\n");
    return 0;
}

static void say_call_error(const char* driver, const char* function, int error) {
    switch (error) {
        case ENOENT:
            if (!driver_loaded(driver))
                fprintf(stderr, "rundrv: no driver \"%s\" is loaded (load it with loadlib, or give its file)\n", driver);
            else
                fprintf(stderr, "rundrv: %s has no function \"%s\" (\"rundrv %s\" lists them)\n",
                        driver, function, driver);
            break;
        case ENOEXEC:
            fprintf(stderr, "rundrv: %s is not a library driver: it exports no functions\n", driver);
            break;
        case EPERM:
            fprintf(stderr, "rundrv: %s %s: this program lacks the rights the function needs\n", driver, function);
            break;
        default:
            fprintf(stderr, "rundrv: %s %s: %s\n", driver, function, strerror(error));
            break;
    }
}

static int list_functions(const char* driver) {
    drv_export_t e;
    int got = drv_exports(driver, 0, &e);
    if (got < 0) {
        say_call_error(driver, "", errno);
        return 1;
    }

    char version[24] = "", origin[16] = "";
    drv_property(driver, "version", version, sizeof(version));
    drv_property(driver, "origin", origin, sizeof(origin));
    printf("%s %s, library driver (%s)\n", driver, version, origin);

    for (unsigned i = 0; got > 0; got = drv_exports(driver, ++i, &e)) {
        char sig[96];
        signature(&e, sig, sizeof(sig));
        printf("  %-28s %s%s\n", sig, e.help, (e.security & SEC_ADMIN) ? " [admin]" : "");
    }
    printf("Call one: rundrv %s <function> [argument ...]\n", driver);
    return 0;
}

//
// A call
//

static int call(const char* driver, const char* function, int argc, char** argv) {
    drv_export_t e;
    int found = drv_find_export(driver, function, &e);
    if (found <= 0) {
        say_call_error(driver, function, found < 0 ? errno : ENOENT);
        return 1;
    }

    char sig[96];
    signature(&e, sig, sizeof(sig));
    if ((size_t)argc != strlen(e.params)) {
        fprintf(stderr, "usage: rundrv %s %s\n", driver, sig);
        return 2;
    }

    // Admin-only functions: ask, as for a load
    int elevated = 0;
    if ((e.security & SEC_ADMIN) != 0) {
        char reason[96];
        snprintf(reason, sizeof(reason), "Run %s's function \"%s\"", driver, function);
        if (get_admin(reason, &elevated) < 0)
            return 1;
    }

    static char out[DRV_OUTPUT_MAX];
    int32_t result = 0;
    int r = drv_call_strings(driver, function, argc, argv, &result, out, sizeof(out));
    int error = errno;
    if (elevated)
        sec_drop(SEC_ADMIN);

    if (r < 0) {
        if (error == EINVAL) {
            // drv_call_strings: a number that isn't one
            for (int i = 0; i < argc; i++) {
                char* end;
                if (e.params[i] == SYS_DRV_ARG_INT && (strtoll(argv[i], &end, 0), *end != '\0' || end == argv[i]))
                    fprintf(stderr, "rundrv: argument %d (\"%s\") is not a number\n", i + 1, argv[i]);
            }
            fprintf(stderr, "usage: rundrv %s %s\n", driver, sig);
            return 2;
        }
        say_call_error(driver, function, error);
        return 1;
    }

    if (opt_quiet)
        return 0;
    if (out[0] != '\0') {
        size_t length = strlen(out);
        printf("%s%s", out, out[length - 1] == '\n' ? "" : "\n");
        if (opt_number)
            printf("%ld\n", (long)result);
    } else {
        printf("%ld\n", (long)result);
    }
    return 0;
}

int main(int argc, char** argv) {
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1] != '\0'; first++) {
        if (strcmp(argv[first], "-n") == 0)
            opt_number = true;
        else if (strcmp(argv[first], "-q") == 0)
            opt_quiet = true;
        else if (strcmp(argv[first], "--") == 0) {
            first++;
            break;
        } else
            return usage();
    }

    int left = argc - first;
    if (left == 0)
        return list_drivers();

    char driver[DRV_NAME_MAX];
    if (resolve_driver(argv[first], driver, sizeof(driver)) < 0)
        return 1;
    if (left == 1)
        return list_functions(driver);
    return call(driver, argv[first + 1], left - 2, argv + first + 2);
}

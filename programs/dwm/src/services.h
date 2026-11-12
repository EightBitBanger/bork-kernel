#ifndef DESKTOP_SERVICES_H
#define DESKTOP_SERVICES_H

/*
 * Services the desktop starts, from the registry.
 *
 * One string value per service under SERVICES_KEY: the value's name names
 * the service, its data is the program's path below the home device:
 *
 *     hkey_root/system/services   netsrv "sys/services/netsrv"
 *
 * They start in the order of their names. A path is always under home: a
 * leading '/' is dropped, and one with a ".." in it is refused, so an entry
 * can't reach a stick in /mnt or climb out of the system folders.
 *
 * Who starts at login is a way in for anything that wants to run unasked,
 * so the key lives in hkey_root: anyone may read it, but adding, changing
 * or removing a service takes admin (the registry's rule for hkey_root,
 * kernel/syscall/calls/sys_registry.h; regedit asks to elevate).
 *
 * "boot install" and "boot repair" write the default line
 * (kernel/console/commands/boot_registry.h); the desktop has the same line
 * built in (SERVICES_DEFAULTS) for a registry without the key.
 */

#include <stddef.h>
#include <limits.h>
#include <os/registry.h>

#define SERVICES_KEY        "hkey_root/system/services"
#define SERVICES_MAX        16          // Started at most; more are left out

typedef struct {
    char name[OS_REG_NAME_MAX];         // The value's name ("netsrv")
    char path[PATH_MAX];                // As stored: below home ("sys/services/netsrv")
} services_entry_t;

// The services in the registry, at most `max`, sorted by name. A value
// that isn't a usable path is left out. Returns how many, or -1 with errno:
// ENOENT when the key isn't there (an install from before it), anything
// else when it can't be read.
int services_read(services_entry_t* out, size_t max);

// "<home>/<path>" into out, for a path as services_read gives it. NULL with
// errno: EINVAL (empty, or a ".." in it), ENAMETOOLONG, or os_gethome's.
char* services_resolve(const char* path, char* out, size_t size);

#endif

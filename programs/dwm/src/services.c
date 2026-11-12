/*
 * Services the desktop starts, from the registry (services.h).
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/os.h>
#include <os/registry.h>

#include "services.h"

static int compare_entries(const void* a, const void* b) {
    return strcmp(((const services_entry_t*)a)->name, ((const services_entry_t*)b)->name);
}

// A ".." anywhere as a whole path segment
static bool climbs(const char* path) {
    for (const char* p = path; (p = strstr(p, "..")) != NULL; p += 2) {
        bool starts = (p == path || p[-1] == '/');
        bool ends   = (p[2] == '\0' || p[2] == '/');
        if (starts && ends)
            return true;
    }
    return false;
}

char* services_resolve(const char* path, char* out, size_t size) {
    while (*path == '/')
        path++;
    if (*path == '\0' || climbs(path)) {
        errno = EINVAL;
        return NULL;
    }
    char home[PATH_MAX];
    if (os_gethome(home, sizeof(home)) == NULL)
        return NULL;
    int n = snprintf(out, size, "%s/%s", home, path);
    if (n < 0 || (size_t)n >= size) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return out;
}

int services_read(services_entry_t* out, size_t max) {
    size_t count = 0;
    for (unsigned i = 0; count < max; i++) {
        services_entry_t* e = &out[count];
        int len = os_reg_value(SERVICES_KEY, i, e->name, sizeof(e->name));
        if (len < 0) {
            if (i == 0)
                return -1;      // No key (ENOENT), or not readable
            break;
        }
        if (len == 0)
            break;
        // Not a string, too long, unreadable or empty: not a service
        if (os_reg_get_string(SERVICES_KEY, e->name, e->path, sizeof(e->path)) < 0 ||
            e->path[0] == '\0')
            continue;
        count++;
    }
    if (count > 1)
        qsort(out, count, sizeof(out[0]), compare_entries);
    return (int)count;
}

/*
 * Desktop context menus from the registry (menus.h).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/registry.h>

#include "menus.h"

static int compare_names(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

int menus_read(const char* name, menus_line_t* out, size_t max) {
    char key[128];
    int n = snprintf(key, sizeof(key), "%s/%s", MENUS_KEY, name);
    if (n < 0 || (size_t)n >= sizeof(key)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    // The lines' key names, sorted: the hive keeps them in no useful order
    char names[DWM_MENU_MAX_ITEMS][OS_REG_NAME_MAX];
    size_t count = 0;
    for (unsigned i = 0; count < DWM_MENU_MAX_ITEMS; i++) {
        int len = os_reg_key(key, i, names[count], sizeof(names[count]));
        if (len < 0) {
            if (i == 0)
                return -1;      // No such menu
            break;
        }
        if (len == 0)
            break;
        count++;
    }
    if (count > 1)
        qsort(names, count, sizeof(names[0]), compare_names);

    size_t at = 0;
    for (size_t i = 0; i < count && at < max; i++) {
        char line[192];
        n = snprintf(line, sizeof(line), "%s/%s", key, names[i]);
        if (n < 0 || (size_t)n >= sizeof(line))
            continue;
        menus_line_t* l = &out[at];
        if (os_reg_get_string(line, "command", l->command, sizeof(l->command)) < 0 ||
            l->command[0] == '\0')
            continue;
        // A long label is cut, not lost
        char label[256];
        if (os_reg_get_string(line, "label", label, sizeof(label)) < 0)
            label[0] = '\0';
        strlcpy(l->label, label, sizeof(l->label));
        at++;
    }
    return (int)at;
}

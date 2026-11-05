#ifndef CONSOLE_CMD_LS_H
#define CONSOLE_CMD_LS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <os/os.h>
#include "console_host.h"
#include "console_const.h"
#include "path_util.h"

// Width of a listing line: the size or <DIR> is right-aligned here
#define CONSOLE_LIST_WIDTH      48

// Print one listing line: permissions, the name, then `right` (may be "")
// right-aligned at CONSOLE_LIST_WIDTH. A name too long for the window is
// shortened with "...".
static void console_print_listing_line(const char* permissions, const char* name, const char* right) {
    size_t cols      = console_columns();
    size_t width     = (CONSOLE_LIST_WIDTH < cols) ? CONSOLE_LIST_WIDTH : cols;
    size_t perm_len  = strlen(permissions);
    size_t right_len = strlen(right);
    size_t name_len  = strlen(name);

    // Room for the name on the line, with one space before the right field
    size_t gap  = right_len ? 1 : 0;
    size_t room = (cols > perm_len + right_len + gap) ? cols - perm_len - right_len - gap : 0;

    print(permissions);
    if (name_len <= room || room < 4) {
        print(name);
    } else {
        char shown[128];
        size_t keep = room - 3;
        if (keep > sizeof(shown) - 4)
            keep = sizeof(shown) - 4;
        memcpy(shown, name, keep);
        memcpy(shown + keep, "...", 4);
        print(shown);
        name_len = keep + 3;
    }

    if (right_len) {
        size_t printed_len = perm_len + name_len;
        size_t spaces = (printed_len + right_len < width) ? width - printed_len - right_len : 1;
        for (size_t i = 0; i < spaces; i++)
            print(" ");
        print(right);
    }
    print("\n");
}

// Print one line per entry of the directory at `path` (absolute):
// permissions, name, and <DIR> or the size. False if `path` is not a
// directory.
static bool console_list_directory(const char* path) {
    DIR* dir = opendir(path);
    if (dir == NULL)
        return false;

    char child[CMD_PATH_MAX];
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        char permissions[5] = "    ";
        const char* right = "";
        char size_text[21];

        int n = snprintf(child, sizeof(child), "%s%s%s", path, strcmp(path, "/") == 0 ? "" : "/", name);
        struct stat st;
        if (n > 0 && (size_t)n < sizeof(child) && stat(child, &st) == 0) {
            if (st.st_mode & S_IXUSR) permissions[0] = 'x';
            if (st.st_mode & S_IRUSR) permissions[1] = 'r';
            if (st.st_mode & S_IWUSR) permissions[2] = 'w';

            if (S_ISDIR(st.st_mode)) {
                // A device root reports as a directory; os_finfo says which
                os_finfo_t info;
                if (os_finfo(child, &info) == 0 && (info.flags & SYS_FINFO_DEVICE))
                    permissions[0] = 'm';
                right = msg_dir_sym;
            } else {
                snprintf(size_text, sizeof(size_text), "%llu", (unsigned long long)st.st_size);
                right = size_text;
            }
        }

        console_print_listing_line(permissions, name, right);
    }
    closedir(dir);
    return true;
}

static int cmd_lsdir_run(int arg_count, char** args) {
    char path[CMD_PATH_MAX];
    if (!command_resolve_directory_arg(arg_count, args, path, sizeof(path)))
        return 1;

    if (!console_list_directory(path)) {
        print(msg_dir_invalid);
        return 1;
    }
    return 0;
}

int console_cmd_lsdir(int arg_count, char** args) {
    int result = cmd_lsdir_run(arg_count, args);
    console_command_end();
    return result;
}

#endif

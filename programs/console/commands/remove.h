#ifndef CONSOLE_CMD_RM_H
#define CONSOLE_CMD_RM_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "path_util.h"

// Remove `path` and, for a directory, everything in it. rmdir() takes only
// an empty directory (the kernel's vfs_remove does the whole tree), so the
// entries go first. The directory is reopened after each one: removing an
// entry shifts the indexes readdir() walks.
static bool rm_tree(const char* path, int depth) {
    struct stat st;
    if (stat(path, &st) < 0)
        return false;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    if (depth > 32)
        return false;               // Deeper than any sane tree

    char child[CMD_PATH_MAX];
    for (;;) {
        DIR* dir = opendir(path);
        if (dir == NULL)
            return false;
        struct dirent* entry;
        bool found = false;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '\0' || strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            int n = snprintf(child, sizeof(child), "%s%s%s", path, strcmp(path, "/") == 0 ? "" : "/", entry->d_name);
            found = n > 0 && (size_t)n < sizeof(child);
            break;
        }
        closedir(dir);
        if (!found)
            break;
        if (!rm_tree(child, depth + 1))
            return false;
    }
    return rmdir(path) == 0;
}

// rm <path>   remove a file, or a directory with everything in it. Removing
// the working directory (or one above it) is fine: the console falls back
// to the nearest directory that still exists.
static int cmd_rm_run(int arg_count, char** args) {
    if (arg_count == 0)
        return 1;

    char path[CMD_PATH_MAX];
    if (!command_resolve_path(args[0], path, sizeof(path)))
        return 2;

    if (!command_exists(path))
        return 4;

    if (!rm_tree(path, 0))
        return 5;

    return 0;
}

int console_cmd_rm(int arg_count, char** args) {
    int result = cmd_rm_run(arg_count, args);
    console_cwd_changed();
    console_command_end();
    return result;
}

#endif

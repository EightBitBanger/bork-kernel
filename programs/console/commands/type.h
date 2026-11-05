#ifndef CONSOLE_CMD_TYPE_H
#define CONSOLE_CMD_TYPE_H

#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "console_host.h"
#include "path_util.h"

// Usage:
//   type <file>                  Print text file to console
//   type <file> > <dest>         Clear/overwrite destination file with source data
//   type <file> >> <dest>        Append source data to destination file
static int cmd_type_run(int arg_count, char** args) {
    if (arg_count == 0) {
        return 1;
    }

    // Resolve source file path
    char src_path[CMD_PATH_MAX];
    if (!command_resolve_path(args[0], src_path, sizeof(src_path))) {
        return 3;
    }

    int src_file = open(src_path, O_RDONLY);
    if (src_file < 0) {
        return 2;
    }

    // Check for redirection arguments (> or >>)
    int redirect_mode = 0; // 0 = none, 1 = overwrite (>), 2 = append (>>)
    char dest_path[CMD_PATH_MAX];

    if (arg_count >= 3) {
        if (strcmp(args[1], ">") == 0) {
            redirect_mode = 1;
        } else if (strcmp(args[1], ">>") == 0) {
            redirect_mode = 2;
        }

        if (redirect_mode > 0) {
            if (!command_resolve_path(args[2], dest_path, sizeof(dest_path))) {
                close(src_file);
                return 4;
            }
        }
    }

    // Open destination file based on redirection mode
    int dest_file = -1;
    if (redirect_mode == 1) {
        // Open for writing, create if missing, truncate (clear) if it exists
        dest_file = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC);
    } else if (redirect_mode == 2) {
        // Open for writing, create if missing, append to the end
        dest_file = open(dest_path, O_WRONLY | O_CREAT | O_APPEND);
    }

    if (redirect_mode > 0 && dest_file < 0) {
        close(src_file);
        return 5;
    }

    // Read and process in chunks
    char buffer[129];
    for (;;) {
        ssize_t got = read(src_file, buffer, sizeof(buffer) - 1);
        if (got <= 0)
            break;

        buffer[got] = '\0';

        if (redirect_mode > 0) {
            // Pipe output into destination file
            if (write(dest_file, buffer, (size_t)got) < 0)
                break;
        } else {
            // Default behavior: print to console
            print(buffer);
        }

        // A NUL byte ends the text, as before
        if (strlen(buffer) < (size_t)got)
            break;
    }

    // Cleanup and close files
    close(src_file);
    if (redirect_mode > 0) {
        close(dest_file);
    }

    return 0;
}

int console_cmd_type(int arg_count, char** args) {
    int result = cmd_type_run(arg_count, args);
    console_command_end();
    return result;
}

#endif

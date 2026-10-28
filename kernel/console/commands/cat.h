#ifndef CONSOLE_CMD_CAT_H
#define CONSOLE_CMD_CAT_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/console/commands/path_util.h>
#include <kernel/console/commands/output.h>

// cat <file> [file...]             print the files one after another
// cat <file> [file...] > <dest>    join them into dest (>> appends)
//
// Unlike type, a NUL byte does not end the output: on the console it is
// skipped, and a redirected copy keeps every byte.
static int cmd_cat_run(int arg_count, char** args) {
    CommandOutput out;
    int file_count = command_output_parse(arg_count, args, &out);
    if (file_count < 0)
        return 1;
    if (file_count == 0) {
        print("cat <file> [file...] [> or >> <dest>]\n");
        return 1;
    }

    // "cat a b > a" would empty a before reading it
    char path[VFS_PATH_MAX];
    for (int i = 0; i < file_count; i++) {
        if (!command_resolve_path(args[i], path, sizeof(path)))
            return 2;
        if (command_output_is(&out, path)) {
            print(args[i]);
            print(": is also the output\n");
            return 2;
        }
    }

    if (!command_output_open(&out))
        return 3;

    int result = 0;
    bool ends_in_newline = true;
    for (int i = 0; i < file_count; i++) {
        // On the console, start each file (or its error) on a new line
        if (out.mode == 0 && !ends_in_newline) {
            print("\n");
            ends_in_newline = true;
        }
        
        File file = command_open_input(args[i], path, sizeof(path));
        if (file == VFS_INVALID_FILE) {
            result = 4;
            continue;
        }

        char buffer[128];
        uint64_t remaining = vfs_get_size(file);
        while (remaining > 0) {
            uint32_t want = (remaining < sizeof(buffer)) ? (uint32_t)remaining : (uint32_t)sizeof(buffer);
            int32_t got = vfs_read(file, buffer, want);
            if (got <= 0)
                break;
            if (!command_output_write(&out, buffer, (uint32_t)got)) {
                print("Write failed\n");
                result = 5;
                remaining = 0;
                break;
            }
            ends_in_newline = (buffer[got - 1] == '\n');
            remaining -= (uint32_t)got;
        }
        vfs_close(file);
        if (result == 5)
            break;
    }

    command_output_close(&out);
    return result;
}

int console_cmd_cat(int arg_count, char** args) {
    int result = cmd_cat_run(arg_count, args);
    console_command_end();
    return result;
}

#endif

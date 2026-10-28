#ifndef CONSOLE_CMD_GREP_H
#define CONSOLE_CMD_GREP_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>
#include <kernel/console/commands/path_util.h>
#include <kernel/console/commands/output.h>

// grep [-i] [-n] [-v] [-c] <text> <file> [file...] [> or >> <dest>]
//
// Prints the lines that contain <text> (a plain substring, no wildcards;
// quote it to search for spaces: grep "two words" notes.txt).
//   -i   ignore upper/lower case
//   -n   put the line number in front of each line
//   -v   print the lines that do NOT contain it
//   -c   print only how many lines matched
// Flags can be combined ("-in"). With more than one file each line starts
// with "<file>:". Lines longer than GREP_LINE_MAX are checked in pieces.
// Returns 0 when something matched, 1 when nothing did, 2 on an error.

#define GREP_LINE_MAX  512

typedef struct {
    const char* text;
    size_t      text_length;
    bool        ignore_case;
    bool        invert;
    bool        number;
    bool        count;
    bool        show_name;
} GrepOptions;

static char grep_fold(char c, bool ignore_case) {
    if (ignore_case && c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

static bool grep_line_matches(const GrepOptions* options, const char* line, size_t length) {
    bool found = false;
    if (options->text_length <= length) {
        for (size_t start = 0; !found && start + options->text_length <= length; start++) {
            size_t i = 0;
            while (i < options->text_length &&
                   grep_fold(line[start + i], options->ignore_case) ==
                   grep_fold(options->text[i], options->ignore_case))
                i++;
            found = (i == options->text_length);
        }
    }
    return found != options->invert;
}

static void grep_print_prefix(const GrepOptions* options, CommandOutput* out,
                              const char* name, uint32_t line_number) {
    if (options->show_name) {
        command_output_print(out, name);
        command_output_print(out, ":");
    }
    if (options->number) {
        char text[11];
        utos(line_number, text);
        command_output_print(out, text);
        command_output_print(out, ":");
    }
}

// Match one line (without its '\n'). Returns true if it matched.
static bool grep_line(const GrepOptions* options, CommandOutput* out, const char* name,
                      char* line, size_t length, uint32_t line_number) {
    if (length > 0 && line[length - 1] == '\r')
        length--;
    if (!grep_line_matches(options, line, length))
        return false;
    if (!options->count) {
        grep_print_prefix(options, out, name, line_number);
        command_output_write(out, line, (uint32_t)length);
        command_output_print(out, "\n");
    }
    return true;
}

// Number of matching lines in an open file
static uint32_t grep_file(const GrepOptions* options, CommandOutput* out, const char* name, File file) {
    char line[GREP_LINE_MAX];
    size_t length = 0;
    uint32_t line_number = 1;
    uint32_t matches = 0;

    char buffer[128];
    uint64_t remaining = vfs_get_size(file);
    while (remaining > 0) {
        uint32_t want = (remaining < sizeof(buffer)) ? (uint32_t)remaining : (uint32_t)sizeof(buffer);
        int32_t got = vfs_read(file, buffer, want);
        if (got <= 0)
            break;
        remaining -= (uint32_t)got;

        for (int32_t i = 0; i < got; i++) {
            if (buffer[i] == '\n') {
                if (grep_line(options, out, name, line, length, line_number))
                    matches++;
                length = 0;
                line_number++;
                continue;
            }
            if (length == sizeof(line)) {
                // Too long: check what we have, carry on with the rest
                if (grep_line(options, out, name, line, length, line_number))
                    matches++;
                length = 0;
            }
            line[length++] = buffer[i];
        }
    }

    // Last line without a '\n'
    if (length > 0 && grep_line(options, out, name, line, length, line_number))
        matches++;
    return matches;
}

static int cmd_grep_run(int arg_count, char** args) {
    CommandOutput out;
    int word_count = command_output_parse(arg_count, args, &out);
    if (word_count < 0)
        return 2;

    GrepOptions options = {0};
    int next = 0;
    for (; next < word_count && args[next][0] == '-' && args[next][1] != '\0'; next++) {
        for (const char* flag = &args[next][1]; *flag != '\0'; flag++) {
            switch (*flag) {
                case 'i': options.ignore_case = true; break;
                case 'n': options.number      = true; break;
                case 'v': options.invert      = true; break;
                case 'c': options.count       = true; break;
                default:
                    print("grep: unknown flag -");
                    char unknown[2] = { *flag, '\0' };
                    print(unknown);
                    print("\n");
                    return 2;
            }
        }
    }

    if (word_count - next < 2) {
        print("grep [-i] [-n] [-v] [-c] <text> <file> [file...]\n");
        return 2;
    }

    options.text        = args[next++];
    options.text_length = strlen(options.text);
    options.show_name   = (word_count - next > 1);

    char path[VFS_PATH_MAX];
    for (int i = next; i < word_count; i++) {
        if (!command_resolve_path(args[i], path, sizeof(path)))
            return 2;
        if (command_output_is(&out, path)) {
            print(args[i]);
            print(": is also the output\n");
            return 2;
        }
    }

    if (!command_output_open(&out))
        return 2;

    bool failed = false;
    uint32_t total = 0;
    for (int i = next; i < word_count; i++) {
        File file = command_open_input(args[i], path, sizeof(path));
        if (file == VFS_INVALID_FILE) {
            failed = true;
            continue;
        }
        uint32_t matches = grep_file(&options, &out, args[i], file);
        vfs_close(file);
        total += matches;

        if (options.count) {
            char text[11];
            utos(matches, text);
            if (options.show_name) {
                command_output_print(&out, args[i]);
                command_output_print(&out, ":");
            }
            command_output_print(&out, text);
            command_output_print(&out, "\n");
        }
    }

    command_output_close(&out);

    if (failed)
        return 2;
    return (total > 0) ? 0 : 1;
}

int console_cmd_grep(int arg_count, char** args) {
    int result = cmd_grep_run(arg_count, args);
    console_command_end();
    return result;
}

#endif

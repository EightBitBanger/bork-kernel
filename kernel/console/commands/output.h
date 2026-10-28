#ifndef CONSOLE_CMD_OUTPUT_H
#define CONSOLE_CMD_OUTPUT_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>
#include <kernel/console/print.h>
#include <kernel/console/commands/path_util.h>

// Where a command's text goes: the console, or a file named on the command
// line with a trailing redirection
//
//   cat a.txt b.txt > all.txt     overwrite (create if missing)
//   echo hello >> log.txt         append (create if missing)
//
// Used by cat, echo and grep.
typedef struct {
    File     file;                  // VFS_INVALID_FILE: the console
    uint16_t mode;                  // vfs_open flags, 0: no redirection
    char     path[VFS_PATH_MAX];    // The file, when mode != 0
} CommandOutput;

// Looks at the last two words for "> <file>" or ">> <file>" and resolves
// the file, without opening it yet (so the command can check it is not one
// of its inputs before "> file" empties it). Returns how many words come
// before the redirection (arg_count when there is none), or -1 after
// printing why the path is no good.
static inline int command_output_parse(int arg_count, char** args, CommandOutput* out) {
    out->file = VFS_INVALID_FILE;
    out->mode = 0;
    out->path[0] = '\0';
    if (arg_count < 2)
        return arg_count;
    
    const char* op = args[arg_count - 2];
    if (strcmp(op, ">") == 0)
        out->mode = VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE;
    else if (strcmp(op, ">>") == 0)
        out->mode = VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_APPEND;
    else
        return arg_count;
    
    if (!command_resolve_path(args[arg_count - 1], out->path, sizeof(out->path)))
        return -1;
    return arg_count - 2;
}

// True when the output is redirected to the file at `path` (absolute and
// normalized, as command_resolve_path makes it)
static inline bool command_output_is(const CommandOutput* out, const char* path) {
    return out->mode != 0 && strcmp(out->path, path) == 0;
}

// Open the redirection file, if any. False after printing why it failed.
static inline bool command_output_open(CommandOutput* out) {
    if (out->mode == 0)
        return true;
    out->file = vfs_open(out->path, out->mode);
    if (out->file == VFS_INVALID_FILE) {
        print("Cannot write ");
        print(out->path);
        print("\n");
        return false;
    }
    return true;
}

static inline bool command_output_to_file(const CommandOutput* out) {
    return out->file != VFS_INVALID_FILE;
}

// Write `length` bytes. On the console NUL bytes are skipped (print() would
// stop at them); a file gets the bytes as they are.
static inline bool command_output_write(CommandOutput* out, const char* data, uint32_t length) {
    if (command_output_to_file(out))
        return vfs_write(out->file, data, length) == (int32_t)length;

    char chunk[129];
    uint32_t used = 0;
    for (uint32_t i = 0; i < length; i++) {
        if (data[i] == '\0')
            continue;
        chunk[used++] = data[i];
        if (used == sizeof(chunk) - 1) {
            chunk[used] = '\0';
            print(chunk);
            used = 0;
        }
    }
    if (used > 0) {
        chunk[used] = '\0';
        print(chunk);
    }
    return true;
}

static inline bool command_output_print(CommandOutput* out, const char* text) {
    return command_output_write(out, text, (uint32_t)strlen(text));
}

static inline void command_output_close(CommandOutput* out) {
    if (command_output_to_file(out))
        vfs_close(out->file);
    out->file = VFS_INVALID_FILE;
}

// Open `arg` for reading, printing why when that is not possible (a
// directory, missing, refused). `path` receives the resolved path.
static inline File command_open_input(const char* arg, char* path, size_t path_size) {
    if (!command_resolve_path(arg, path, path_size))
        return VFS_INVALID_FILE;

    FSFileStats stats;
    if (!vfs_stat(path, &stats)) {
        print(arg);
        print(": not found\n");
        return VFS_INVALID_FILE;
    }
    if (stats.type != VFS_TYPE_FILE) {
        print(arg);
        print(": is a directory\n");
        return VFS_INVALID_FILE;
    }

    File file = vfs_open(path, VFS_OPEN_READ);
    if (file == VFS_INVALID_FILE) {
        print(arg);
        print(": cannot open\n");
    }
    return file;
}

#endif

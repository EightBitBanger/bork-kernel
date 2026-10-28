#ifndef CONSOLE_CMD_WC_H
#define CONSOLE_CMD_WC_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>
#include <kernel/console/commands/output.h>

// wc <file> [file...]   count lines, words and bytes
//
//   lines    words    bytes  name
// A total follows when there is more than one file.

typedef struct {
    uint64_t lines;
    uint64_t words;
    uint64_t bytes;
} WcCounts;

static void wc_print_column(uint64_t value) {
    char text[21];
    u64tos(value, text);
    for (size_t pad = strlen(text); pad < 8; pad++)
        print(" ");
    print(text);
    print(" ");
}

static void wc_print(const WcCounts* counts, const char* name) {
    wc_print_column(counts->lines);
    wc_print_column(counts->words);
    wc_print_column(counts->bytes);
    print(" ");
    print(name);
    print("\n");
}

static void wc_count(File file, WcCounts* counts) {
    bool in_word = false;
    char buffer[128];
    uint64_t remaining = vfs_get_size(file);
    while (remaining > 0) {
        uint32_t want = (remaining < sizeof(buffer)) ? (uint32_t)remaining : (uint32_t)sizeof(buffer);
        int32_t got = vfs_read(file, buffer, want);
        if (got <= 0)
            break;
        remaining -= (uint32_t)got;
        counts->bytes += (uint32_t)got;
        
        for (int32_t i = 0; i < got; i++) {
            char c = buffer[i];
            if (c == '\n')
                counts->lines++;
            bool space = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0');
            if (!space && !in_word)
                counts->words++;
            in_word = !space;
        }
    }
}

static int cmd_wc_run(int arg_count, char** args) {
    if (arg_count == 0) {
        print("wc <file> [file...]\n");
        return 1;
    }
    
    int result = 0;
    WcCounts total = {0};
    char path[VFS_PATH_MAX];
    for (int i = 0; i < arg_count; i++) {
        File file = command_open_input(args[i], path, sizeof(path));
        if (file == VFS_INVALID_FILE) {
            result = 2;
            continue;
        }
        WcCounts counts = {0};
        wc_count(file, &counts);
        vfs_close(file);
        wc_print(&counts, args[i]);
        
        total.lines += counts.lines;
        total.words += counts.words;
        total.bytes += counts.bytes;
    }
    
    if (arg_count > 1)
        wc_print(&total, "total");
    return result;
}

int console_cmd_wc(int arg_count, char** args) {
    int result = cmd_wc_run(arg_count, args);
    console_command_end();
    return result;
}

#endif

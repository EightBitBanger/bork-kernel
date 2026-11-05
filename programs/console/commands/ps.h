#ifndef CONSOLE_CMD_PS_H
#define CONSOLE_CMD_PS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include "console_host.h"

// ps   list the running processes
//
//   PID    STATE  PRI  THR  HEAP KB  NAME
//
// Read from /proc/<pid> (one ProcessRecord each), as the kernel console
// does. A process that ends while the list is printed may be left out. The
// last line counts the threads of the listed processes (the kernel console
// also counts kernel threads, which a program can't see).

// The contents of /proc/<pid>: a copy of ProcessRecord from the kernel's
// scheduler/process.h, which must stay the same layout
#define PROCESS_RECORD_MAGIC    0x434F5250u     // "PROC"
#define PROCESS_RECORD_VERSION  1
#define PROCESS_RECORD_NAME     16
#define PROCESS_NAME_MAX        16
#define PROCESS_PATH_MAX        256

typedef struct {
    char     node_name[PROCESS_RECORD_NAME];    // The decimal pid
    uint32_t magic;                             // PROCESS_RECORD_MAGIC
    uint32_t version;                           // PROCESS_RECORD_VERSION
    int32_t  pid;
    uint32_t state;                             // ProcessState
    uint32_t priority;                          // ThreadPriority (base)
    uint32_t security;                          // SYS_SEC_* mask
    uint32_t thread_count;
    uint32_t entry;                             // User address of the entry point
    uint32_t code_size;
    uint32_t data_size;
    uint32_t bss_size;
    uint32_t heap_size;
    char     name[PROCESS_NAME_MAX];
    char     image_path[PROCESS_PATH_MAX];      // "<memory>" for a flat image
} __attribute__((packed)) ProcessRecord;

_Static_assert(sizeof(ProcessRecord) == 16 + 12 * 4 + PROCESS_NAME_MAX + PROCESS_PATH_MAX,
               "ProcessRecord layout");

static void ps_print_column(uint32_t value, size_t width) {
    char text[11];
    snprintf(text, sizeof(text), "%u", (unsigned)value);
    for (size_t pad = strlen(text); pad < width; pad++)
        print(" ");
    print(text);
    print("  ");
}

static void ps_print_text(const char* text, size_t width) {
    print(text);
    for (size_t pad = strlen(text); pad < width; pad++)
        print(" ");
}

// ProcessState and ThreadPriority values (scheduler/process.h, scheduler.h)
static const char* ps_state_name(uint32_t state) {
    switch (state) {
        case 1:  return "start";
        case 2:  return "run";
        case 3:  return "exit";
        default: return "?";
    }
}

static const char* ps_priority_name(uint32_t priority) {
    switch (priority) {
        case 1:  return "idle";
        case 2:  return "low";
        case 3:  return "norm";
        case 4:  return "high";
        case 5:  return "rt";
        default: return "?";
    }
}

// Read /proc/<name> into *record. False if it is gone or not a record.
static bool ps_read_record(const char* name, ProcessRecord* record) {
    char path[64];
    int n = snprintf(path, sizeof(path), "/proc/%s", name);
    if (n < 0 || (size_t)n >= sizeof(path))
        return false;

    int file = open(path, O_RDONLY);
    if (file < 0)
        return false;

    bool ok = read(file, record, sizeof(*record)) == (ssize_t)sizeof(*record);
    close(file);

    if (!ok || record->magic != PROCESS_RECORD_MAGIC || record->version != PROCESS_RECORD_VERSION)
        return false;
    record->name[PROCESS_NAME_MAX - 1] = '\0';
    return true;
}

static int cmd_ps_run(int arg_count, char** args) {
    (void)arg_count;
    (void)args;

    DIR* dir = opendir("/proc");
    if (dir == NULL) {
        print("No /proc\n");
        return 1;
    }

    print("PID    STATE  PRI  THR  HEAP KB  NAME\n");

    uint32_t listed = 0;
    uint32_t threads = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        ProcessRecord record;
        if (!ps_read_record(entry->d_name, &record))
            continue;

        ps_print_text(entry->d_name, 7);
        ps_print_text(ps_state_name(record.state), 7);
        ps_print_text(ps_priority_name(record.priority), 5);
        ps_print_column(record.thread_count, 3);
        ps_print_column((record.heap_size + 1023) / 1024, 7);
        print(record.name);
        print("\n");
        listed++;
        threads += record.thread_count;
    }
    closedir(dir);

    char text[64];
    snprintf(text, sizeof(text), "%u %s, %u threads\n", (unsigned)listed,
             listed == 1 ? "process" : "processes", (unsigned)threads);
    print(text);
    return 0;
}

int console_cmd_ps(int arg_count, char** args) {
    int result = cmd_ps_run(arg_count, args);
    console_command_end();
    return result;
}

#endif

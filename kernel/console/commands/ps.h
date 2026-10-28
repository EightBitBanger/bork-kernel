#ifndef CONSOLE_CMD_PS_H
#define CONSOLE_CMD_PS_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/process.h>

// ps   list the running processes
//
//   PID    STATE  PRI  THR  HEAP KB  NAME
//
// Read from /proc/<pid> (one ProcessRecord each, see scheduler/process.h),
// the same place a user program would look. A process that ends while the
// list is printed may be left out. The last line counts every thread,
// kernel threads included.

static void ps_print_column(uint32_t value, size_t width) {
    char text[11];
    utos(value, text);
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

static const char* ps_state_name(uint32_t state) {
    switch (state) {
        case PROCESS_STATE_CREATING: return "start";
        case PROCESS_STATE_RUNNING:  return "run";
        case PROCESS_STATE_DYING:    return "exit";
        default:                     return "?";
    }
}

static const char* ps_priority_name(uint32_t priority) {
    switch (priority) {
        case PRIORITY_IDLE:     return "idle";
        case PRIORITY_LOW:      return "low";
        case PRIORITY_NORMAL:   return "norm";
        case PRIORITY_HIGH:     return "high";
        case PRIORITY_REALTIME: return "rt";
        default:                return "?";
    }
}

// Read /proc/<name> into *record. False if it is gone or not a record.
static bool ps_read_record(const char* name, ProcessRecord* record) {
    char path[VFS_PATH_MAX];
    if (!vfs_path_normalize("/proc", name, path, sizeof(path)))
        return false;
    
    File file = vfs_open(path, VFS_OPEN_READ);
    if (file == VFS_INVALID_FILE)
        return false;
    
    bool ok = vfs_get_size(file) >= sizeof(*record) &&
              vfs_read(file, record, sizeof(*record)) == (int32_t)sizeof(*record);
    vfs_close(file);
    
    if (!ok || record->magic != PROCESS_RECORD_MAGIC || record->version != PROCESS_RECORD_VERSION)
        return false;
    record->name[PROCESS_NAME_MAX - 1] = '\0';
    return true;
}

static int cmd_ps_run(int arg_count, char** args) {
    (void)arg_count;
    (void)args;
    
    if (!vfs_directory_check("/proc")) {
        print("No /proc\n");
        return 1;
    }
    
    print("PID    STATE  PRI  THR  HEAP KB  NAME\n");
    
    uint32_t listed = 0;
    char name[VFS_NAME_MAX + 1];
    uint32_t count = vfs_directory_get_item_count("/proc");
    for (uint32_t i = 0; i < count; i++) {
        if (!vfs_directory_get_item("/proc", i, name, sizeof(name)))
            continue;
        
        ProcessRecord record;
        if (!ps_read_record(name, &record))
            continue;
        
        ps_print_text(name, 7);
        ps_print_text(ps_state_name(record.state), 7);
        ps_print_text(ps_priority_name(record.priority), 5);
        ps_print_column(record.thread_count, 3);
        ps_print_column((record.heap_size + 1023) / 1024, 7);
        print(record.name);
        print("\n");
        listed++;
    }
    
    char text[11];
    utos(listed, text);
    print(text);
    print(listed == 1 ? " process, " : " processes, ");
    utos(thread_get_count(), text);
    print(text);
    print(" threads\n");
    return 0;
}

int console_cmd_ps(int arg_count, char** args) {
    int result = cmd_ps_run(arg_count, args);
    console_command_end();
    return result;
}

#endif

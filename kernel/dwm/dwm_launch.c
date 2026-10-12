// Starting programs from the desktop (and anything else in the DWM).
//
// A file whose VFS permissions include VFS_PERMISSION_EXECUTE is a program:
// opening it (double click, the "open" menu action) loads it as an ELF
// executable and starts it as a process instead of opening it in notepad.
//
// Loading reads the whole file through the VFS, which can take a while for
// a large program on a slow disk. Window procedures and actions must not
// block (the whole desktop stops), so the load runs on a short-lived kernel
// thread. Only a failure comes back to the DWM, through dwm_invoke(), as a
// notification. Details of why an ELF was refused go to COM1.

#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/vfs/vfs.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/process.h>
#include <kernel/malloc.h>
#include <kernel/util/string.h>

#define DWM_LAUNCH_PRIORITY      PRIORITY_NORMAL    // Priority of started programs
#define DWM_LAUNCHER_PRIORITY    PRIORITY_NORMAL    // Priority of the loading thread

bool dwm_path_is_executable(const char* path) {
    if (path == NULL || path[0] == '\0') return false;

    // Directories (and mounts) may carry the execute bit too ("search");
    // only files run
    if (!vfs_exists(path) || vfs_directory_check(path)) return false;

    // Same call the Properties window uses to show and toggle the 'x'
    uint8_t permissions = 0;
    if (!vfs_get_permissions(path, &permissions)) return false;
    return (permissions & VFS_PERMISSION_EXECUTE) != 0;
}

// DWM thread, DWM lock held (queued with dwm_invoke). arg is one of the
// static strings from process_error_string(), so nothing to free.
static void launch_report_failure(void* arg) {
    dwm_notify("Run", (const char*)arg, DWM_NOTIFY_LONG_MS);
}

// Launcher thread: owns `arg` (a heap copy of the path)
static void launch_thread(void* arg) {
    char* path = (char*)arg;

    ProcessId pid = CreateProcess(path, DWM_LAUNCH_PRIORITY);
    free(path);

    if (pid < 0)
        dwm_invoke(launch_report_failure, (void*)process_error_string(pid));
}

bool dwm_launch_program(const char* path) {
    DWM_GUARD();

    if (!dwm_path_is_executable(path)) {
        dwm_notify("Run", "The file is not marked executable", DWM_NOTIFY_LONG_MS);
        return false;
    }

    size_t length = strnlen(path, VFS_PATH_MAX);
    if (length >= VFS_PATH_MAX) {
        dwm_notify("Run", "The path is too long", DWM_NOTIFY_LONG_MS);
        return false;
    }

    // The thread outlives this call (and the icon the path came from)
    char* copy = (char*)malloc(length + 1);
    if (copy == NULL) {
        dwm_notify("Run", "Not enough memory to start it", DWM_NOTIFY_LONG_MS);
        return false;
    }
    memcpy(copy, path, length);
    copy[length] = '\0';

    if (kthread_create(launch_thread, copy, DWM_LAUNCHER_PRIORITY, "launcher") == NULL) {
        free(copy);
        dwm_notify("Run", "Could not start the loader thread", DWM_NOTIFY_LONG_MS);
        return false;
    }
    return true;
}

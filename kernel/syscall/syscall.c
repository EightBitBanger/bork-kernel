#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/vfs/vfs.h>

#include <kernel/syscall/calls/sys_exit.h>
#include <kernel/syscall/calls/sys_print.h>
#include <kernel/syscall/calls/sys_yield.h>
#include <kernel/syscall/calls/sys_sleep.h>
#include <kernel/syscall/calls/sys_getpid.h>
#include <kernel/syscall/calls/sys_sbrk.h>

// File calls. sys_files.h holds the per-process descriptor tables and
// working directories they share; sys_user.h the user-memory helpers.
#include <kernel/syscall/calls/sys_file.h>      // open close read write seek fstat
#include <kernel/syscall/calls/sys_path.h>      // stat mkdir remove rename truncate copy
#include <kernel/syscall/calls/sys_dir.h>       // readdir chdir getcwd

typedef struct {
    const char* name;       // For traces and reports
    SyscallFn   handler;
} SyscallEntry;

// Indexed by syscall number, so a missing row is a compile error
static const SyscallEntry syscall_table[] = {
    [SYS_EXIT]     = { "exit",     sys_exit     },
    [SYS_PRINT]    = { "print",    sys_print    },
    [SYS_YIELD]    = { "yield",    sys_yield    },
    [SYS_SLEEP]    = { "sleep",    sys_sleep    },
    [SYS_GETPID]   = { "getpid",   sys_getpid   },
    [SYS_SBRK]     = { "sbrk",     sys_sbrk     },
    [SYS_OPEN]     = { "open",     sys_open     },
    [SYS_CLOSE]    = { "close",    sys_close    },
    [SYS_READ]     = { "read",     sys_read     },
    [SYS_WRITE]    = { "write",    sys_write    },
    [SYS_SEEK]     = { "seek",     sys_seek     },
    [SYS_FSTAT]    = { "fstat",    sys_fstat    },
    [SYS_STAT]     = { "stat",     sys_stat     },
    [SYS_MKDIR]    = { "mkdir",    sys_mkdir    },
    [SYS_REMOVE]   = { "remove",   sys_remove   },
    [SYS_RENAME]   = { "rename",   sys_rename   },
    [SYS_TRUNCATE] = { "truncate", sys_truncate },
    [SYS_READDIR]  = { "readdir",  sys_readdir  },
    [SYS_CHDIR]    = { "chdir",    sys_chdir    },
    [SYS_GETCWD]   = { "getcwd",   sys_getcwd   },
    [SYS_COPY]     = { "copy",     sys_copy     },
};

_Static_assert(sizeof(syscall_table) / sizeof(syscall_table[0]) == SYS_COUNT,
               "every syscall number needs a row in syscall_table");

// The user ABI mirrors these VFS values; the handlers copy them straight through
_Static_assert(SYS_PATH_MAX == VFS_PATH_MAX, "SYS_PATH_MAX must equal VFS_PATH_MAX");
_Static_assert(SYS_NAME_MAX == VFS_NAME_MAX, "SYS_NAME_MAX must equal VFS_NAME_MAX");
_Static_assert(SYS_TYPE_FILE == VFS_TYPE_FILE && SYS_TYPE_DIRECTORY == VFS_TYPE_DIRECTORY &&
               SYS_TYPE_MOUNT == VFS_TYPE_MOUNT, "SYS_TYPE_* must match VFS_TYPE_*");
_Static_assert(SYS_PERM_EXECUTE == VFS_PERMISSION_EXECUTE && SYS_PERM_READ == VFS_PERMISSION_READ &&
               SYS_PERM_WRITE == VFS_PERMISSION_WRITE, "SYS_PERM_* must match VFS_PERMISSION_*");
_Static_assert(sizeof(SysStat) == 20, "SysStat is part of the user ABI");
_Static_assert(SYS_FD_FIRST < SYS_FD_MAX, "no room for descriptors");

// What isr_syscall.asm leaves on the kernel stack: segment registers, the
// pushad block, then the ring 3 -> 0 frame the CPU pushed. The handler's
// result goes back through the eax slot, which popad loads.
struct SyscallFrame {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, user_esp, user_ss;
} __attribute__((packed));

_Static_assert(sizeof(struct SyscallFrame) == 68, "must match isr_syscall.asm");

extern ThreadBlock* current_thread;

void syscall_init(void) {
    serial_init();
    sys_print_init();
}

void syscall_process_exit(uint32_t process_id) {
    sys_files_process_exit(process_id);
}

// Called from isr_syscall.asm with interrupts enabled, on the calling
// thread's kernel stack.
void syscall_dispatch(struct SyscallFrame* frame) {
    // The gate is DPL 3 so ring 3 can use it; a kernel thread has no
    // business here (and no process to act on)
    if ((frame->cs & 3) != 3 || current_thread == NULL || current_thread->process_id == 0) {
        frame->eax = (uint32_t)SYS_ENOSYS;
        return;
    }

    uint32_t number = frame->eax;
    if (number >= SYS_COUNT || syscall_table[number].handler == NULL) {
        frame->eax = (uint32_t)SYS_ENOSYS;
        return;
    }

    SyscallArgs args = {
        .arg1 = frame->ebx,
        .arg2 = frame->ecx,
        .arg3 = frame->edx,
        .arg4 = frame->esi,
        .arg5 = frame->edi,
    };

    frame->eax = (uint32_t)syscall_table[number].handler(&args);
}

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/process.h>
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

// Graphics: per-process surfaces drawn with the kernel's gfx layer
#include <kernel/syscall/calls/sys_gfx.h>       // gfx_info create destroy submit write read present

// Security clearance: per-process SYS_SEC_* masks
#include <kernel/syscall/calls/sys_security.h>  // sec_get sec_set

// IPC: pipes, ports, shared memory; and the keyboard / mouse grab
#include <kernel/syscall/calls/sys_ipc.h>       // ipc_* shm_* input_*

// Resources: load by path, look up by name, draw with the gfx calls
#include <kernel/syscall/calls/sys_resource.h>  // res_load res_get res_info
#include <kernel/syscall/calls/sys_home.h>      // gethome
#include <kernel/syscall/calls/sys_time.h>      // time
#include <kernel/syscall/calls/sys_input.h>     // input_warp

typedef struct {
    const char* name;       // For traces and reports
    SyscallFn   handler;
    uint32_t    requires;   // SYS_SEC_* bits the caller must hold (see syscall.h)
} SyscallEntry;

#define R   SYS_SEC_FILE_READ
#define W   SYS_SEC_FILE_WRITE
#define GFX SYS_SEC_GFX
#define IPC SYS_SEC_IPC
#define INP SYS_SEC_INPUT

// Indexed by syscall number, so a missing row is a compile error.
//
// `requires` is what every call of that number needs. A 0 here does not
// always mean "free": open (flags) and write (console or file) add
// requirements that depend on their arguments.
static const SyscallEntry syscall_table[] = {
    [SYS_EXIT]        = { "exit",        sys_exit,        0 },
    [SYS_PRINT]       = { "print",       sys_print,       SYS_SEC_CONSOLE },
    [SYS_YIELD]       = { "yield",       sys_yield,       0 },
    [SYS_SLEEP]       = { "sleep",       sys_sleep,       0 },
    [SYS_GETPID]      = { "getpid",      sys_getpid,      0 },
    [SYS_SBRK]        = { "sbrk",        sys_sbrk,        0 },
    [SYS_OPEN]        = { "open",        sys_open,        0 },      // R and/or W by flags
    [SYS_CLOSE]       = { "close",       sys_close,       0 },      // Always: lets a process let go
    [SYS_READ]        = { "read",        sys_read,        R },
    [SYS_WRITE]       = { "write",       sys_write,       0 },      // CONSOLE or W by descriptor
    [SYS_SEEK]        = { "seek",        sys_seek,        0 },
    [SYS_FSTAT]       = { "fstat",       sys_fstat,       R },
    [SYS_STAT]        = { "stat",        sys_stat,        R },
    [SYS_MKDIR]       = { "mkdir",       sys_mkdir,       W },
    [SYS_REMOVE]      = { "remove",      sys_remove,      W },
    [SYS_RENAME]      = { "rename",      sys_rename,      W },
    [SYS_TRUNCATE]    = { "truncate",    sys_truncate,    W },
    [SYS_READDIR]     = { "readdir",     sys_readdir,     R },
    [SYS_CHDIR]       = { "chdir",       sys_chdir,       R },
    [SYS_GETCWD]      = { "getcwd",      sys_getcwd,      0 },
    [SYS_COPY]        = { "copy",        sys_copy,        R | W },
    [SYS_GFX_INFO]    = { "gfx_info",    sys_gfx_info,    GFX },
    [SYS_GFX_CREATE]  = { "gfx_create",  sys_gfx_create,  GFX },
    [SYS_GFX_DESTROY] = { "gfx_destroy", sys_gfx_destroy, GFX },
    [SYS_GFX_SUBMIT]  = { "gfx_submit",  sys_gfx_submit,  GFX },
    [SYS_GFX_WRITE]   = { "gfx_write",   sys_gfx_write,   GFX },
    [SYS_GFX_READ]    = { "gfx_read",    sys_gfx_read,    GFX },
    [SYS_GFX_PRESENT] = { "gfx_present", sys_gfx_present, GFX | SYS_SEC_GFX_SCREEN },
    [SYS_SEC_GET]     = { "sec_get",     sys_sec_get,     0 },      // ADMIN for other processes
    [SYS_SEC_SET]     = { "sec_set",     sys_sec_set,     SYS_SEC_ADMIN },
    [SYS_IPC_CLOSE]   = { "ipc_close",   sys_ipc_close,   0 },      // Always: lets a process let go
    [SYS_IPC_PIPE]    = { "ipc_pipe",    sys_ipc_pipe,    IPC },
    [SYS_IPC_PORT_CREATE] = { "ipc_port_create", sys_ipc_port_create, IPC },  // + ADMIN for "sys.*"
    [SYS_IPC_CONNECT] = { "ipc_connect", sys_ipc_connect, IPC },
    [SYS_IPC_ACCEPT]  = { "ipc_accept",  sys_ipc_accept,  IPC },
    [SYS_IPC_SEND]    = { "ipc_send",    sys_ipc_send,    IPC },
    [SYS_IPC_RECV]    = { "ipc_recv",    sys_ipc_recv,    IPC },
    [SYS_IPC_WAIT]    = { "ipc_wait",    sys_ipc_wait,    0 },      // IPC or INPUT by handle type
    [SYS_SHM_CREATE]  = { "shm_create",  sys_shm_create,  IPC },
    [SYS_SHM_MAP]     = { "shm_map",     sys_shm_map,     IPC },
    [SYS_SHM_UNMAP]   = { "shm_unmap",   sys_shm_unmap,   IPC },
    [SYS_IPC_INFO]    = { "ipc_info",    sys_ipc_info,    0 },
    [SYS_INPUT_OPEN]  = { "input_open",  sys_input_open,  INP },
    [SYS_INPUT_READ]  = { "input_read",  sys_input_read,  INP },
    [SYS_RES_LOAD]    = { "res_load",    sys_res_load,    R | GFX },
    [SYS_RES_GET]     = { "res_get",     sys_res_get,     GFX },
    [SYS_RES_INFO]    = { "res_info",    sys_res_info,    GFX },
    [SYS_GETHOME]     = { "gethome",     sys_gethome,     0 },      // Always, like getcwd
    [SYS_TIME]        = { "time",        sys_time,        0 },
    [SYS_INPUT_WARP]  = { "input_warp",  sys_input_warp,  INP },    // + the caller's input handle
};

#undef R
#undef W
#undef GFX
#undef IPC
#undef INP

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
_Static_assert(sizeof(SysGfxRect) == 16 && sizeof(SysGfxInfo) == 32, "gfx structures are part of the user ABI");
_Static_assert(SYS_GFX_BLEND_COPY == GFX_BLEND_COPY && SYS_GFX_BLEND_ALPHA == GFX_BLEND_ALPHA &&
               SYS_GFX_GRADIENT_VERTICAL == GFX_GRADIENT_VERTICAL &&
               SYS_GFX_GRADIENT_HORIZONTAL == GFX_GRADIENT_HORIZONTAL, "SYS_GFX_* must match the gfx layer");
_Static_assert(SYS_GFX_PIE_MAX == GFX_PIE_MAX_SLICES, "SYS_GFX_PIE_MAX must equal GFX_PIE_MAX_SLICES");
_Static_assert(SYS_RES_TYPE_SPRITE == RESOURCE_TYPE_SPRITE, "SYS_RES_TYPE_* must match ResourceType");
_Static_assert(sizeof(SysResInfo) == 16, "SysResInfo is part of the user ABI");

// File security masks hold SYS_SEC_* bits: a process's mask is its clearance
_Static_assert(VFS_SECURITY_ADMIN == SYS_SEC_ADMIN, "VFS_SECURITY_ADMIN must equal SYS_SEC_ADMIN");
_Static_assert((SYS_SEC_VALID & ~VFS_CLEARANCE_ALL) == 0, "every SYS_SEC_* bit must fit a file mask");

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

    // From here on, file access by a process thread is checked against its
    // security mask; kernel threads keep full clearance
    vfs_set_clearance_source(process_current_clearance);
}

bool syscall_process_start(uint32_t process_id, struct AddressSpace* space, const char* cwd) {
    return sys_files_process_start(process_id, space, cwd);
}

void syscall_process_exit(uint32_t process_id) {
    sys_files_process_exit(process_id);
    sys_gfx_process_exit(process_id);
    ipc_process_exit(process_id);       // Unmaps shared memory: the space must still exist
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

    // Clearance first: a refused call has no effect at all
    uint32_t requires = syscall_table[number].requires;
    if (requires != 0 && (process_current_security() & requires) != requires) {
        frame->eax = (uint32_t)SYS_EPERM;
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





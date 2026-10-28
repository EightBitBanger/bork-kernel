#ifndef _KERNEL_PROCESS_H_
#define _KERNEL_PROCESS_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/scheduler/scheduler.h>
#include <kernel/syscall/syscall.h>     // SYS_SEC_* (security mask bits)

// Processes
//
// A process owns a private user address space (page directory), the image
// and stack mapped into it, and a list of threads. Its threads run in ring 3
// and reach the kernel only through interrupts, faults and syscalls
// (int 0x81, see syscall/syscall.h).
//
// User address space layout (vmm.h has the full map):
//
//   0x40000000 - 0x4FFFFFFF    image area (PROCESS_IMAGE_MAX). A flat image
//                              sits at PROCESS_IMAGE_BASE; an ELF executable's
//                              PT_LOAD segments go wherever they were linked,
//                              as long as they stay inside this area
//   next page after the image  heap: grows up with sbrk, at most
//                              PROCESS_HEAP_MAX bytes
//   ...                        unmapped
//   0xBFFF0000 - 0xBFFFFFFF    PROCESS_STACK_PAGES of stack, top at 0xC0000000
//                              (the unmapped page below it catches overflow)
//
// Everything below 0x40000000 and from 0xC0000000 up is kernel, mapped
// supervisor-only: a user access there page-faults and kills the process.
//
// ProcessId layout: [ generation (23 bits) | slot (8 bits) ], always > 0.
// The generation is bumped every time a slot is freed, so a stale id held
// after DestroyProcess() can never refer to a newer process that happened
// to reuse the same slot.
//
// Working directory: every process starts with one, set before its first
// thread runs (syscall_process_start). Relative paths in file syscalls
// resolve against it, and the program can change it with SYS_CHDIR.
//
// Loading:
//   CreateProcess()           an ELF32 i386 executable from the VFS
//                             (resources/classes/elf.h). The file must carry
//                             VFS_PERMISSION_EXECUTE. Build programs static
//                             and non-PIE, linked at 0x40000000. Starts in
//                             the directory holding the executable.
//   CreateProcessIn()         the same, in a working directory of the
//                             caller's choice.
//   CreateProcessFromImage()  a flat binary already in kernel memory, mapped
//                             read/write at PROCESS_IMAGE_BASE. Starts in "/".
//
// Security clearance: every process has a mask of SYS_SEC_* bits (see the
// "Security clearance" section of syscall/syscall.h). The syscall layer
// checks it on every call. The plain CreateProcess* functions give
// PROCESS_SECURITY_DEFAULT; the *WithSecurity variants and
// CreateProcessLaunch() take the mask, which is in place before the main
// thread can run. Afterwards it changes only through ProcessSetSecurity()
// (kernel, unchecked) or SYS_SEC_SET (needs SYS_SEC_ADMIN).
//
// /proc: every process is listed as a read-only kernel-node file
// /proc/<pid> (decimal pid) holding one ProcessRecord (below), from just
// before its main thread starts until DestroyProcess(). The state, thread
// count, security mask and heap size are kept current. /proc must exist as
// a kernel folder (kernel_init makes it); without it processes still run,
// unlisted. See the /proc section of process.c.
//
// Arguments: the main thread starts with argc / argv on its stack (layout in
// the "Processes" section of syscall/syscall.h). CreateProcessLaunch() takes
// them; the other loaders pass argv = { image path } (flat: { name }).
// SYS_SPAWN reaches CreateProcessLaunch() from user space, always with a
// mask inside the caller's own.

#define PROCESS_MAX                64     // Must be <= 256 (8-bit slot field)
#define PROCESS_NAME_MAX           16
#define PROCESS_PATH_MAX          256
#define PROCESS_ARG_MAX            64     // argv strings (= SYS_ARG_MAX)
#define PROCESS_ARG_BYTES        4096     // Bytes of them, terminators included (= SYS_ARG_BYTES)

#define PROCESS_IMAGE_BASE        0x40000000u            // == VM_USER_START
#define PROCESS_IMAGE_MAX         (256u * 1024u * 1024u) // Size of the image area
#define PROCESS_STACK_TOP         0xC0000000u            // == VM_USER_STACK_TOP
#define PROCESS_STACK_PAGES       16                     // 64 KB

// Per-process heap limit, so one process cannot take all user memory. Across
// processes, user pages come from pmm_alloc_user_frame(), which leaves a
// reserve of frames for the kernel; past that, sbrk fails with SYS_ENOMEM.
// Pages are mapped only as the break reaches them, so a high limit costs
// nothing until a program uses it (lm training on a 20 MB text needs ~450 MB).
#define PROCESS_HEAP_MAX          (1024u * 1024u * 1024u)

typedef int32_t ProcessId;

// Security mask: SYS_SEC_* bits
typedef uint32_t ProcessSecurity;

// What CreateProcess / CreateProcessIn / CreateProcessFromImage grant: files,
// the console, graphics, IPC, starting other programs and asking the user
// for admin (SYS_ELEVATE). Not the screen, input or admin itself.
#ifndef PROCESS_SECURITY_DEFAULT
#define PROCESS_SECURITY_DEFAULT  (SYS_SEC_FILE_RW | SYS_SEC_CONSOLE | SYS_SEC_GFX | SYS_SEC_IPC | \
                                   SYS_SEC_EXEC | SYS_SEC_ELEVATE)
#endif

// Every clearance, administration included (a trusted shell or launcher)
#define PROCESS_SECURITY_ALL      SYS_SEC_VALID

#define PROCESS_INVALID_ID          0     // Reserved: "the kernel" / no process

// Error codes (all negative, so any ProcessId > 0 is a success)
#define PROCESS_OK                  0
#define PROCESS_ERR_INVALID_ARG    -1
#define PROCESS_ERR_NO_SLOTS       -2
#define PROCESS_ERR_NO_MEMORY      -3
#define PROCESS_ERR_NO_PAGING      -4
#define PROCESS_ERR_LOAD_FAILED    -5     // The executable could not be read
#define PROCESS_ERR_NOT_FOUND      -6
#define PROCESS_ERR_THREAD         -7
#define PROCESS_ERR_NOT_EXECUTABLE -8     // Not a file, or no VFS_PERMISSION_EXECUTE
#define PROCESS_ERR_BAD_IMAGE      -9     // Not ELF, or a malformed one
#define PROCESS_ERR_UNSUPPORTED   -10     // Valid ELF this kernel can't run (PIE, dynamic, TLS)
#define PROCESS_ERR_TOO_BIG       -11     // More than PROCESS_ARG_MAX / PROCESS_ARG_BYTES of arguments

typedef enum {
    PROCESS_STATE_FREE,         // Slot unused
    PROCESS_STATE_CREATING,     // Slot reserved, being built
    PROCESS_STATE_RUNNING,      // Fully constructed, threads scheduled
    PROCESS_STATE_DYING         // Being destructed
} ProcessState;

// The contents of /proc/<pid>, read as a plain file (vfs_open / vfs_read,
// or SYS_OPEN / SYS_READ from user space). Fixed layout, little endian;
// a reader should check magic and version, and the file size against
// sizeof(ProcessRecord). Copy this struct into user-space headers as is.
#define PROCESS_RECORD_MAGIC    0x434F5250u     // "PROC"
#define PROCESS_RECORD_VERSION  1
#define PROCESS_RECORD_NAME     16              // == KNODE_NAME_LENGTH_MAX

typedef struct {
    char     node_name[PROCESS_RECORD_NAME];    // Knode name, the decimal pid. Must stay first:
                                                // the knode layer reads every entry's name here
    uint32_t magic;                             // PROCESS_RECORD_MAGIC
    uint32_t version;                           // PROCESS_RECORD_VERSION
    int32_t  pid;
    uint32_t state;                             // ProcessState
    uint32_t priority;                          // ThreadPriority (base)
    uint32_t security;                          // SYS_SEC_* mask
    uint32_t thread_count;
    uint32_t entry;                             // User address of the entry point
    uint32_t code_size;                         // As in ProcessInfo
    uint32_t data_size;
    uint32_t bss_size;
    uint32_t heap_size;
    char     name[PROCESS_NAME_MAX];
    char     image_path[PROCESS_PATH_MAX];      // "<memory>" for a flat image
} __attribute__((packed)) ProcessRecord;

_Static_assert(sizeof(ProcessRecord) == 16 + 12 * 4 + PROCESS_NAME_MAX + PROCESS_PATH_MAX,
               "ProcessRecord layout");

// Read-only snapshot for debugging / task list.
typedef struct {
    ProcessId pid;
    ProcessState state;
    ThreadPriority priority;
    char name[PROCESS_NAME_MAX];
    uint32_t thread_count;
    uint32_t code_size;         // File bytes in executable segments (flat: the whole image)
    uint32_t data_size;         // File bytes in other segments (flat: 0)
    uint32_t bss_size;          // Zero-filled bytes
    uint32_t heap_size;         // Bytes between the heap start and the break
    ProcessSecurity security;   // SYS_SEC_* mask
} ProcessInfo;

// A flat binary to run. It is copied to PROCESS_IMAGE_BASE and must be built
// (or written position independent) for that address.
typedef struct {
    const void* data;           // Image bytes (kernel memory)
    uint32_t    size;           // Bytes in data
    uint32_t    memory_size;    // Bytes to map, >= size; the rest is zeroed (0 = size)
    uint32_t    entry_offset;   // Entry point, relative to PROCESS_IMAGE_BASE
} ProcessImage;

// Create a process from a flat image and start its main thread in ring 3 at
// PROCESS_IMAGE_BASE + entry_offset, ESP near PROCESS_STACK_TOP. name labels
// it in reports (truncated to PROCESS_NAME_MAX - 1). Its working directory
// is "/". Returns a ProcessId (> 0) or a PROCESS_ERR_* code.
ProcessId CreateProcessFromImage(const char* name, const ProcessImage* image, ThreadPriority priority);

// Load the ELF executable at image_path (absolute VFS path) and start its
// main thread in ring 3 at e_entry. The process is named after the file and
// starts in the directory holding it ("/mnt/ata0p1/bin/hello" starts in
// "/mnt/ata0p1/bin"). Same as CreateProcessIn(image_path, NULL, priority).
// Reads the file through the VFS, so: thread context only, and never while
// holding a lock other threads wait on for long.
// Returns a ProcessId (> 0) or a PROCESS_ERR_* code; the reason a load was
// refused is also written to COM1.
ProcessId CreateProcess(const char* image_path, ThreadPriority priority);

// CreateProcess with a chosen working directory. cwd is an absolute path
// (normalized for you); NULL or "" means the directory holding the
// executable. A cwd that does not exist or is not a directory is not an
// error: the process starts in "/" and COM1 says why.
ProcessId CreateProcessIn(const char* image_path, const char* cwd, ThreadPriority priority);

// The same two loaders with an explicit security mask (SYS_SEC_* bits; bits
// outside SYS_SEC_VALID are PROCESS_ERR_INVALID_ARG). SYS_SPAWN creates
// processes from user space, but never with a bit its caller lacks, so
// SYS_SEC_ADMIN still only spreads from processes the kernel made admin.
ProcessId CreateProcessWithSecurity(const char* image_path, const char* cwd, ThreadPriority priority,
                                    ProcessSecurity security);
ProcessId CreateProcessFromImageWithSecurity(const char* name, const ProcessImage* image,
                                             ThreadPriority priority, ProcessSecurity security);

// Everything CreateProcessWithSecurity() takes, plus the arguments
typedef struct {
    const char*        image_path;  // Absolute VFS path of the ELF executable
    const char*        cwd;         // As CreateProcessIn(): NULL / "" = the executable's directory
    ThreadPriority     priority;
    ProcessSecurity    security;    // SYS_SEC_* mask
    uint32_t           argc;        // 0: argv = { image_path }
    const char* const* argv;        // argc strings (kernel memory), copied onto the new stack
    bool               elevated;    // Admin handed down from an elevated process (sys_spawn)
    void*              tty;         // Pipe end for descriptors 0-2 (sys_spawn_tty), or NULL
} ProcessLaunch;

// Load and start an ELF executable with arguments. Same rules and errors as
// CreateProcessWithSecurity(), plus PROCESS_ERR_TOO_BIG for too many
// arguments. argv only needs to live until this returns.
ProcessId CreateProcessLaunch(const ProcessLaunch* launch);

// Close the process's open files, then kill every thread of the process and
// free its address space. Thread context with interrupts on: closing files
// takes the VFS lock and may wait (up to SYS_FILES_EXIT_WAIT_MS) for one of
// its threads to finish a file call. Safe to call on the calling thread's
// own process (does not return then).
//
// If a thread of the process is inside a syscall (and so may hold kernel
// locks), the kill is deferred instead: the process is marked, its sleeps
// and IPC waits are cut short, and it ends itself with PROCESS_EXIT_KILLED
// as that syscall returns. PROCESS_OK then means "will end"; the slot is
// freed once it has (ProcessGetInfo() starts failing).
// Returns PROCESS_OK or a PROCESS_ERR_* code.
int DestroyProcess(ProcessId pid);

// Exit code of a process ended by a deferred DestroyProcess()
#define PROCESS_EXIT_KILLED  137

// Fill *info for a live process. Returns false if pid is not valid.
bool ProcessGetInfo(ProcessId pid, ProcessInfo* info);

// Read a running process's security mask. False if pid is not running.
bool ProcessGetSecurity(ProcessId pid, ProcessSecurity* security);

// Replace a running process's security mask. Kernel side: NO permission
// check (SYS_SEC_SET does that before calling here). The new mask applies
// from the process's next syscall. Logged on COM1. Stores the old mask in
// *previous when previous is not NULL. Returns PROCESS_OK,
// PROCESS_ERR_INVALID_ARG (bits outside SYS_SEC_VALID) or
// PROCESS_ERR_NOT_FOUND (pid not running).
int ProcessSetSecurity(ProcessId pid, ProcessSecurity security, ProcessSecurity* previous);

// Add `bits` to a running process's mask (kernel side, NO permission check),
// read and written under one lock. `elevated` also marks the process as
// elevated: SYS_ELEVATE's grant, after the user said yes. Logged on COM1.
// PROCESS_OK, PROCESS_ERR_INVALID_ARG or PROCESS_ERR_NOT_FOUND.
int ProcessGrantSecurity(ProcessId pid, ProcessSecurity bits, bool elevated);

// True when the process holds admin through SYS_ELEVATE (or a parent that
// did). False for a pid that is not live.
bool ProcessIsElevated(ProcessId pid);

// The kernel's own record of a running process: its name (the executable's
// file name) and the absolute path it was loaded from. Never the program's
// say. False if pid is not running.
bool ProcessGetImage(ProcessId pid, char name[PROCESS_NAME_MAX], char path[PROCESS_PATH_MAX]);

// Number of live (creating or running) processes
uint32_t process_get_count(void);

// Short, static, human-readable text for a PROCESS_ERR_* code
const char* process_error_string(int code);

//
// Calling thread's process (syscalls, fault handling)
//

// PROCESS_INVALID_ID for a kernel thread
ProcessId process_current_id(void);

// DestroyProcess() was asked for the calling thread's process while it was
// inside the kernel (see DestroyProcess). Long waits in a syscall should
// give up when this is true; syscall_dispatch() then ends the process.
bool process_current_kill_pending(void);

// Security mask of the calling thread's process. SYS_SEC_NONE for a kernel
// thread or a process that is being destroyed.
ProcessSecurity process_current_security(void);

// ProcessIsElevated() for the calling thread's process (false: kernel thread)
bool process_current_elevated(void);

// File-system clearance of the calling thread (the VFS's clearance source,
// installed by syscall_init): VFS_CLEARANCE_ALL (every bit) for a kernel
// thread, otherwise the process's security mask (SYS_SEC_NONE while it is
// being destroyed). A file whose basefs `security` mask has bits this lacks
// is refused, so a process without SYS_SEC_ADMIN cannot touch files marked
// VFS_SECURITY_ADMIN.
uint32_t process_current_clearance(void);

// Address space of the calling thread's process, NULL for a kernel thread
struct AddressSpace* process_current_space(void);

// Move the calling process's heap break by increment bytes (negative
// shrinks) and store the previous break in *previous. Whole pages are mapped
// (zeroed) or unmapped as the break crosses page boundaries. Returns
// PROCESS_OK, PROCESS_ERR_NO_MEMORY (out of memory or past PROCESS_HEAP_MAX),
// PROCESS_ERR_INVALID_ARG (below the heap start) or PROCESS_ERR_NOT_FOUND
// (not a process thread).
int process_sbrk(int32_t increment, uint32_t* previous);

// End the calling thread's process with an exit code. Does not return when
// the caller belongs to a process; returns PROCESS_ERR_NOT_FOUND otherwise.
int process_exit_current(int32_t code);

// Called first by the exception, #GP and #PF stubs. A fault raised in ring 3
// ((cs & 3) == 3) is reported on COM1 and kills only the faulting process
// (exit code 128 + vector); this does not return then. A kernel fault
// returns at once so the caller can panic as before.
void process_user_fault(uint32_t vector, uint32_t error_code, uint32_t eip,
                        uint32_t cs, uint32_t address);

#endif

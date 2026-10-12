#ifndef _KERNEL_PROCESS_H_
#define _KERNEL_PROCESS_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/scheduler/scheduler.h>

/*
 * Processes
 *
 * A process owns a private user address space (page directory), the image
 * and stack mapped into it, and a list of threads. Its threads run in ring 3
 * and reach the kernel only through interrupts, faults and syscalls
 * (int 0x81, see syscall/syscall.h).
 *
 * User address space layout (vmm.h has the full map):
 *
 *   0x40000000 - 0x4FFFFFFF    image area (PROCESS_IMAGE_MAX). A flat image
 *                              sits at PROCESS_IMAGE_BASE; an ELF executable's
 *                              PT_LOAD segments go wherever they were linked,
 *                              as long as they stay inside this area
 *   next page after the image  heap: grows up with sbrk, at most
 *                              PROCESS_HEAP_MAX bytes
 *   ...                        unmapped
 *   0xBFFF0000 - 0xBFFFFFFF    PROCESS_STACK_PAGES of stack, top at 0xC0000000
 *                              (the unmapped page below it catches overflow)
 *
 * Everything below 0x40000000 and from 0xC0000000 up is kernel, mapped
 * supervisor-only: a user access there page-faults and kills the process.
 *
 * ProcessId layout: [ generation (23 bits) | slot (8 bits) ], always > 0.
 * The generation is bumped every time a slot is freed, so a stale id held
 * after DestroyProcess() can never refer to a newer process that happened
 * to reuse the same slot.
 *
 * Loading:
 *   CreateProcess()           an ELF32 i386 executable from the VFS
 *                             (scheduler/elf.h). The file must carry
 *                             VFS_PERMISSION_EXECUTE. Build programs static
 *                             and non-PIE, linked at 0x40000000.
 *   CreateProcessFromImage()  a flat binary already in kernel memory, mapped
 *                             read/write at PROCESS_IMAGE_BASE.
 */

#define PROCESS_MAX                64     // Must be <= 256 (8-bit slot field)
#define PROCESS_NAME_MAX           16
#define PROCESS_PATH_MAX          256

#define PROCESS_IMAGE_BASE        0x40000000u            // == VM_USER_START
#define PROCESS_IMAGE_MAX         (256u * 1024u * 1024u) // Size of the image area
#define PROCESS_STACK_TOP         0xC0000000u            // == VM_USER_STACK_TOP
#define PROCESS_STACK_PAGES       16                     // 64 KB

// Per-process heap limit. There is no global memory accounting yet, and the
// kernel panics when vmm_alloc_pages() runs dry (user page tables come from
// it), so this keeps one process from exhausting the machine.
#define PROCESS_HEAP_MAX          (64u * 1024u * 1024u)

typedef int32_t ProcessId;

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

typedef enum {
    PROCESS_STATE_FREE,         // Slot unused
    PROCESS_STATE_CREATING,     // Slot reserved, being built
    PROCESS_STATE_RUNNING,      // Fully constructed, threads scheduled
    PROCESS_STATE_DYING         // Being destructed
} ProcessState;

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
// it in reports (truncated to PROCESS_NAME_MAX - 1). Returns a ProcessId (> 0)
// or a PROCESS_ERR_* code.
ProcessId CreateProcessFromImage(const char* name, const ProcessImage* image, ThreadPriority priority);

// Load the ELF executable at image_path (absolute VFS path) and start its
// main thread in ring 3 at e_entry. The process is named after the file.
// Reads the file through the VFS, so: thread context only, and never while
// holding the DWM lock for long (the DWM launches from a worker thread).
// Returns a ProcessId (> 0) or a PROCESS_ERR_* code; the reason a load was
// refused is also written to COM1.
ProcessId CreateProcess(const char* image_path, ThreadPriority priority);

// Kill every thread of the process and free its address space.
// Safe to call on the calling thread's own process (does not return then).
// Returns PROCESS_OK or a PROCESS_ERR_* code.
int DestroyProcess(ProcessId pid);

// Fill *info for a live process. Returns false if pid is not valid.
bool ProcessGetInfo(ProcessId pid, ProcessInfo* info);

// Number of live (creating or running) processes
uint32_t process_get_count(void);

// Short, static, human-readable text for a PROCESS_ERR_* code
const char* process_error_string(int code);

//
// Calling thread's process (syscalls, fault handling)
//

// PROCESS_INVALID_ID for a kernel thread
ProcessId process_current_id(void);

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

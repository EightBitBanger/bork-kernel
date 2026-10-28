#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/scheduler/process.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/resources/classes/elf.h>
#include <kernel/mutex.h>
#include <kernel/knode.h>

#include <kernel/arch/x86/irq.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/arch/x86/drivers/serial.h>

#include <kernel/vfs/vfs.h>
#include <kernel/syscall/syscall.h>

#include <kernel/util/list.h>
#include <kernel/util/string.h>

#define CR0_PG              0x80000000u

#define PID_SLOT_BITS       8
#define PID_SLOT_MASK       0xFFu
#define PID_GEN_MASK        0x007FFFFFu     // 23 bits: keeps the id positive

// Initial ESP: 16 bytes below the top, so ESP % 16 == 0 at the entry point
#define PROCESS_INITIAL_ESP (PROCESS_STACK_TOP - 16u)

#define PROCESS_STACK_BASE  (PROCESS_STACK_TOP - PROCESS_STACK_PAGES * PAGE_SIZE)

// End of the area ELF segments may occupy
#define PROCESS_IMAGE_LIMIT (PROCESS_IMAGE_BASE + PROCESS_IMAGE_MAX)

typedef char process_max_fits_slot_field[(PROCESS_MAX <= 256) ? 1 : -1];

_Static_assert(PROCESS_IMAGE_BASE == VM_USER_START, "image must start at the bottom of user space");
_Static_assert(PROCESS_STACK_TOP == VM_USER_STACK_TOP, "stack must end at the top of user space");
_Static_assert((PROCESS_IMAGE_LIMIT & (PAGE_SIZE - 1)) == 0, "image area must end on a page");
_Static_assert(PROCESS_IMAGE_LIMIT + PROCESS_HEAP_MAX < PROCESS_STACK_BASE - PAGE_SIZE,
               "image, heap and stack (plus its guard page) overlap");

// An executable's directory always fits a syscall path
_Static_assert(PROCESS_PATH_MAX <= SYS_PATH_MAX, "image paths must fit SYS_PATH_MAX");

// One mapped region of a process's user address space
typedef struct {
    uint32_t base;      // User virtual address, page aligned (0 = none)
    uint32_t size;      // Bytes in use
    uint32_t pages;     // Pages spanned
} UserRegion;

typedef struct {
    ProcessState state;
    uint32_t generation;
    ProcessId pid;

    ThreadPriority priority;
    ProcessSecurity security;   // SYS_SEC_* mask, checked by every syscall
    bool elevated;              // SYS_SEC_ADMIN came from the user's yes (SYS_ELEVATE),
                                // or from a parent that had it that way
    char name[PROCESS_NAME_MAX];
    char image_path[PROCESS_PATH_MAX];

    // Address space: kernel half shared, user half (0x40000000-0xBFFFFFFF) private
    AddressSpace* space;
    uint32_t cr3;               // Physical address of the directory, loaded per thread

    // Image and main thread stack, both inside the user half. For an ELF
    // image, `image` spans the lowest to the highest segment; pages between
    // segments may be unmapped.
    UserRegion image;
    uint32_t code_bytes;        // File bytes in executable segments
    uint32_t data_bytes;        // File bytes in other segments
    uint32_t bss_bytes;         // Zero-filled bytes
    UserRegion heap;            // Starts on the page after the image; size = break - base
    UserRegion stack;
    uint32_t entry;             // User virtual address of the entry point

    // Threads (list payloads are ThreadBlock*)
    struct list_node* threads_head;
    struct list_node* threads_tail;
    uint32_t thread_count;

    // Kernel node of /proc/<pid> (NULL: none). Set and cleared with the VFS
    // lock held, which also guards the knode tree.
    void* proc_node;

    // DestroyProcess() from another thread found one of our threads inside
    // the kernel: it ends the process itself when it leaves (interrupts off
    // to touch)
    bool kill_pending;
} Process;

// Fills in p->image, p->entry and the byte counts, mapping the image into
// p->space. Returns PROCESS_OK or a PROCESS_ERR_* code; on failure the
// caller destroys the space (and whatever was mapped into it).
typedef int (*ProcessLoadFn)(Process* p, void* ctx);

extern ThreadBlock* current_thread;

static Process process_table[PROCESS_MAX];
static uint32_t process_count = 0;

// CPU helpers

static inline uint32_t read_cr0(void) {
    uint32_t value;
    __asm__ volatile ("mov %%cr0, %0" : "=r" (value));
    return value;
}

static inline void write_cr3(uint32_t value) {
    __asm__ volatile ("mov %0, %%cr3" : : "r" (value) : "memory");
}

static void copy_string(char* dst, const char* src, uint32_t capacity) {
    uint32_t i = 0;
    if (src != NULL) {
        for (; i + 1 < capacity && src[i] != '\0'; i++)
            dst[i] = src[i];
    }
    dst[i] = '\0';
}

// Directory holding an executable: "/mnt/ata0p1/bin/hello" -> "/mnt/ata0p1/bin",
// "/hello" -> "/". Anything that is not an absolute path gives "/".
static void process_image_directory(const char* image_path, char out[PROCESS_PATH_MAX]) {
    if (image_path == NULL || image_path[0] != '/') {
        copy_string(out, "/", PROCESS_PATH_MAX);
        return;
    }
    copy_string(out, image_path, PROCESS_PATH_MAX);

    char* slash = strrchr(out, '/');
    if (slash == out)
        out[1] = '\0';
    else
        *slash = '\0';
}

// Handles

static ProcessId pid_encode(uint32_t generation, uint32_t slot) {
    return (ProcessId)(((generation & PID_GEN_MASK) << PID_SLOT_BITS) | (slot & PID_SLOT_MASK));
}

// Caller must hold the lock (interrupts disabled)
static Process* process_lookup(ProcessId pid) {
    if (pid <= 0)
        return NULL;
    uint32_t slot = (uint32_t)pid & PID_SLOT_MASK;
    if (slot >= PROCESS_MAX)
        return NULL;
    Process* p = &process_table[slot];
    if (p->state == PROCESS_STATE_FREE || p->pid != pid)
        return NULL;
    return p;
}

// Slot management

// Reserve a free slot (state CREATING). NULL when the table is full.
static Process* process_reserve(void) {
    Process* p = NULL;
    uint32_t irq = irq_save();
    for (uint32_t slot = 0; slot < PROCESS_MAX; slot++) {
        if (process_table[slot].state == PROCESS_STATE_FREE) {
            p = &process_table[slot];
            if (p->generation == 0)
                p->generation = 1;
            p->state = PROCESS_STATE_CREATING;
            p->pid = pid_encode(p->generation, slot);
            process_count++;
            break;
        }
    }
    irq_restore(irq);
    return p;
}

// Caller must hold the lock
static void process_release_slot(Process* p) {
    uint32_t generation = (p->generation + 1) & PID_GEN_MASK;
    if (generation == 0)
        generation = 1;

    memset(p, 0, sizeof(*p));
    p->generation = generation;
    p->state = PROCESS_STATE_FREE;
    process_count--;
}

// Frees every user page, the page tables and the directory. If the space is
// the loaded one, the VMM switches CR3 to the kernel directory first.
static void process_free_address_space(Process* p) {
    if (p->space != NULL)
        vmm_space_destroy(p->space);
    p->space = NULL;
    p->cr3 = 0;
    memset(&p->image, 0, sizeof(p->image));
    memset(&p->heap, 0, sizeof(p->heap));
    memset(&p->stack, 0, sizeof(p->stack));
}

// Threads

static void thread_detach_from_event(ThreadBlock* t) {
    Event* event = t->wait_event;
    if (event == NULL)
        return;

    ThreadBlock** link = &event->waiters;
    while (*link != NULL) {
        if (*link == t) {
            *link = t->wait_next;
            break;
        }
        link = &(*link)->wait_next;
    }
    t->wait_event = NULL;
    t->wait_next = NULL;
}

// A thread of `p` is inside the kernel on its behalf. Interrupts off.
static bool process_in_kernel(const Process* p) {
    for (struct list_node* n = p->threads_head; n != NULL; n = n->next) {
        const ThreadBlock* t = (const ThreadBlock*)n->data;
        if (t->in_syscall)
            return true;
    }
    return false;
}

// Cut short the waits of `p`'s threads inside the kernel, so a pending kill
// is not held up by a long sleep or an IPC wait. Mutex waits are left alone:
// the lock comes, the call finishes and the thread ends. Interrupts off.
static void process_interrupt_waits(Process* p) {
    for (struct list_node* n = p->threads_head; n != NULL; n = n->next) {
        ThreadBlock* t = (ThreadBlock*)n->data;
        if (!t->in_syscall || t->wait_mutex != NULL)
            continue;
        if (t->state == THREAD_SLEEPING || t->state == THREAD_BLOCKED) {
            thread_detach_from_event(t);
            t->wait_signaled = false;   // To event_wait() this looks like a timeout
            t->state = THREAD_READY;
        }
    }
}

// Caller must hold the lock
static void process_kill_threads(Process* p) {
    while (p->threads_head != NULL) {
        ThreadBlock* t = (ThreadBlock*)p->threads_head->data;

        thread_detach_from_event(t);            // Don't leave it in an event's wait list
        mutex_cancel_wait(t);                   // ...or in a mutex's (its waiter node is on the freed stack)
        t->cr3 = vmm_kernel_directory_phys();   // Its own directory is about to be freed
        t->state = THREAD_DEAD;                 // The scheduler's reaper frees the stack

        list_remove(&p->threads_head, &p->threads_tail, t);
    }
    p->thread_count = 0;
}

// Reporting (COM1)

// Copy a live process's name ("" when pid is not valid)
static void process_copy_name(ProcessId pid, char name[PROCESS_NAME_MAX]) {
    name[0] = '\0';

    uint32_t irq = irq_save();
    Process* p = process_lookup(pid);
    if (p != NULL)
        memcpy(name, p->name, PROCESS_NAME_MAX);
    irq_restore(irq);
}

// "[process] name (pid N) "
static void process_log_prefix(ProcessId pid) {
    char name[PROCESS_NAME_MAX];
    process_copy_name(pid, name);
    
    serial_puts("[process] ");
    serial_puts(name[0] ? name : "?");
    serial_puts(" (pid ");
    serial_put_dec((uint32_t)pid);
    serial_puts(") ");
}

// /proc
//
// Every process is a kernel-node file /proc/<pid>: one ProcessRecord
// (process.h), stored straight in a kmalloc block and linked into the
// /proc knode the way /dev links its device blocks. Like any knode, its
// name sits in the first 16 bytes; the VFS lists it, stats it and reads
// it as a plain read-only file.
//
// The VFS cannot make such a file (vfs_open creates only on storage
// devices, and /proc is read-only), so this talks to the knode layer
// directly, holding the VFS lock: it guards the knode tree and the open
// handles that may point into a record.
//
// Made before the main thread exists, refreshed when the state, thread
// count, security mask or heap changes, and removed by DestroyProcess()
// (the one place every process ends). If there is no /proc, processes
// still run unlisted (reported once on COM1).

static void* proc_dir     = NULL;   // /proc knode, found on first use
static bool proc_warned   = false;

static inline bool interrupts_enabled(void) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}

// Caller holds the VFS lock
static void* proc_directory(void) {
    if (proc_dir == NULL) {
        void* dir = knode_find_by_name(knode_get_root(), "proc");
        if (knode_check_is_valid(dir))
            proc_dir = dir;
    }
    return proc_dir;
}

static void proc_node_name(ProcessId pid, char out[PROCESS_RECORD_NAME]) {
    char digits[10];
    uint32_t v = (uint32_t)pid;
    int n = 0;
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v != 0);

    memset(out, 0, PROCESS_RECORD_NAME);
    for (int i = 0; i < n; i++)
        out[i] = digits[n - 1 - i];
}

// Snapshot of `p` (interrupts off, or the slot is ours)
static void proc_fill(const Process* p, ProcessRecord* r) {
    memset(r, 0, sizeof(*r));
    proc_node_name(p->pid, r->node_name);
    r->magic        = PROCESS_RECORD_MAGIC;
    r->version      = PROCESS_RECORD_VERSION;
    r->pid          = p->pid;
    r->state        = (uint32_t)p->state;
    r->priority     = (uint32_t)p->priority;
    r->security     = p->security;
    r->thread_count = p->thread_count;
    r->entry        = p->entry;
    r->code_size    = p->code_bytes;
    r->data_size    = p->data_bytes;
    r->bss_size     = p->bss_bytes;
    r->heap_size    = p->heap.size;
    memcpy(r->name, p->name, PROCESS_NAME_MAX);
    memcpy(r->image_path, p->image_path, PROCESS_PATH_MAX);
}

// Make /proc/<pid>. The slot is CREATING (ours), interrupts are on.
// A failure is not fatal: the process just has no entry.
static void proc_publish(Process* p) {
    ProcessRecord record;
    proc_fill(p, &record);

    vfs_lock();

    void* dir  = proc_directory();
    void* node = NULL;
    if (dir != NULL)
        node = kmalloc(sizeof(ProcessRecord));

    if (node != NULL) {
        memcpy(node, &record, sizeof(record));
        kmalloc_set_permissions(node, KMALLOC_PERMISSION_READ);
        if (knode_add_reference(dir, node)) {
            uint32_t irq = irq_save();
            p->proc_node = node;
            irq_restore(irq);
        } else {
            kfree(node);
            node = NULL;
        }
    }

    vfs_unlock();

    if (node == NULL && !proc_warned) {
        proc_warned = true;
        process_log_prefix(p->pid);
        serial_puts(dir == NULL ? "not listed: no /proc\n"
                             : "not listed: /proc entry could not be made\n");
    }
}

// Take /proc/<pid> away. Interrupts on.
static void proc_unpublish(ProcessId pid) {
    vfs_lock();

    void* node = NULL;
    uint32_t irq = irq_save();
    Process* p = process_lookup(pid);
    if (p != NULL) {
        node = p->proc_node;
        p->proc_node = NULL;
    }
    irq_restore(irq);

    if (node != NULL) {
        vfs_knode_revoke_handles(node);     // No open handle may point into it
        knode_remove_reference(proc_dir, node);
        kfree(node);
    }

    vfs_unlock();
}

// Rewrite /proc/<pid> from the Process. The record never changes size, so
// this is one write in place. Skipped with interrupts off (the VFS lock
// may sleep): the entry is then a little stale until the next refresh.
static void proc_refresh(ProcessId pid) {
    if (!interrupts_enabled())
        return;

    ProcessRecord record;
    void* node = NULL;

    vfs_lock();

    uint32_t irq = irq_save();
    Process* p = process_lookup(pid);
    if (p != NULL && p->proc_node != NULL) {
        node = p->proc_node;
        proc_fill(p, &record);
    }
    irq_restore(irq);

    // proc_node only changes under the VFS lock, so `node` is still live
    if (node != NULL)
        memcpy(node, &record, sizeof(record));

    vfs_unlock();
}

// Loaders

// Flat image: one read/write/execute region at PROCESS_IMAGE_BASE. The
// ProcessImage was validated by CreateProcessFromImage().
static int process_load_flat(Process* p, void* ctx) {
    const ProcessImage* image = (const ProcessImage*)ctx;
    uint32_t memory_size = image->memory_size ? image->memory_size : image->size;

    // Map zeroed pages, then copy the bytes in. The copy goes through the
    // VMM's scratch window, so the space does not need to be loaded.
    p->image.base  = PROCESS_IMAGE_BASE;
    p->image.size  = memory_size;
    p->image.pages = (memory_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!vmm_user_alloc(p->space, p->image.base, p->image.pages, VM_READWRITE))
        return PROCESS_ERR_NO_MEMORY;
    if (!vmm_user_copy_to(p->space, p->image.base, image->data, image->size))
        return PROCESS_ERR_LOAD_FAILED;

    p->code_bytes = image->size;
    p->data_bytes = 0;
    p->bss_bytes  = memory_size - image->size;
    p->entry      = p->image.base + image->entry_offset;
    return PROCESS_OK;
}

typedef struct {
    File     file;
    uint32_t size;
} ElfSource;

// ELF executable: every PT_LOAD segment where it was linked (resources/classes/elf.c)
static int process_load_elf(Process* p, void* ctx) {
    const ElfSource* src = (const ElfSource*)ctx;
    ElfImageInfo info;

    ElfResult result = elf_load(p->space, src->file, src->size,
                                PROCESS_IMAGE_BASE, PROCESS_IMAGE_LIMIT, &info);
    if (result != ELF_OK) {
        process_log_prefix(p->pid);
        serial_puts("not loaded: ");
        serial_puts(info.error ? info.error : elf_result_string(result));
        serial_puts("\n");

        switch (result) {
            case ELF_ERR_NO_MEMORY:   return PROCESS_ERR_NO_MEMORY;
            case ELF_ERR_IO:          return PROCESS_ERR_LOAD_FAILED;
            case ELF_ERR_UNSUPPORTED: return PROCESS_ERR_UNSUPPORTED;
            default:                  return PROCESS_ERR_BAD_IMAGE;
        }
    }

    p->image.base  = info.image_start;
    p->image.size  = info.image_end - info.image_start;
    p->image.pages = p->image.size / PAGE_SIZE;
    p->code_bytes  = info.code_bytes;
    p->data_bytes  = info.data_bytes;
    p->bss_bytes   = info.bss_bytes;
    p->entry       = info.entry;

    process_log_prefix(p->pid);
    serial_puts("ELF ");
    serial_put_dec(info.segment_count);
    serial_puts(" segments, ");
    serial_put_hex(info.image_start);
    serial_puts(" - ");
    serial_put_hex(info.image_end);
    serial_puts("\n");
    return PROCESS_OK;
}

// Arguments
//
// Written to the top of the main thread's user stack before it exists:
//
//   esp ->  argc
//           argv[0] .. argv[argc - 1]   (user addresses of the strings)
//           NULL                        (end of argv)
//           NULL                        (envp: empty for now)
//           ... alignment padding ...
//           the strings, back to back
//           16 bytes of zeros up to PROCESS_STACK_TOP
//
// esp is 16-byte aligned. The stack pages are zeroed, so a program that
// ignores the arguments reads argc as 0.
static int process_push_args(Process* p, uint32_t argc, const char* const* argv, uint32_t* esp_out) {
    if (argc > PROCESS_ARG_MAX)
        return PROCESS_ERR_TOO_BIG;
    if (argc > 0 && argv == NULL)
        return PROCESS_ERR_INVALID_ARG;

    uint32_t bytes = 0;
    for (uint32_t i = 0; i < argc; i++) {
        if (argv[i] == NULL)
            return PROCESS_ERR_INVALID_ARG;
        uint32_t n = (uint32_t)strnlen(argv[i], PROCESS_ARG_BYTES) + 1;
        if (n > PROCESS_ARG_BYTES - bytes)
            return PROCESS_ERR_TOO_BIG;
        bytes += n;
    }

    uint32_t words   = argc + 3;
    uint32_t strings = (PROCESS_INITIAL_ESP - bytes) & ~15u;
    uint32_t esp     = (strings - words * 4u) & ~15u;

    uint32_t vector[PROCESS_ARG_MAX + 3];
    uint32_t at = strings;
    vector[0] = argc;
    for (uint32_t i = 0; i < argc; i++) {
        uint32_t n = (uint32_t)strlen(argv[i]) + 1;
        if (!vmm_user_copy_to(p->space, at, argv[i], n))
            return PROCESS_ERR_NO_MEMORY;
        vector[1 + i] = at;
        at += n;
    }
    vector[argc + 1] = 0;
    vector[argc + 2] = 0;

    if (!vmm_user_copy_to(p->space, esp, vector, words * 4u))
        return PROCESS_ERR_NO_MEMORY;
    *esp_out = esp;
    return PROCESS_OK;
}

_Static_assert(PROCESS_ARG_BYTES + (PROCESS_ARG_MAX + 3) * 4 + 64 < PROCESS_STACK_PAGES * PAGE_SIZE,
               "arguments must leave room on the stack");

// Build

// Reserve a slot, create the address space, let `load` put the image in it,
// then add the heap, the stack (with argc / argv on it), the file table
// (working directory `cwd`) and the main thread.
static ProcessId process_build(const char* name, const char* path, const char* cwd,
                               ThreadPriority priority, ProcessSecurity security, bool elevated,
                               uint32_t argc, const char* const* argv, void* tty,
                               ProcessLoadFn load, void* ctx) {
    if (priority < PRIORITY_IDLE || priority > PRIORITY_REALTIME)
        return PROCESS_ERR_INVALID_ARG;
    if ((security & ~SYS_SEC_VALID) != 0)
        return PROCESS_ERR_INVALID_ARG;
    if ((read_cr0() & CR0_PG) == 0)
        return PROCESS_ERR_NO_PAGING;

    Process* p = process_reserve();
    if (p == NULL)
        return PROCESS_ERR_NO_SLOTS;

    // The slot is reserved (CREATING), so it can be filled in without the lock
    int error = PROCESS_OK;
    bool files_ready = false;
    uint32_t irq;
    p->priority = priority;
    p->security = security;         // Before the main thread exists: its first syscall is checked
    p->elevated = elevated;         // ... and so is this (sys_spawn, SYS_ELEVATE_AGENT)
    copy_string(p->name, (name && name[0]) ? name : "process", PROCESS_NAME_MAX);
    copy_string(p->image_path, path, PROCESS_PATH_MAX);

    // Address space: kernel half shared, user half empty
    p->space = vmm_space_create();
    if (p->space == NULL) {
        error = PROCESS_ERR_NO_MEMORY;
        goto fail;
    }
    p->cr3 = p->space->directory_phys;

    // Image
    error = load(p, ctx);
    if (error != PROCESS_OK)
        goto fail;

    // Heap: empty until the program calls sbrk
    p->heap.base  = p->image.base + p->image.pages * PAGE_SIZE;
    p->heap.size  = 0;
    p->heap.pages = 0;

    // Main thread stack
    p->stack.base  = PROCESS_STACK_BASE;
    p->stack.pages = PROCESS_STACK_PAGES;
    p->stack.size  = PROCESS_STACK_PAGES * PAGE_SIZE;
    if (!vmm_user_alloc(p->space, p->stack.base, p->stack.pages, VM_READWRITE)) {
        error = PROCESS_ERR_NO_MEMORY;
        goto fail;
    }

    // Arguments at the top of that stack; the main thread starts below them
    uint32_t initial_esp = PROCESS_INITIAL_ESP;
    error = process_push_args(p, argc, argv, &initial_esp);
    if (error != PROCESS_OK)
        goto fail;

    // Descriptor table, working directory and terminal. In place before the
    // main thread exists, so its very first file call already resolves
    // relative paths against `cwd`. (Interrupts on: this checks the
    // directory through the VFS.)
    if (!syscall_process_start((uint32_t)p->pid, p->space, cwd, tty)) {
        error = PROCESS_ERR_NO_MEMORY;
        goto fail;
    }
    files_ready = true;

    // /proc/<pid>, before any thread of the process can run (and end it)
    proc_publish(p);

    // Main thread. Interrupts stay off until the process is RUNNING, so the
    // thread cannot run (and, say, exit) before its process is complete.
    irq = irq_save();
    ThreadBlock* thread = thread_create_user(p->entry, initial_esp, priority,
                                             p->cr3, p->pid, p->name);
    if (thread != NULL) {
        if (list_append(&p->threads_head, &p->threads_tail, thread)) {
            p->thread_count = 1;
            p->state = PROCESS_STATE_RUNNING;
        } else {
            thread->cr3 = vmm_kernel_directory_phys();
            thread->state = THREAD_DEAD;
            thread = NULL;
        }
    }
    irq_restore(irq);

    if (thread == NULL) {
        error = PROCESS_ERR_THREAD;
        goto fail;
    }

    proc_refresh(p->pid);           // Now RUNNING, with its main thread

    process_log_prefix(p->pid);
    serial_puts("started, entry ");
    serial_put_hex(p->entry);
    serial_puts(", security ");
    serial_put_hex(security);
    serial_puts("\n");
    return p->pid;

fail:
    // No thread of this process ever ran, so the table is unused: this
    // only frees it (interrupts are on here)
    if (files_ready)
        syscall_process_exit((uint32_t)p->pid);
    proc_unpublish(p->pid);
    process_free_address_space(p);
    irq = irq_save();
    process_release_slot(p);
    irq_restore(irq);
    return error;
}

// Public API

ProcessId CreateProcessFromImage(const char* name, const ProcessImage* image, ThreadPriority priority) {
    return CreateProcessFromImageWithSecurity(name, image, priority, PROCESS_SECURITY_DEFAULT);
}

ProcessId CreateProcessFromImageWithSecurity(const char* name, const ProcessImage* image,
                                             ThreadPriority priority, ProcessSecurity security) {
    if (image == NULL || image->data == NULL || image->size == 0)
        return PROCESS_ERR_INVALID_ARG;

    uint32_t memory_size = image->memory_size ? image->memory_size : image->size;
    if (memory_size < image->size || memory_size > PROCESS_IMAGE_MAX)
        return PROCESS_ERR_INVALID_ARG;
    if (image->entry_offset >= memory_size)
        return PROCESS_ERR_INVALID_ARG;

    // No file to be next to: start at the top. argv = { name }.
    const char* arg0 = (name && name[0]) ? name : "process";
    return process_build(name, "<memory>", "/", priority, security, false, 1, &arg0, NULL,
                         process_load_flat, (void*)image);
}

ProcessId CreateProcessIn(const char* image_path, const char* cwd, ThreadPriority priority) {
    return CreateProcessWithSecurity(image_path, cwd, priority, PROCESS_SECURITY_DEFAULT);
}

ProcessId CreateProcessWithSecurity(const char* image_path, const char* cwd, ThreadPriority priority,
                                    ProcessSecurity security) {
    ProcessLaunch launch = { image_path, cwd, priority, security, 0, NULL, false, NULL };
    return CreateProcessLaunch(&launch);
}

ProcessId CreateProcessLaunch(const ProcessLaunch* launch) {
    if (launch == NULL)
        return PROCESS_ERR_INVALID_ARG;

    const char*     image_path = launch->image_path;
    const char*     cwd        = launch->cwd;
    ThreadPriority  priority   = launch->priority;
    ProcessSecurity security   = launch->security;

    // No arguments given: argv = { image_path }, as for every kernel launch
    uint32_t argc = launch->argc;
    const char* const* argv = launch->argv;
    if (argc == 0) {
        argc = 1;
        argv = &image_path;
    }
    if (argc > PROCESS_ARG_MAX)
        return PROCESS_ERR_TOO_BIG;
    if (argv == NULL)
        return PROCESS_ERR_INVALID_ARG;

    if (image_path == NULL || image_path[0] != '/')
        return PROCESS_ERR_INVALID_ARG;
    if (strnlen(image_path, PROCESS_PATH_MAX) >= PROCESS_PATH_MAX)
        return PROCESS_ERR_INVALID_ARG;
    if (priority < PRIORITY_IDLE || priority > PRIORITY_REALTIME)
        return PROCESS_ERR_INVALID_ARG;
    if ((security & ~SYS_SEC_VALID) != 0)
        return PROCESS_ERR_INVALID_ARG;

    // Only regular files marked executable may run. Every launcher goes
    // through here, so this is the one place it is enforced.
    if (!vfs_exists(image_path))
        return PROCESS_ERR_NOT_FOUND;

    uint8_t permissions = 0;
    if (vfs_directory_check(image_path) ||
        !vfs_get_permissions(image_path, &permissions) ||
        !(permissions & VFS_PERMISSION_EXECUTE))
        return PROCESS_ERR_NOT_EXECUTABLE;

    // Default working directory: where the program lives
    char image_dir[PROCESS_PATH_MAX];
    if (cwd == NULL || cwd[0] == '\0') {
        process_image_directory(image_path, image_dir);
        cwd = image_dir;
    }

    File file = vfs_open(image_path, VFS_OPEN_READ);
    if (file == VFS_INVALID_FILE)
        return PROCESS_ERR_LOAD_FAILED;

    ElfSource source = { file, vfs_get_size(file) };

    // Strip path from filename
    const char* name = strrchr(image_path, '/');
    name = (name != NULL) ? name + 1 : image_path;

    ProcessId pid = process_build(name, image_path, cwd, priority, security, launch->elevated, argc, argv,
                                  launch->tty, process_load_elf, &source);

    vfs_close(file);
    return pid;
}

ProcessId CreateProcess(const char* image_path, ThreadPriority priority) {
    return CreateProcessIn(image_path, NULL, priority);
}

int DestroyProcess(ProcessId pid) {
    uint32_t irq = irq_save();

    Process* p = process_lookup(pid);
    if (p == NULL || p->state != PROCESS_STATE_RUNNING) {
        irq_restore(irq);
        return PROCESS_ERR_NOT_FOUND;
    }
    // Another thread asking while one of ours is inside the kernel: that
    // thread may hold a kernel lock (VFS, IPC, a descriptor table...), and a
    // thread killed holding one leaves it locked for good. Let it finish the
    // call and end the process itself on the way out (syscall_dispatch).
    bool self = current_thread != NULL && current_thread->process_id == pid;
    if (!self && process_in_kernel(p)) {
        p->kill_pending = true;
        process_interrupt_waits(p);
        irq_restore(irq);
        return PROCESS_OK;
    }

    // DYING first: from here on process_current_space() returns NULL for this
    // pid, so its threads cannot start a new file call (sys_files_acquire
    // fails) and a second DestroyProcess() returns NOT_FOUND
    p->state = PROCESS_STATE_DYING;
    irq_restore(irq);

    // Close the process's files before its threads and memory go. This runs
    // with interrupts on: it takes the VFS lock and may sleep while a thread
    // of the process finishes a file call already in progress (bounded by
    // SYS_FILES_EXIT_WAIT_MS). Every way a process ends comes through here.
    syscall_process_exit((uint32_t)pid);

    // Its /proc entry goes next, also with interrupts on (VFS lock)
    proc_unpublish(pid);

    irq = irq_save();

    // Only DestroyProcess() frees a slot, and only from RUNNING, so the slot
    // is still ours (DYING, same pid)
    p = process_lookup(pid);
    if (p == NULL || p->state != PROCESS_STATE_DYING) {
        irq_restore(irq);
        return PROCESS_ERR_NOT_FOUND;
    }

    // Destroying our own process: get off its page directory before freeing it
    if (self)
        write_cr3(vmm_kernel_directory_phys());

    // Everything below runs with interrupts off. If we are one of the
    // threads being killed, a preemption here would never come back and the
    // process memory would leak.
    process_kill_threads(p);
    process_free_address_space(p);
    process_release_slot(p);

    irq_restore(irq);

    if (self) {
        // Our ThreadBlock is now DEAD; the scheduler drops us at the next switch
        for (;;) {
            thread_yield();
        }
    }
    return PROCESS_OK;
}

bool ProcessGetInfo(ProcessId pid, ProcessInfo* info) {
    if (info == NULL)
        return false;

    uint32_t irq = irq_save();
    Process* p = process_lookup(pid);
    if (p == NULL) {
        irq_restore(irq);
        return false;
    }

    info->pid = p->pid;
    info->state = p->state;
    info->priority = p->priority;
    memcpy(info->name, p->name, PROCESS_NAME_MAX);
    info->thread_count = p->thread_count;
    info->code_size = p->code_bytes;
    info->data_size = p->data_bytes;
    info->bss_size = p->bss_bytes;
    info->heap_size = p->heap.size;
    info->security = p->security;

    irq_restore(irq);
    return true;
}

bool ProcessGetSecurity(ProcessId pid, ProcessSecurity* security) {
    if (security == NULL)
        return false;

    uint32_t irq = irq_save();
    Process* p = process_lookup(pid);
    bool found = p != NULL && p->state == PROCESS_STATE_RUNNING;
    if (found)
        *security = p->security;
    irq_restore(irq);
    return found;
}

int ProcessSetSecurity(ProcessId pid, ProcessSecurity security, ProcessSecurity* previous) {
    if ((security & ~SYS_SEC_VALID) != 0)
        return PROCESS_ERR_INVALID_ARG;

    // One word, written with interrupts off: a syscall of the target reads
    // either the old mask or the new one, never a mix
    uint32_t irq = irq_save();
    Process* p = process_lookup(pid);
    if (p == NULL || p->state != PROCESS_STATE_RUNNING) {
        irq_restore(irq);
        return PROCESS_ERR_NOT_FOUND;
    }
    ProcessSecurity old = p->security;
    p->security = security;
    irq_restore(irq);

    if (previous != NULL)
        *previous = old;

    // Audit trail: every change, and who made it (0 = the kernel itself)
    if (old != security) {
        process_log_prefix(pid);
        serial_puts("security ");
        serial_put_hex(old);
        serial_puts(" -> ");
        serial_put_hex(security);
        serial_puts(" by pid ");
        serial_put_dec((uint32_t)process_current_id());
        serial_puts("\n");

        proc_refresh(pid);
    }
    return PROCESS_OK;
}

int ProcessGrantSecurity(ProcessId pid, ProcessSecurity bits, bool elevated) {
    if ((bits & ~SYS_SEC_VALID) != 0)
        return PROCESS_ERR_INVALID_ARG;

    // Read and write under one lock: a SYS_SEC_SET landing at the same time
    // is either fully before or fully after, never half overwritten
    uint32_t irq = irq_save();
    Process* p = process_lookup(pid);
    if (p == NULL || p->state != PROCESS_STATE_RUNNING) {
        irq_restore(irq);
        return PROCESS_ERR_NOT_FOUND;
    }
    ProcessSecurity old = p->security;
    p->security = old | bits;
    if (elevated)
        p->elevated = true;
    ProcessSecurity now = p->security;
    irq_restore(irq);

    process_log_prefix(pid);
    serial_puts("security ");
    serial_put_hex(old);
    serial_puts(" -> ");
    serial_put_hex(now);
    serial_puts(elevated ? " granted by the user\n" : " granted by the kernel\n");

    if (old != now)
        proc_refresh(pid);
    return PROCESS_OK;
}

bool ProcessIsElevated(ProcessId pid) {
    uint32_t irq = irq_save();
    Process* p = process_lookup(pid);
    bool elevated = p != NULL && p->elevated;
    irq_restore(irq);
    return elevated;
}

bool ProcessGetImage(ProcessId pid, char name[PROCESS_NAME_MAX], char path[PROCESS_PATH_MAX]) {
    uint32_t irq = irq_save();
    Process* p = process_lookup(pid);
    bool found = p != NULL && p->state == PROCESS_STATE_RUNNING;
    if (found) {
        memcpy(name, p->name, PROCESS_NAME_MAX);
        memcpy(path, p->image_path, PROCESS_PATH_MAX);
    }
    irq_restore(irq);
    if (found) {
        name[PROCESS_NAME_MAX - 1] = '\0';
        path[PROCESS_PATH_MAX - 1] = '\0';
    }
    return found;
}

uint32_t process_get_count(void) {
    return process_count;
}

const char* process_error_string(int code) {
    if (code > 0) return "ok";
    switch (code) {
        case PROCESS_OK:                 return "ok";
        case PROCESS_ERR_INVALID_ARG:    return "Invalid program path";
        case PROCESS_ERR_NO_SLOTS:       return "Too many programs running";
        case PROCESS_ERR_NO_MEMORY:      return "Not enough memory";
        case PROCESS_ERR_NO_PAGING:      return "Paging is not enabled";
        case PROCESS_ERR_LOAD_FAILED:    return "The program could not be read";
        case PROCESS_ERR_NOT_FOUND:      return "The program was not found";
        case PROCESS_ERR_THREAD:         return "Thread main unable to start";
        case PROCESS_ERR_NOT_EXECUTABLE: return "The file is not marked executable";
        case PROCESS_ERR_BAD_IMAGE:      return "Not a valid ELF executable";
        case PROCESS_ERR_UNSUPPORTED:    return "Unsupported executable";
        case PROCESS_ERR_TOO_BIG:        return "Argument list too long";
        default:                         return "Unknown error";
    }
}

//
// Calling thread's process
//

ProcessId process_current_id(void) {
    ThreadBlock* self = current_thread;
    return self ? (ProcessId)self->process_id : PROCESS_INVALID_ID;
}

ProcessSecurity process_current_security(void) {
    ProcessId pid = process_current_id();
    if (pid == PROCESS_INVALID_ID)
        return SYS_SEC_NONE;

    uint32_t irq = irq_save();
    Process* p = process_lookup(pid);
    ProcessSecurity security = (p != NULL && p->state == PROCESS_STATE_RUNNING) ? p->security : SYS_SEC_NONE;
    irq_restore(irq);
    return security;
}

bool process_current_kill_pending(void) {
    ProcessId pid = process_current_id();
    if (pid == PROCESS_INVALID_ID)
        return false;

    uint32_t irq = irq_save();
    Process* p = process_lookup(pid);
    bool pending = p != NULL && p->kill_pending;
    irq_restore(irq);
    return pending;
}

bool process_current_elevated(void) {
    ProcessId pid = process_current_id();
    return pid != PROCESS_INVALID_ID && ProcessIsElevated(pid);
}

uint32_t process_current_clearance(void) {
    if (process_current_id() == PROCESS_INVALID_ID)
        return VFS_CLEARANCE_ALL;   // The kernel itself is never refused
    return (uint32_t)process_current_security();
}

// The returned pointer stays valid while the caller runs: the only way the
// space is freed is DestroyProcess(), which also kills the caller, and a
// killed thread is never scheduled again.
struct AddressSpace* process_current_space(void) {
    ProcessId pid = process_current_id();
    if (pid == PROCESS_INVALID_ID)
        return NULL;

    uint32_t irq = irq_save();
    Process* p = process_lookup(pid);
    AddressSpace* space = (p != NULL && p->state == PROCESS_STATE_RUNNING) ? p->space : NULL;
    irq_restore(irq);
    return space;
}

// The Process is read and changed without the lock: only the process's own
// thread (there is one per process for now) moves its break, and the only
// other writer, DestroyProcess(), also kills this thread, which then never
// runs again. With several threads per process this needs a per-process
// mutex around the whole call.
int process_sbrk(int32_t increment, uint32_t* previous) {
    ProcessId pid = process_current_id();
    if (pid == PROCESS_INVALID_ID || previous == NULL)
        return PROCESS_ERR_NOT_FOUND;

    uint32_t irq = irq_save();
    Process* p = process_lookup(pid);
    if (p == NULL || p->state != PROCESS_STATE_RUNNING) {
        irq_restore(irq);
        return PROCESS_ERR_NOT_FOUND;
    }
    irq_restore(irq);

    uint32_t old_break = p->heap.base + p->heap.size;

    if (increment > 0) {
        uint32_t grow = (uint32_t)increment;
        if (grow > PROCESS_HEAP_MAX - p->heap.size)
            return PROCESS_ERR_NO_MEMORY;

        uint32_t new_size  = p->heap.size + grow;
        uint32_t new_pages = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;

        // Map only the pages the break newly reaches (zeroed by the VMM).
        // On failure nothing is mapped and the break does not move.
        if (new_pages > p->heap.pages) {
            uint32_t first = p->heap.base + p->heap.pages * PAGE_SIZE;
            if (!vmm_user_alloc(p->space, first, new_pages - p->heap.pages, VM_READWRITE))
                return PROCESS_ERR_NO_MEMORY;
            p->heap.pages = new_pages;
        }
        p->heap.size = new_size;
    } else if (increment < 0) {
        uint32_t shrink = (uint32_t)0 - (uint32_t)increment;
        if (shrink > p->heap.size)
            return PROCESS_ERR_INVALID_ARG;

        uint32_t new_size   = p->heap.size - shrink;
        uint32_t keep_pages = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;

        // Give back every page now wholly above the break
        if (keep_pages < p->heap.pages) {
            uint32_t first = p->heap.base + keep_pages * PAGE_SIZE;
            vmm_user_free(p->space, first, p->heap.pages - keep_pages);
            p->heap.pages = keep_pages;
        }
        p->heap.size = new_size;
    }

    *previous = old_break;
    if (increment != 0)
        proc_refresh(pid);
    return PROCESS_OK;
}

int process_exit_current(int32_t code) {
    ProcessId pid = process_current_id();
    if (pid == PROCESS_INVALID_ID)
        return PROCESS_ERR_NOT_FOUND;

    process_log_prefix(pid);
    serial_puts("exited with code ");
    serial_put_int(code);
    serial_puts("\n");

    return DestroyProcess(pid);     // Does not return when it succeeds
}

static const char* fault_name(uint32_t vector) {
    switch (vector) {
        case 0:  return "divide error";
        case 1:  return "debug trap";
        case 3:  return "breakpoint";
        case 4:  return "overflow";
        case 5:  return "bound range exceeded";
        case 6:  return "invalid opcode";
        case 7:  return "device not available";
        case 12: return "stack-segment fault";
        case 13: return "general protection fault";
        case 14: return "page fault";
        case 16: return "x87 floating-point error";
        case 17: return "alignment check";
        case 19: return "SIMD floating-point error";
        default: return "CPU exception";
    }
}

void process_user_fault(uint32_t vector, uint32_t error_code, uint32_t eip,
                        uint32_t cs, uint32_t address) {
    // Kernel faults go to the panic screen. A double fault or machine check
    // is never the user's doing, and the #DF frame's CS is not reliable.
    if ((cs & 3) != 3 || vector == 8 || vector == 18)
        return;

    ProcessId pid = process_current_id();
    if (pid != PROCESS_INVALID_ID)
        process_log_prefix(pid);
    else
        serial_puts("[process] ring 3 thread with no process ");

    serial_puts("killed: ");
    serial_puts(fault_name(vector));
    serial_puts(" at eip ");
    serial_put_hex(eip);
    if (vector == 14) {
        serial_puts(", address ");
        serial_put_hex(address);
        // Error code: P (protection, else not present), W (write), U (user)
        serial_puts((error_code & 1) ? " (protection, " : " (not present, ");
        serial_puts((error_code & 2) ? "write)" : "read)");
    } else if (vector == 13 && error_code != 0) {
        serial_puts(", selector ");
        serial_put_hex(error_code);
    }
    serial_puts("\n");

    // The fault came from ring 3, so this thread holds no kernel lock and
    // was running with interrupts on (IOPL 0: user code cannot cli). The
    // exception gate turned them off; turn them back on, because closing
    // the process's files takes mutexes and may sleep. From here on it is
    // in the kernel for good: a kill from elsewhere must leave it to finish.
    if (current_thread != NULL)
        current_thread->in_syscall = true;
    __asm__ volatile("sti" : : : "memory");

    process_exit_current((int32_t)(128 + vector));

    // Not in the process table (should not happen): end just this thread
    kthread_exit();
}

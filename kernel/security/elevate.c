#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/security/elevate.h>
#include <kernel/scheduler/process.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/ipc/ipc_platform.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>

_Static_assert(SYS_ELEVATE_NAME_MAX == PROCESS_NAME_MAX, "SYS_ELEVATE_NAME_MAX must equal PROCESS_NAME_MAX");
_Static_assert(SYS_ELEVATE_PATH_MAX == PROCESS_PATH_MAX, "SYS_ELEVATE_PATH_MAX must equal PROCESS_PATH_MAX");
_Static_assert(sizeof(SysElevateRequest) == 16 + SYS_ELEVATE_NAME_MAX + SYS_ELEVATE_PATH_MAX +
               SYS_ELEVATE_REASON_MAX, "SysElevateRequest is part of the user ABI");

// How often a waiting requester looks at the clock and its kill flag. The
// answer itself wakes it at once (the slot's event).
#define ELEVATE_POLL_MS     250

// Refusals remembered for the cooldown: by process, and by executable path
// (so a program can't dodge it by starting copies of itself)
#define ELEVATE_REFUSED_MAX 8

typedef enum {
    SLOT_FREE = 0,
    SLOT_QUEUED,        // Waiting for the agent to pick it up
    SLOT_SHOWN,         // Handed to the agent (NEXT): on screen
    SLOT_DONE,          // Answered (or the agent went away): `result` holds the outcome
} SlotState;

typedef struct {
    SlotState       state;
    uint32_t        id;             // Unique, never 0, never reused
    ProcessId       pid;
    ProcessSecurity security;
    int32_t         result;         // SLOT_DONE: SYS_OK (allowed) or SYS_E*
    uint64_t        queued_ms;
    uint64_t        shown_ms;
    Event           done;           // Signaled when the slot becomes DONE
    char            name[PROCESS_NAME_MAX];
    char            path[PROCESS_PATH_MAX];
    char            reason[SYS_ELEVATE_REASON_MAX];
} ElevateSlot;

typedef struct {
    ProcessId pid;
    uint32_t  path_hash;
    uint64_t  until_ms;
} ElevateRefusal;

static ElevateSlot    slots[ELEVATE_QUEUE_MAX];
static ElevateRefusal refused[ELEVATE_REFUSED_MAX];
static uint32_t       refused_next;
static ProcessId      agent_pid;
static uint32_t       next_id = 1;

//
// Helpers (interrupts off unless said otherwise)
//

static uint32_t path_hash(const char* path) {
    uint32_t hash = 2166136261u;
    for (; *path; path++) {
        hash ^= (uint8_t)*path;
        hash *= 16777619u;
    }
    return hash;
}

static ElevateSlot* slot_by_id(uint32_t id) {
    if (id == 0)
        return NULL;
    for (uint32_t i = 0; i < ELEVATE_QUEUE_MAX; i++) {
        if (slots[i].state != SLOT_FREE && slots[i].id == id)
            return &slots[i];
    }
    return NULL;
}

static void slot_free(ElevateSlot* s) {
    memset(s, 0, sizeof(*s));
}

static bool recently_refused(ProcessId pid, uint32_t hash, uint64_t now) {
    for (uint32_t i = 0; i < ELEVATE_REFUSED_MAX; i++) {
        const ElevateRefusal* r = &refused[i];
        if (r->until_ms > now && (r->pid == pid || r->path_hash == hash))
            return true;
    }
    return false;
}

static void remember_refusal(const ElevateSlot* s, uint64_t now) {
    ElevateRefusal* r = &refused[refused_next];
    refused_next = (refused_next + 1) % ELEVATE_REFUSED_MAX;
    r->pid       = s->pid;
    r->path_hash = path_hash(s->path);
    r->until_ms  = now + SYS_ELEVATE_COOLDOWN_MS;
}

// The agent is set and still running
static bool agent_alive(void) {
    ProcessSecurity security;
    return agent_pid > 0 && ProcessGetSecurity(agent_pid, &security);
}

static bool caller_is_agent(void) {
    ProcessId self = process_current_id();
    return self > 0 && self == agent_pid;
}

// Printable ASCII only, so the prompt can't be made to show control codes
// or bytes the font draws as something else. Trailing blanks go.
static void clean_reason(const char* src, char out[SYS_ELEVATE_REASON_MAX]) {
    uint32_t n = 0;
    if (src != NULL) {
        for (; n + 1 < SYS_ELEVATE_REASON_MAX && src[n] != '\0'; n++) {
            char c = src[n];
            out[n] = (c >= 0x20 && c <= 0x7E) ? c : '?';
        }
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

// Audit trail on COM1. Copies, not a slot: the slot may be gone by now.
static void log_request(const char* what, ProcessId pid, const char* path) {
    serial_puts("elevate: ");
    serial_puts(what);
    serial_puts(" pid ");
    serial_put_dec((uint32_t)pid);
    serial_puts(" (");
    serial_puts(path);
    serial_puts(")\n");
}

//
// Requester
//

// Take the slot back if it is still ours. The result when it was answered
// first (it then counts), `fallback` otherwise.
static int32_t withdraw(ElevateSlot* s, uint32_t id, int32_t fallback) {
    int32_t result = fallback;
    uint32_t irq = irq_save();
    if (s->id == id) {
        if (s->state == SLOT_DONE)
            result = s->result;
        slot_free(s);
    }
    irq_restore(irq);
    return result;
}

int32_t elevate_request(const char* reason) {
    ProcessId self = process_current_id();
    if (self <= 0)
        return SYS_EPERM;
    if (process_current_security() & SYS_SEC_ADMIN)
        return SYS_OK;      // Nothing to ask for

    // Who is asking, from the process table (interrupts on: plain copies)
    char name[PROCESS_NAME_MAX];
    char path[PROCESS_PATH_MAX];
    char text[SYS_ELEVATE_REASON_MAX];
    if (!ProcessGetImage(self, name, path))
        return SYS_ESRCH;
    clean_reason(reason, text);

    uint64_t now = timer_get_ms();
    uint32_t irq = irq_save();

    if (!agent_alive()) {
        irq_restore(irq);
        return SYS_EAGAIN;
    }
    if (recently_refused(self, path_hash(path), now)) {
        irq_restore(irq);
        return SYS_EBUSY;
    }

    ElevateSlot* s = NULL;
    for (uint32_t i = 0; i < ELEVATE_QUEUE_MAX; i++) {
        if (slots[i].state != SLOT_FREE && slots[i].pid == self) {
            irq_restore(irq);
            return SYS_EBUSY;       // One at a time (no user threads yet, but cheap)
        }
        if (s == NULL && slots[i].state == SLOT_FREE)
            s = &slots[i];
    }
    if (s == NULL) {
        irq_restore(irq);
        return SYS_EBUSY;
    }

    memset(s, 0, sizeof(*s));
    s->id = next_id++;
    if (next_id == 0)
        next_id = 1;
    s->pid       = self;
    s->security  = process_current_security();
    s->queued_ms = now;
    event_init(&s->done);
    memcpy(s->name, name, sizeof(s->name));
    memcpy(s->path, path, sizeof(s->path));
    memcpy(s->reason, text, sizeof(s->reason));
    s->state = SLOT_QUEUED;
    uint32_t id = s->id;
    irq_restore(irq);

    log_request("asked by", self, path);
    ipc_plat_wake_all();        // The agent waits on its input handle

    int32_t result;
    while (1) {
        bool over = false;
        irq = irq_save();
        if (s->id != id) {
            result = SYS_EAGAIN;        // Dropped (never happens while we wait, but be sure)
            over = true;
        } else if (s->state == SLOT_DONE) {
            result = s->result;
            slot_free(s);
            over = true;
        }
        irq_restore(irq);
        if (over)
            break;

        // Killed (Ctrl+C, kill): give up; syscall_dispatch() ends us
        if (process_current_kill_pending()) {
            result = withdraw(s, id, SYS_EPIPE);
            if (result == SYS_OK)
                result = SYS_EPIPE;     // Never grant to a dying process
            break;
        }
        if (timer_get_ms() - now >= SYS_ELEVATE_TIMEOUT_MS) {
            result = withdraw(s, id, SYS_ETIMEDOUT);
            break;
        }

        event_wait(&s->done, ELEVATE_POLL_MS);
    }

    // The grant, by the kernel on this thread: nothing the program does can
    // get here without the agent's yes for this very request
    if (result == SYS_OK) {
        if (ProcessGrantSecurity(self, SYS_SEC_ADMIN, true) != PROCESS_OK)
            result = SYS_ESRCH;
    }
    return result;
}

//
// Agent
//

int32_t elevate_agent_register(void) {
    ProcessId self = process_current_id();
    if (self <= 0 || process_current_elevated())
        return SYS_EPERM;

    uint32_t irq = irq_save();
    if (agent_pid != self && agent_alive()) {
        irq_restore(irq);
        return SYS_EBUSY;
    }
    agent_pid = self;
    irq_restore(irq);

    serial_puts("elevate: agent is pid ");
    serial_put_dec((uint32_t)self);
    serial_puts("\n");
    return SYS_OK;
}

int32_t elevate_agent_next(SysElevateRequest* out) {
    if (!caller_is_agent())
        return SYS_EPERM;

    uint32_t irq = irq_save();

    // One on screen at a time: one already handed out is handed out again
    // (an agent that lost track gets it back), else the oldest waiting
    ElevateSlot* pick = NULL;
    uint32_t waiting = 0;
    for (uint32_t i = 0; i < ELEVATE_QUEUE_MAX; i++) {
        if (slots[i].state == SLOT_SHOWN)
            pick = &slots[i];
    }
    for (uint32_t i = 0; i < ELEVATE_QUEUE_MAX; i++) {
        ElevateSlot* s = &slots[i];
        if (s->state != SLOT_QUEUED)
            continue;
        waiting++;
        if (pick == NULL || (pick->state == SLOT_QUEUED && s->id < pick->id))
            pick = s;
    }
    if (pick == NULL) {
        irq_restore(irq);
        return 0;
    }
    if (pick->state == SLOT_QUEUED) {
        pick->state    = SLOT_SHOWN;
        pick->shown_ms = timer_get_ms();
        waiting--;
    }

    memset(out, 0, sizeof(*out));
    out->id       = pick->id;
    out->pid      = pick->pid;
    out->security = pick->security;
    out->waiting  = waiting;
    memcpy(out->name, pick->name, sizeof(out->name));
    memcpy(out->path, pick->path, sizeof(out->path));
    memcpy(out->reason, pick->reason, sizeof(out->reason));
    irq_restore(irq);
    return 1;
}

int32_t elevate_agent_status(uint32_t id) {
    if (!caller_is_agent())
        return SYS_EPERM;

    uint32_t irq = irq_save();
    ElevateSlot* s = slot_by_id(id);
    int32_t waiting = (s != NULL && (s->state == SLOT_QUEUED || s->state == SLOT_SHOWN)) ? 1 : 0;
    irq_restore(irq);
    return waiting;
}

int32_t elevate_agent_answer(uint32_t id, bool allow) {
    if (!caller_is_agent())
        return SYS_EPERM;

    uint64_t now = timer_get_ms();
    uint32_t irq = irq_save();
    ElevateSlot* s = slot_by_id(id);
    if (s == NULL || s->state == SLOT_DONE) {
        irq_restore(irq);
        return SYS_ESRCH;
    }
    // Only what the agent was given to show, and a yes only once it has
    // been up long enough to be read: a click meant for whatever was under
    // the pointer a moment ago must not land on Allow
    if (s->state != SLOT_SHOWN) {
        irq_restore(irq);
        return SYS_EINVAL;
    }
    if (allow && now - s->shown_ms < SYS_ELEVATE_ARM_MS) {
        irq_restore(irq);
        return SYS_EAGAIN;
    }

    s->result = allow ? SYS_OK : SYS_EACCES;
    s->state  = SLOT_DONE;
    if (!allow)
        remember_refusal(s, now);
    ProcessId pid = s->pid;
    char path[PROCESS_PATH_MAX];
    memcpy(path, s->path, sizeof(path));
    event_signal(&s->done);
    irq_restore(irq);

    log_request(allow ? "allowed for" : "refused for", pid, path);
    return SYS_OK;
}

bool elevate_is_agent(int32_t pid) {
    return pid > 0 && pid == agent_pid;
}

bool elevate_agent_has_work(void) {
    bool queued = false, shown = false;
    uint32_t irq = irq_save();
    if (agent_pid > 0) {
        for (uint32_t i = 0; i < ELEVATE_QUEUE_MAX; i++) {
            queued |= slots[i].state == SLOT_QUEUED;
            shown  |= slots[i].state == SLOT_SHOWN;
        }
    }
    irq_restore(irq);
    return queued && !shown;
}

void elevate_process_exit(int32_t pid) {
    if (pid <= 0)
        return;

    uint32_t irq = irq_save();
    bool was_agent = (pid == agent_pid);
    if (was_agent)
        agent_pid = 0;

    for (uint32_t i = 0; i < ELEVATE_QUEUE_MAX; i++) {
        ElevateSlot* s = &slots[i];
        if (s->state == SLOT_FREE)
            continue;
        if (s->pid == pid) {
            // Its thread is gone (a requester ending normally has already
            // taken its slot back)
            slot_free(s);
        } else if (was_agent && s->state != SLOT_DONE) {
            s->result = SYS_EAGAIN;     // Nobody left to ask
            s->state  = SLOT_DONE;
            event_signal(&s->done);
        }
    }
    irq_restore(irq);

    if (was_agent)
        serial_puts("elevate: agent gone, waiting requests refused\n");
}

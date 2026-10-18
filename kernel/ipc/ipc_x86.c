// IPC platform layer for the x86 kernel

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/ipc/ipc.h>
#include <kernel/ipc/ipc_platform.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/arch/x86/drivers/keyboard.h>
#include <kernel/boot/x86/interrupt.h>
#include <kernel/console/mouse.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/mutex.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>

//
// Lock
//

static mutex_t ipc_mutex;
static bool    ipc_mutex_ready = false;

void ipc_plat_lock(void) {
    // The first IPC call can only come from a process thread, long after
    // boot, so a lazy init with interrupts off is enough
    if (!ipc_mutex_ready) {
        uint32_t flags = irq_save();
        if (!ipc_mutex_ready) {
            mutex_init(&ipc_mutex);
            ipc_mutex_ready = true;
        }
        irq_restore(flags);
    }
    mutex_lock(&ipc_mutex);
}

void ipc_plat_unlock(void) {
    mutex_unlock(&ipc_mutex);
}

//
// User memory
//

bool ipc_plat_user_read(void* space, void* dst, uint32_t user_addr, uint32_t length) {
    AddressSpace* s = (AddressSpace*)space;
    if (s == NULL || !vmm_is_user_range(user_addr, length))
        return false;
    return vmm_user_check(s, user_addr, length, false) &&
           vmm_user_copy_from(s, dst, user_addr, length);
}

bool ipc_plat_user_write(void* space, uint32_t user_addr, const void* src, uint32_t length) {
    AddressSpace* s = (AddressSpace*)space;
    if (s == NULL || !vmm_is_user_range(user_addr, length))
        return false;
    return vmm_user_check(s, user_addr, length, true) &&
           vmm_user_copy_to(s, user_addr, src, length);
}

//
// Shared pages
//

void* ipc_plat_alloc_pages(uint32_t pages) {
    // vmm_alloc_pages() panics rather than fail; SYS_SHM_TOTAL_MAX keeps
    // shared memory to a size the machine is expected to have
    void* kva = vmm_alloc_pages(pages);
    if (kva != NULL)
        memset(kva, 0, (size_t)pages * PAGE_SIZE);
    return kva;
}

void ipc_plat_free_pages(void* kva, uint32_t pages) {
    vmm_free_pages(kva, pages);
}

bool ipc_plat_map(void* space, void* kva, uint32_t pages, uint32_t* addr) {
    AddressSpace* s = (AddressSpace*)space;
    uint32_t base = *addr;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = vmm_get_phys_addr((uint8_t*)kva + i * PAGE_SIZE);
        if (phys == 0 || !vmm_user_map_page(s, phys, base + i * PAGE_SIZE, VM_READWRITE)) {
            // Frames mapped with vmm_user_map_page() are not owned: this
            // only removes the mappings
            if (i > 0)
                vmm_user_free(s, base, i);
            return false;
        }
    }
    return true;
}

void ipc_plat_unmap(void* space, uint32_t addr, uint32_t pages) {
    vmm_user_free((AddressSpace*)space, addr, pages);
}

//
// Time
//

uint32_t ipc_plat_time_ms(void) {
    return (uint32_t)timer_get_ms();
}

void ipc_plat_sleep_ms(uint32_t ms) {
    thread_sleep(ms);
}

//
// Sleepers: one Event each, on a list the wake walks with interrupts off
//

typedef struct IpcKSleeper {
    Event ev;
    struct IpcKSleeper* next;
    struct IpcKSleeper* prev;
} IpcKSleeper;

_Static_assert(sizeof(IpcKSleeper) <= sizeof(IpcSleeper), "IpcSleeper storage too small");

static IpcKSleeper* ipc_sleepers = NULL;

void ipc_plat_sleep_begin(IpcSleeper* storage) {
    IpcKSleeper* s = (IpcKSleeper*)storage;
    event_init(&s->ev);

    uint32_t flags = irq_save();
    s->prev = NULL;
    s->next = ipc_sleepers;
    if (ipc_sleepers != NULL)
        ipc_sleepers->prev = s;
    ipc_sleepers = s;
    irq_restore(flags);
}

void ipc_plat_sleep(IpcSleeper* storage, uint32_t timeout_ms) {
    IpcKSleeper* s = (IpcKSleeper*)storage;
    // event_wait: 0 means no timeout
    if (timeout_ms == SYS_IPC_WAIT_FOREVER)
        event_wait(&s->ev, 0);
    else
        event_wait(&s->ev, (timeout_ms == 0) ? 1 : timeout_ms);
}

void ipc_plat_sleep_end(IpcSleeper* storage) {
    IpcKSleeper* s = (IpcKSleeper*)storage;
    uint32_t flags = irq_save();
    if (s->prev != NULL) s->prev->next = s->next;
    else                 ipc_sleepers  = s->next;
    if (s->next != NULL) s->next->prev = s->prev;
    irq_restore(flags);
}

void ipc_plat_wake_all(void) {
    uint32_t flags = irq_save();
    for (IpcKSleeper* s = ipc_sleepers; s != NULL; s = s->next)
        event_signal(&s->ev);
    irq_restore(flags);
}

//
// Input
//

static uint8_t ipc_input_mods(void) {
    uint8_t mods = 0;
    if (kb_shift_down()) mods |= SYS_INPUT_MOD_SHIFT;
    if (kb_ctrl_down())  mods |= SYS_INPUT_MOD_CTRL;
    if (kb_alt_down())   mods |= SYS_INPUT_MOD_ALT;
    return mods;
}

static int16_t ipc_clamp16(int32_t v) {
    if (v < -32768) return -32768;
    if (v > 32767)  return 32767;
    return (int16_t)v;
}

void ipc_plat_input_state(SysInputEvent* ev) {
    Point p = mouse_get_position();
    ev->x       = ipc_clamp16(p.x);
    ev->y       = ipc_clamp16(p.y);
    ev->buttons = (mouse_get_button(MOUSE_BUTTON_LEFT)   ? SYS_INPUT_BTN_LEFT   : 0) |
                  (mouse_get_button(MOUSE_BUTTON_RIGHT)  ? SYS_INPUT_BTN_RIGHT  : 0) |
                  (mouse_get_button(MOUSE_BUTTON_MIDDLE) ? SYS_INPUT_BTN_MIDDLE : 0);
    ev->mods     = ipc_input_mods();
    ev->reserved = 0;
}

bool ipc_plat_input_pop(SysInputEvent* ev) {
    MouseEvent m;
    if (mouse_dequeue_event(&m)) {
        ev->type     = SYS_INPUT_MOUSE;
        ev->key      = 0;
        ev->x        = ipc_clamp16(m.x);
        ev->y        = ipc_clamp16(m.y);
        ev->buttons  = (m.left_button   ? SYS_INPUT_BTN_LEFT   : 0) |
                       (m.right_button  ? SYS_INPUT_BTN_RIGHT  : 0) |
                       (m.middle_button ? SYS_INPUT_BTN_MIDDLE : 0);
        ev->mods     = ipc_input_mods();
        ev->reserved = 0;
        ev->time_ms  = (uint32_t)m.timestamp;
        return true;
    }

    uint16_t key;
    if (input_key_pop(&key)) {
        ipc_plat_input_state(ev);
        ev->type    = SYS_INPUT_KEY;
        ev->key     = key;
        ev->time_ms = (uint32_t)timer_get_ms();
        return true;
    }
    return false;
}

bool ipc_plat_input_pending(void) {
    return mouse_event_pending() || input_key_pending();
}

void ipc_plat_input_flush(void) {
    MouseEvent m;
    uint16_t key;
    while (mouse_dequeue_event(&m)) {}
    while (input_key_pop(&key)) {}
}

void ipc_plat_input_grab_changed(bool grabbed) {
    // Nothing to do: the kernel console checks ipc_input_grabbed() before
    // it reads input, and takes it back as soon as the grab is let go
    (void)grabbed;
}

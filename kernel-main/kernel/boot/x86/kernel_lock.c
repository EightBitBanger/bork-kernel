#include <kernel/kernel_lock.h>
#include <kernel/mutex.h>
#include <kernel/scheduler/scheduler.h>

extern ThreadBlock* current_thread;

static mutex_t kernel_lock;

void kernel_lock_init(void) {
    mutex_init(&kernel_lock);
}

void kernel_lock_acquire(void) {
    mutex_lock(&kernel_lock);
}

void kernel_lock_release(void) {
    mutex_unlock(&kernel_lock);
}

bool kernel_lock_held(void) {
    return kernel_lock.locked && kernel_lock.owner == current_thread;
}

mutex_t* kernel_lock_mutex(void) {
    return &kernel_lock;
}

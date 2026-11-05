#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>
#include <kernel/driver/driver_internal.h>
#include <kernel/driver/kdrv_loader.h>

#include <kernel/mutex.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>

extern ThreadBlock* current_thread;

// Filled in by the linker: every DRIVER_BUILTIN_DECLARE (boot/x86/linker.ld)
extern const struct Driver* const __kdrivers_start[];
extern const struct Driver* const __kdrivers_end[];

// Registration order is probe order
static const struct Driver* drivers[DRIVER_MAX];
static uint32_t drivers_count = 0;

// When each driver's poll is next due, by its index in `drivers`
static uint64_t poll_due[DRIVER_MAX];

// The driver whose poll is running on the "drivers" thread, NULL if none.
// driver_unregister waits for it to come back before forgetting a driver.
static const struct Driver* volatile polling = NULL;

//
// Driver lock (recursive, the vfs_lock pattern: see vfs_lock.c)
//

static mutex_t driver_mutex;
static ThreadBlock* volatile driver_owner = NULL;
static volatile uint32_t     driver_depth = 0;

void driver_lock(void) {
    if (driver_depth != 0 && driver_owner == current_thread) {
        driver_depth++;
        return;
    }
    mutex_lock(&driver_mutex);
    driver_owner = current_thread;
    driver_depth = 1;
}

void driver_unlock(void) {
    if (driver_depth == 0 || driver_owner != current_thread)
        return;
    if (--driver_depth != 0)
        return;
    driver_owner = NULL;
    mutex_unlock(&driver_mutex);
}

//
// Matching
//

static bool match_number(uint16_t want, uint16_t have) {
    return want == MATCH_ANY || want == have;
}

static bool match_row(const struct DeviceMatch* row, const struct Device* dev) {
    if (row->bus != dev->bus)
        return false;
    if (row->name != NULL && strcmp(row->name, dev->name) != 0)
        return false;
    if (dev->bus == BUS_PLATFORM)
        return row->name != NULL;   // Platform devices are matched by name only
    return match_number(row->vendor,     dev->vendor)     &&
           match_number(row->product,    dev->product)    &&
           match_number(row->class_code, dev->class_code) &&
           match_number(row->subclass,   dev->subclass)   &&
           match_number(row->protocol,   dev->protocol);
}

bool driver_matches(const struct Driver* drv, const struct Device* dev) {
    if (drv == NULL || drv->match == NULL || dev == NULL)
        return false;
    for (const struct DeviceMatch* row = drv->match; row->bus != 0; row++) {
        if (match_row(row, dev))
            return true;
    }
    return false;
}

//
// Registry
//

static uint32_t strnlen_bounded(const char* text, uint32_t max) {
    uint32_t length = 0;
    while (length < max && text[length] != '\0')
        length++;
    return length;
}

static int32_t driver_index(const struct Driver* drv) {
    for (uint32_t i = 0; i < drivers_count; i++) {
        if (drivers[i] == drv)
            return (int32_t)i;
    }
    return -1;
}

static const struct Driver* driver_find_locked(const char* name) {
    for (uint32_t i = 0; i < drivers_count; i++) {
        if (strncmp(drivers[i]->name, name, DRIVER_NAME_MAX) == 0)
            return drivers[i];
    }
    return NULL;
}

bool driver_register(const struct Driver* drv) {
    if (drv == NULL || drv->abi != DRIVER_ABI || !driver_shape_ok(drv))
        return false;
    if (drv->name[0] == '\0' || strnlen_bounded(drv->name, DRIVER_NAME_MAX) >= DRIVER_NAME_MAX)
        return false;   // Empty, or no terminator inside the name field

    driver_lock();
    if (drivers_count >= DRIVER_MAX || driver_index(drv) >= 0 || driver_find_locked(drv->name) != NULL) {
        driver_unlock();
        return false;
    }

    poll_due[drivers_count] = 0;    // First poll right away
    drivers[drivers_count++] = drv;

    if (drv->type == DRIVER_TYPE_DEVICE)
        device_offer_all(drv);
    driver_tree_update();
    driver_unlock();
    return true;
}

bool driver_unregister(const struct Driver* drv) {
    driver_lock();
    int32_t index = driver_index(drv);
    if (index < 0 || (drv->flags & DRIVER_BUILTIN)) {
        driver_unlock();
        return false;
    }

    device_release_all(drv);

    // Out of the table: its poll is never started again
    for (uint32_t i = (uint32_t)index; i + 1 < drivers_count; i++) {
        drivers[i]  = drivers[i + 1];
        poll_due[i] = poll_due[i + 1];
    }
    drivers[--drivers_count] = NULL;

    // The devices it let go of may suit someone else
    for (uint32_t i = 0; i < device_count(); i++) {
        struct Device* dev = device_get(i);
        if (dev->driver == NULL)
            device_offer(dev);
    }

    driver_tree_update();
    driver_unlock();

    // A poll already running finishes first (unless that poll is the caller)
    while (polling == drv && driver_owner != current_thread)
        thread_sleep(1);
    return true;
}

uint32_t driver_count(void) {
    return drivers_count;
}

const struct Driver* driver_get(uint32_t index) {
    return (index < drivers_count) ? drivers[index] : NULL;
}

const struct Driver* driver_find(const char* name) {
    if (name == NULL)
        return NULL;
    driver_lock();
    const struct Driver* drv = driver_find_locked(name);
    driver_unlock();
    return drv;
}

uint32_t driver_device_count(const struct Driver* drv) {
    uint32_t count = 0;
    driver_lock();
    for (uint32_t i = 0; i < device_count(); i++) {
        if (device_get(i)->driver == drv)
            count++;
    }
    driver_unlock();
    return count;
}

//
// "drivers" thread: runs every registered driver's poll on its schedule
//

#define DRIVER_THREAD_MAX_SLEEP_MS  100

static void driver_thread(void* arg) {
    (void)arg;

    while (1) {
        uint64_t now = timer_get_ms();
        uint64_t next = now + DRIVER_THREAD_MAX_SLEEP_MS;

        for (uint32_t i = 0; ; i++) {
            // Pick the next due driver with the lock held, call it without
            void (*poll)(void) = NULL;

            driver_lock();
            if (i >= drivers_count) {
                driver_unlock();
                break;
            }
            const struct Driver* drv = drivers[i];
            if (drv->poll != NULL) {
                uint32_t interval = drv->poll_ms ? drv->poll_ms : DRIVER_POLL_DEFAULT_MS;
                if (poll_due[i] <= now) {
                    poll_due[i] = now + interval;
                    poll = drv->poll;
                    polling = drv;
                }
                if (poll_due[i] < next)
                    next = poll_due[i];
            }
            driver_unlock();

            if (poll != NULL) {
                poll();
                polling = NULL;
            }
        }

        now = timer_get_ms();
        thread_sleep((next > now) ? (uint32_t)(next - now) : 1);
    }
}

//
// Boot
//

void driver_init(void) {
    static bool started = false;
    if (started)
        return;
    started = true;

    mutex_init(&driver_mutex);
    kdrv_loader_init();

    // Built-in drivers first, so the platform devices find them
    for (const struct Driver* const* entry = __kdrivers_start; entry < __kdrivers_end; entry++)
        driver_register(*entry);

    // Virtual devices: /dev/null, /dev/zero (builtin/memdev.c)
    device_add_platform("mem");

    driver_lock();
    driver_tree_update();
    driver_unlock();

    kthread_create(driver_thread, NULL, PRIORITY_NORMAL, "drivers");
}

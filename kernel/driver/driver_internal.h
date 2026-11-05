#ifndef _KERNEL_DRIVER_INTERNAL_H_
#define _KERNEL_DRIVER_INTERNAL_H_

// Shared by the files in driver/ only

#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>

// The driver lock (see driver.h). Recursive: probe may add child devices.
void driver_lock(void);
void driver_unlock(void);

// Driver lock held for everything below

// Does any row of `drv`'s match table fit `dev`
bool driver_matches(const struct Driver* drv, const struct Device* dev);

// Offer `dev` to the registered drivers in registration order. Binds it to
// the first that answers PROBE_BOUND; stops at PROBE_FAILED.
void device_offer(struct Device* dev);

// Offer every unbound device to `drv` only
void device_offer_all(const struct Driver* drv);

// remove() and unbind every device `drv` holds
void device_release_all(const struct Driver* drv);

// Rebuild /drv (driver_tree.c). Takes the VFS lock.
void driver_tree_update(void);

#endif

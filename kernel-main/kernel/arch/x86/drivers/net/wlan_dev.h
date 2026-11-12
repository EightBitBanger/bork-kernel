#ifndef _KERNEL_DRIVERS_WLAN_DEV_H_
#define _KERNEL_DRIVERS_WLAN_DEV_H_

#include <stdint.h>

#include <kernel/arch/x86/drivers/net/wlan.h>

//
// The wlanN names, shared by every Wi-Fi driver (Realtek USB, Atheros
// PCIe, ...), so two adapters of different kinds never both become wlan0.
//
// A driver registers each adapter it binds. That takes the lowest free
// wlanN, creates /dev/wlanN (SYS_SEC_NET, permission checked on every
// call) and routes its ioctls, and the kernel-side wlan_info /
// wlan_scan_start / wlan_scan_results of wlan.h, to the driver's ops.
//
// Ops return SYS_OK, or SYS_ENOENT once the adapter is going away; they
// may take the driver's own lock. They are called with the VFS lock held
// (ioctl) or the wlan lock held (kernel side), never both. Lock order:
//
//   driver lock -> wlan lock -> the Wi-Fi driver's lock
//
// so wlan_register / wlan_unregister may be called from probe / remove,
// but never with the Wi-Fi driver's own lock held.
//

#define WLAN_MAX_ADAPTERS   8

struct WlanOps {
    // Fill `out` (zeroed by the caller), version included
    int32_t (*info)(void* ctx, struct WlanInfo* out);
    // Start a scan: SYS_OK, SYS_EBUSY, SYS_EAGAIN, SYS_ENOTTY, SYS_ENOENT
    int32_t (*scan_start)(void* ctx);
    // Fill `out` (zeroed by the caller), version included
    int32_t (*scan_results)(void* ctx, struct WlanScanResults* out);

    // Optional (NULL: SYS_ENOTTY). Join `bss` (from the driver's own last
    // scan, found by wlan_dev), `pmk` NULL for an open network:
    // SYS_OK, SYS_EINVAL, SYS_EBUSY (a scan runs), SYS_ENOENT.
    int32_t (*connect)(void* ctx, const struct WlanBss* bss, const uint8_t* pmk);
    int32_t (*disconnect)(void* ctx);
    // Fill `out` (zeroed by the caller), version included
    int32_t (*link)(void* ctx, struct WlanLink* out);
};

struct WlanDev;

// Lowest free wlanN and its /dev entry. NULL when every name is taken or
// /dev refused it. `ops` and `ctx` must stay valid until wlan_unregister
// returns.
struct WlanDev* wlan_register(const struct WlanOps* ops, void* ctx);

// "wlan0" (valid until wlan_unregister)
const char* wlan_dev_name(const struct WlanDev* wd);

// Removes /dev/wlanN and waits out every call into the ops in progress.
// No op is called once it returns. NULL is ignored.
void wlan_unregister(struct WlanDev* wd);

#endif

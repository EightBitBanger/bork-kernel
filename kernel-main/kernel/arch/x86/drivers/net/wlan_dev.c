#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/arch/x86/drivers/net/wlan.h>
#include <kernel/arch/x86/drivers/net/wlan_dev.h>
#include <kernel/crypt/crypt.h>
#include <kernel/crypt/wpa.h>
#include <kernel/driver/chardev.h>

#include <kernel/malloc.h>
#include <kernel/mutex.h>
#include <kernel/scheduler/process.h>
#include <kernel/syscall/syscall.h>
#include <kernel/util/string.h>
#include <kernel/util/snprint.h>

//
// /dev/wlanN for every Wi-Fi driver (see wlan_dev.h)
//

struct WlanDev {
    const struct WlanOps* ops;
    void*    ctx;
    struct CharDevice* cd;
    uint8_t  index;
    char     name[8];               // "wlan7"
};

// slots[] only. Held around the kernel-side calls into the ops, so
// wlan_unregister waits them out; never taken from an ioctl (VFS lock held).
static mutex_t wlan_lock;
static bool wlan_ready = false;
// A slot is NULL (free), WLAN_SLOT_RESERVED (wlan_register is creating its
// /dev entry) or the adapter
static struct WlanDev* slots[WLAN_MAX_ADAPTERS];
#define WLAN_SLOT_RESERVED  ((struct WlanDev*)1)

static void wlan_init_once(void) {
    // Drivers probe one at a time (driver lock), so this can't race
    if (!wlan_ready) {
        mutex_init(&wlan_lock);
        wlan_ready = true;
    }
}

//
// /dev/wlanN
//

// The network bit: sys_open checks the CharDevice's security bits, but
// only after open() ran, and not again on later calls, so every call
// checks for itself. Kernel threads (no process) are trusted.
static bool wlan_caller_allowed(void) {
    if (process_current_id() == PROCESS_INVALID_ID)
        return true;
    return (process_current_security() & SYS_SEC_NET) == SYS_SEC_NET;
}

static int32_t wlan_open(struct CharDevice* cd, uint16_t flags) {
    (void)cd; (void)flags;
    return wlan_caller_allowed() ? 0 : SYS_EPERM;
}

// Data goes through the network interface, not here
static int32_t wlan_read(struct CharDevice* cd, void* buffer, uint32_t size) {
    (void)cd; (void)buffer; (void)size;
    return wlan_caller_allowed() ? 0 : SYS_EPERM;
}

// WLAN_IOCTL_CONNECT: the network from the driver's last scan, the key
// from the password (here, so the password goes no further)
static int32_t wlan_connect(struct WlanDev* wd, uint32_t arg) {
    if (wd->ops->connect == NULL)
        return SYS_ENOTTY;
    struct WlanConnect* c = (struct WlanConnect*)malloc(sizeof(struct WlanConnect));
    struct WlanScanResults* r = (struct WlanScanResults*)malloc(sizeof(struct WlanScanResults));
    int32_t result = SYS_OK;
    if (c == NULL || r == NULL) {
        result = SYS_ENOMEM;
        goto out;
    }
    if (!chardev_copy_from_user(c, arg, sizeof(*c))) {
        result = SYS_EFAULT;
        goto out;
    }
    if (c->version != WLAN_CONNECT_VERSION || c->ssid_length > WLAN_SSID_MAX ||
        c->passphrase_length > WLAN_PASSPHRASE_MAX) {
        result = SYS_EINVAL;
        goto out;
    }
    memset(r, 0, sizeof(*r));
    result = wd->ops->scan_results(wd->ctx, r);
    if (result != SYS_OK)
        goto out;
    struct WlanBss bss;
    bool found = false;
    for (uint32_t i = 0; i < r->count && i < WLAN_SCAN_MAX && !found; i++) {
        if (memcmp(r->bss[i].bssid, c->bssid, 6) == 0) {
            bss = r->bss[i];
            found = true;
        }
    }
    if (!found) {
        result = SYS_ENOENT;
        goto out;
    }
    if (bss.flags & WLAN_BSS_HIDDEN) {
        // Its name comes from the user; it's checked by the access point
        if (c->ssid_length == 0) {
            result = SYS_EINVAL;
            goto out;
        }
        bss.ssid_length = c->ssid_length;
        memcpy(bss.ssid, c->ssid, c->ssid_length);
    } else if (c->ssid_length != bss.ssid_length || memcmp(c->ssid, bss.ssid, bss.ssid_length) != 0) {
        result = SYS_EINVAL;
        goto out;
    }

    uint8_t pmk[WPA_PMK_LEN];
    bool secure = bss.security != 0;
    if (secure && !wpa_pmk(c->passphrase, c->passphrase_length, bss.ssid, bss.ssid_length, pmk)) {
        result = SYS_EINVAL;
        goto out;
    }
    result = wd->ops->connect(wd->ctx, &bss, secure ? pmk : NULL);
    crypt_wipe(pmk, sizeof(pmk));
out:
    if (c != NULL) {
        crypt_wipe(c, sizeof(*c));
        free(c);
    }
    free(r);
    return result;
}

// chardev_unregister waits this out, so `wd` and its ops stay valid here
static int32_t wlan_ioctl(struct CharDevice* cd, uint32_t request, uint32_t arg) {
    if (request != WLAN_IOCTL_INFO && request != WLAN_IOCTL_SCAN && request != WLAN_IOCTL_SCAN_RESULTS &&
        request != WLAN_IOCTL_CONNECT && request != WLAN_IOCTL_DISCONNECT && request != WLAN_IOCTL_LINK)
        return SYS_ENOTTY;
    if (!wlan_caller_allowed())
        return SYS_EPERM;
    // Joining or leaving a network changes it for every program: the
    // network tray's own bit, not just the network bit
    if ((request == WLAN_IOCTL_CONNECT || request == WLAN_IOCTL_DISCONNECT) &&
        process_current_id() != PROCESS_INVALID_ID &&
        (process_current_security() & SYS_SEC_NET_CONFIG) != SYS_SEC_NET_CONFIG)
        return SYS_EPERM;
    struct WlanDev* wd = (struct WlanDev*)cd->driver_data;

    if (request == WLAN_IOCTL_CONNECT)
        return wlan_connect(wd, arg);
    if (request == WLAN_IOCTL_DISCONNECT)
        return wd->ops->disconnect != NULL ? wd->ops->disconnect(wd->ctx) : SYS_ENOTTY;
    if (request == WLAN_IOCTL_LINK) {
        if (wd->ops->link == NULL)
            return SYS_ENOTTY;
        struct WlanLink link;
        memset(&link, 0, sizeof(link));
        int32_t result = wd->ops->link(wd->ctx, &link);
        if (result == SYS_OK && !chardev_copy_to_user(arg, &link, sizeof(link)))
            result = SYS_EFAULT;
        return result;
    }

    if (request == WLAN_IOCTL_SCAN)
        return wd->ops->scan_start(wd->ctx);

    if (request == WLAN_IOCTL_SCAN_RESULTS) {
        // 1.5 KB: off the kernel stack. Zeroed first: padding and unused
        // entries never carry kernel memory out.
        struct WlanScanResults* out = (struct WlanScanResults*)malloc(sizeof(struct WlanScanResults));
        if (out == NULL)
            return SYS_ENOMEM;
        memset(out, 0, sizeof(*out));
        int32_t result = wd->ops->scan_results(wd->ctx, out);
        if (result == SYS_OK && !chardev_copy_to_user(arg, out, sizeof(*out)))
            result = SYS_EFAULT;
        free(out);
        return result;
    }

    struct WlanInfo info;
    memset(&info, 0, sizeof(info));
    int32_t result = wd->ops->info(wd->ctx, &info);
    if (result != SYS_OK)
        return result;
    return chardev_copy_to_user(arg, &info, sizeof(info)) ? 0 : SYS_EFAULT;
}

static const struct CharDeviceOps wlan_ops = {
    .open  = wlan_open,
    .read  = wlan_read,
    .ioctl = wlan_ioctl,
};

//
// Registering
//

struct WlanDev* wlan_register(const struct WlanOps* ops, void* ctx) {
    if (ops == NULL || ops->info == NULL || ops->scan_start == NULL || ops->scan_results == NULL)
        return NULL;
    wlan_init_once();

    struct WlanDev* wd = (struct WlanDev*)malloc(sizeof(struct WlanDev));
    if (wd == NULL)
        return NULL;
    memset(wd, 0, sizeof(*wd));
    wd->ops = ops;
    wd->ctx = ctx;

    // Reserve the name, then create /dev/wlanN without the lock (the VFS
    // lock comes after it in no order we keep)
    mutex_lock(&wlan_lock);
    int index = -1;
    for (int i = 0; i < WLAN_MAX_ADAPTERS && index < 0; i++) {
        if (slots[i] == NULL)
            index = i;
    }
    if (index >= 0)
        slots[index] = WLAN_SLOT_RESERVED;
    mutex_unlock(&wlan_lock);
    if (index < 0) {
        free(wd);
        return NULL;
    }

    wd->index = (uint8_t)index;
    snprintf(wd->name, sizeof(wd->name), "wlan%d", index);
    wd->cd = chardev_register(wd->name, &wlan_ops, wd, SYS_SEC_NET);

    mutex_lock(&wlan_lock);
    slots[index] = wd->cd != NULL ? wd : NULL;
    mutex_unlock(&wlan_lock);
    if (wd->cd == NULL) {
        free(wd);
        return NULL;
    }
    return wd;
}

const char* wlan_dev_name(const struct WlanDev* wd) {
    return wd != NULL ? wd->name : "";
}

void wlan_unregister(struct WlanDev* wd) {
    if (wd == NULL)
        return;
    // Waits out a kernel-side call in progress
    mutex_lock(&wlan_lock);
    slots[wd->index] = NULL;
    mutex_unlock(&wlan_lock);
    // Revokes open handles, and waits out an ioctl in progress (VFS lock)
    chardev_unregister(wd->cd);
    free(wd);
}

//
// Kernel side (wlan.h), for the kernel console
//

// wlan_lock held
static struct WlanDev* wlan_find_locked(uint32_t index) {
    if (index >= WLAN_MAX_ADAPTERS)
        return NULL;
    struct WlanDev* wd = slots[index];
    return wd != WLAN_SLOT_RESERVED ? wd : NULL;
}

int32_t wlan_scan_start(uint32_t index) {
    if (!wlan_ready)
        return SYS_ENOENT;
    mutex_lock(&wlan_lock);
    struct WlanDev* wd = wlan_find_locked(index);
    int32_t result = wd != NULL ? wd->ops->scan_start(wd->ctx) : SYS_ENOENT;
    mutex_unlock(&wlan_lock);
    return result;
}

int32_t wlan_scan_results(uint32_t index, struct WlanScanResults* out) {
    if (!wlan_ready || out == NULL)
        return SYS_ENOENT;
    memset(out, 0, sizeof(*out));
    mutex_lock(&wlan_lock);
    struct WlanDev* wd = wlan_find_locked(index);
    int32_t result = wd != NULL ? wd->ops->scan_results(wd->ctx, out) : SYS_ENOENT;
    mutex_unlock(&wlan_lock);
    return result;
}

int32_t wlan_info(uint32_t index, struct WlanInfo* out) {
    if (!wlan_ready || out == NULL)
        return SYS_ENOENT;
    memset(out, 0, sizeof(*out));
    mutex_lock(&wlan_lock);
    struct WlanDev* wd = wlan_find_locked(index);
    int32_t result = wd != NULL ? wd->ops->info(wd->ctx, out) : SYS_ENOENT;
    mutex_unlock(&wlan_lock);
    return result;
}

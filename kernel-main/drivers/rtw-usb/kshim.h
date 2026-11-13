#ifndef RTW_KSHIM_H
#define RTW_KSHIM_H

//
// The kernel as the Realtek USB Wi-Fi driver sees it. Built in, rtw_usb.c
// called the kernel's functions directly; as a driver file it only has
// `kapi` (driver/kapi.h), so the same names are kept here and passed on.
// The kernel's headers give the types (the USB interface record, the wlanN
// ops, the scan results); the calls go through the table. Only for
// drivers/rtw-usb.
//

#include <kdrv_module.h>

#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/drivers/net/wlan.h>
#include <kernel/arch/x86/drivers/net/wlan_dev.h>
#include <kernel/arch/x86/drivers/net/wlan_scan.h>
#include <kernel/net/net.h>

extern const struct KernelApi* kapi;       // rtw_usb.c: set by kdrv_entry

// Kernel heap, time
#define malloc(n)               (kapi->malloc(n))
#define free(p)                 (kapi->free(p))
#define timer_get_ms()          (kapi->time_ms())
#define thread_sleep(ms)        (kapi->sleep_ms(ms))

// Locks: a pointer from kapi->mutex_create
typedef void* mutex_t;
#define mutex_lock(m)           (kapi->mutex_lock(*(m)))
#define mutex_unlock(m)         (kapi->mutex_unlock(*(m)))

// USB (arch/x86/bus/usb/usb.h)
#define usb_dev_interface(d)                    (kapi->usb_interface(d))
#define usb_dev_control(d, t, r, v, i, p, n)    (kapi->usb_control((d), (t), (r), (v), (i), (p), (n)))
#define usb_pipe_open_bulk(d, e, in)            (kapi->usb_pipe_open_bulk((d), (e), (in)))
#define usb_pipe_transfer(p, d, n, t)           (kapi->usb_pipe_transfer((p), (d), (n), (t)))
#define usb_pipe_clear_halt(p)                  (kapi->usb_pipe_clear_halt(p))
#define usb_pipe_close(p)                       (kapi->usb_pipe_close(p))

// Wi-Fi names and scan results, the IP stack's interfaces
#define wlan_register(o, c)                 (kapi->wlan_register((o), (c)))
#define wlan_dev_name(w)                    (kapi->wlan_dev_name(w))
#define wlan_unregister(w)                  (kapi->wlan_unregister(w))
#define wlan_parse_beacon(f, l, c, o)       (kapi->wlan_parse_beacon((f), (l), (c), (o)))
#define wlan_scan_merge(t, c, m, b)         (kapi->wlan_scan_merge((t), (c), (m), (b)))
#define netif_register(n, o, d, m, t)       (kapi->netif_register((n), (o), (d), (m), (t)))
#define netif_unregister(i)                 (kapi->netif_unregister(i))
#define netif_set_mac(i, m)                 (kapi->netif_set_mac((i), (m)))

// kstd.c
char*  strncpy(char* dest, const char* src, size_t n);

#endif

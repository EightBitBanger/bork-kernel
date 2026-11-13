#ifndef _KERNEL_KAPI_H_
#define _KERNEL_KAPI_H_

// The kernel as a loaded driver sees it
//
// A driver file (driver/kdrv.h) links against nothing in the kernel: its
// entry point is handed this table and calls the kernel only through it.
// So the kernel can be rebuilt, stripped and rearranged without breaking
// drivers already built, as long as this table and the structures it passes
// (driver.h, device.h, chardev.h) keep their layout.
//
// Rules for the table:
//   - New entries only ever go at the end, and KAPI_VERSION goes up by one.
//     `size` tells a module how much of the table this kernel has.
//   - An entry is never removed or changed; a new one is added instead.
//   - A module states the lowest version it needs (KdrvHeader.kapi_version)
//     and the loader refuses it on an older kernel.
//
// What a module may and may not do with it:
//   - Class devices (chardev_register) belong in probe and come out again in
//     remove: an unloaded module's code is freed, so nothing may point into
//     it afterwards.
//   - kdrv_entry runs once, on the loading thread, with no lock held. It
//     may add the platform device its driver is for (device_add_platform)
//     and set up its own state. It must not register class devices.
//   - Every call has the rules of the kernel function it stands for (see
//     the header named next to it).

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>
#include <kernel/driver/chardev.h>

struct HdaCodecDriver;      // arch/x86/drivers/audio/hda_codec_driver.h
struct WlanOps;             // arch/x86/drivers/net/wlan_dev.h
struct WlanDev;
struct WlanBss;             // arch/x86/drivers/net/wlan.h
struct WlanLink;
struct WlanSta;             // arch/x86/drivers/net/wlan_sta.h
struct WlanStaOps;
struct NetIf;               // net/net.h
struct NetIfOps;
struct UsbInterface;        // arch/x86/bus/usb/usb.h
struct UsbPipe;

#define KAPI_VERSION        5

struct KernelApi {
    uint32_t version;               // KAPI_VERSION of this kernel
    uint32_t size;                  // sizeof(struct KernelApi) of this kernel

    // Kernel heap (malloc.h)
    void*    (*malloc)(size_t size);
    void     (*free)(void* ptr);

    // One line to COM1, prefixed with the module's name. printf formats as
    // util/snprint.h takes them; a line longer than 160 bytes is cut.
    void     (*log)(const char* format, ...);
    int      (*vsnprintf)(char* out, size_t size, const char* format, va_list args);

    // Time (util/timer.h, scheduler/scheduler.h). sleep: thread context only.
    uint64_t (*time_ms)(void);
    void     (*sleep_ms)(uint32_t ms);

    // Devices (driver/device.h)
    struct Device* (*device_add_platform)(const char* name);
    struct Device* (*device_find)(const char* name);
    void           (*device_remove)(struct Device* dev);

    // Character devices (driver/chardev.h)
    struct CharDevice* (*chardev_register)(const char* name, const struct CharDeviceOps* ops,
                                           void* driver_data, uint32_t security);
    void     (*chardev_unregister)(struct CharDevice* cd);
    bool     (*chardev_copy_from_user)(void* dst, uint32_t user_address, uint32_t length);
    bool     (*chardev_copy_to_user)(uint32_t user_address, const void* src, uint32_t length);

    // --- KAPI_VERSION 2 ---

    // Call a library driver's exported function (driver/drvcall.h): fill
    // call->argc / args (with their types) and out / out_size. Returns a
    // DrvCallResult (0: it ran, *result is what it returned). Not from
    // kdrv_entry, probe or remove.
    int32_t  (*driver_call)(const char* driver, const char* function, struct DriverCall* call,
                            int32_t* result);

    // --- KAPI_VERSION 3 ---

    // HDA codec drivers (arch/x86/drivers/audio/hda_codec_driver.h): codec
    // support on top of the HDA controller driver. Register from probe,
    // unregister from remove. Since KAPI 4 the controller driver is a file
    // itself (sys/drivers/hda): these pass the call on to it, and register
    // says false while it isn't loaded. NULL on a kernel without HDA (not x86).
    bool     (*hda_codec_register)(const struct HdaCodecDriver* d);
    void     (*hda_codec_unregister)(const struct HdaCodecDriver* d);

    // --- KAPI_VERSION 4 ---
    // What a PCI device driver needs that isn't a file's to do by itself.

    // One line on the screen console (console/print.h), as the boot
    // messages are. Keep it to one short line per device.
    void     (*console_print)(const char* text);

    // Locks (mutex.h): a sleeping mutex, made with mutex_create (NULL when
    // out of memory). Thread context only.
    void*    (*mutex_create)(void);
    void     (*mutex_destroy)(void* m);
    void     (*mutex_lock)(void* m);
    void     (*mutex_unlock)(void* m);

    // PCI (arch/x86/bus/pci/pci.h). pci_location: where a BUS_PCI device
    // sits (false for any other device). Config space by bus / slot /
    // function; offsets are bytes into the 256-byte header.
    bool     (*pci_location)(const struct Device* dev, uint8_t* bus, uint8_t* slot, uint8_t* func);
    uint8_t  (*pci_read8)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
    uint16_t (*pci_read16)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
    uint32_t (*pci_read32)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
    void     (*pci_write8)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value);
    void     (*pci_write16)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
    void     (*pci_write32)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
    // Set bits in the command register (PCI_COMMAND_MEMORY 0x2, bus master
    // 0x4, INTx disable 0x400)
    void     (*pci_enable)(uint8_t bus, uint8_t slot, uint8_t func, uint16_t bits);
    // Map memory BAR `bar` into the kernel, at most `bytes` of it, uncached
    // or not. NULL when there is no such BAR or no room. Stays mapped.
    void*    (*pci_map_bar)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar, uint32_t bytes,
                            bool uncached);

    // Memory for DMA (arch/x86/virtual/vmm.h): whole 4 KB pages in the
    // kernel window, physically below 4 GB. dma_phys gives the physical
    // address of any byte of them (each page on its own: pages from one
    // dma_alloc need not be physically next to each other).
    void*    (*dma_alloc)(uint32_t pages);
    void     (*dma_free)(void* addr, uint32_t pages);
    uint32_t (*dma_phys)(const void* addr);

    // The process making the current call (a chardev op): its id (0 for a
    // kernel thread) and its program name into `name` ("mediaplay").
    uint32_t (*caller_process)(char* name, uint32_t size);

    // Load <home>/sys/drivers/<name> (driver/autoload.h): only when it is
    // there and only an admin can change it and its folder. KAPI_LOAD_*.
    // Thread context, no lock held (a poll): never from probe or remove.
    int32_t  (*system_driver_load)(const char* name);

    // The HDA controller driver hands the kernel its codec-driver calls, so
    // hda_codec_register above reaches it; NULLs when it goes. `owner` is
    // its struct Driver: the calls are only made while that is registered.
    void     (*hda_provider_set)(const struct Driver* owner,
                                 bool (*reg)(const struct HdaCodecDriver* d),
                                 void (*unreg)(const struct HdaCodecDriver* d));

    // --- KAPI_VERSION 5 ---
    // What a network card driver needs. The card's own code is the file
    // (sys/drivers/athn-pci, rtw-usb); everything with no hardware in it
    // stays in the kernel and is reached here: the wlanN names and their
    // /dev entries, beacon parsing, the Wi-Fi station (joining, WPA2), the
    // IP stack's interfaces, the random pool. NULL on a kernel without them
    // (not x86).

    // Wi-Fi adapters (arch/x86/drivers/net/wlan_dev.h). Register from
    // probe, unregister from remove: `ops` point into the file.
    struct WlanDev* (*wlan_register)(const struct WlanOps* ops, void* ctx);
    const char*     (*wlan_dev_name)(const struct WlanDev* wd);
    void            (*wlan_unregister)(struct WlanDev* wd);

    // Scan helpers (arch/x86/drivers/net/wlan_scan.h)
    bool     (*wlan_parse_beacon)(const uint8_t* frame, uint32_t length, uint8_t channel,
                                  struct WlanBss* out);
    void     (*wlan_scan_merge)(struct WlanBss* table, uint8_t* count, uint8_t max,
                                const struct WlanBss* bss);

    // The Wi-Fi station (arch/x86/drivers/net/wlan_sta.h). The driver owns
    // the struct WlanSta (so the file and the kernel must be built from the
    // same wlan_sta.h) and calls in under its own lock.
    void     (*wlan_sta_init)(struct WlanSta* s, const struct WlanStaOps* ops, void* ctx,
                              const uint8_t mac[6]);
    int32_t  (*wlan_sta_connect)(struct WlanSta* s, const struct WlanBss* bss, const uint8_t* pmk,
                                 uint64_t now);
    void     (*wlan_sta_disconnect)(struct WlanSta* s, uint8_t reason);
    void     (*wlan_sta_rx)(struct WlanSta* s, const uint8_t* frame, uint32_t length, int8_t rssi,
                            uint64_t now);
    void     (*wlan_sta_tick)(struct WlanSta* s, uint64_t now);
    int32_t  (*wlan_sta_send)(struct WlanSta* s, const uint8_t* frame, uint32_t length);
    void     (*wlan_sta_link)(const struct WlanSta* s, struct WlanLink* out);

    // Network interfaces (net/net.h). Register from probe, unregister from
    // remove: `ops` point into the file.
    struct NetIf* (*netif_register)(const char* name, const struct NetIfOps* ops, void* driver_data,
                                    const uint8_t* mac, uint16_t mtu);
    void     (*netif_unregister)(struct NetIf* ifp);
    void     (*netif_set_link)(struct NetIf* ifp, bool link);
    void     (*netif_set_mac)(struct NetIf* ifp, const uint8_t* mac);
    void     (*netif_rx)(struct NetIf* ifp, const void* frame, uint32_t length);

    // The random pool (crypt/random.h): stir it, take bytes from it
    void     (*random_add)(const void* data, size_t n);
    void     (*random_bytes)(void* out, size_t n);

    // USB (arch/x86/bus/usb/usb.h), for a BUS_USB device. usb_speed:
    // USB_SPEED_*, 0 for any other device. usb_interface: its interface's
    // endpoints, NULL for any other device; valid until remove() returns.
    uint8_t  (*usb_speed)(const struct Device* dev);
    const struct UsbInterface* (*usb_interface)(const struct Device* dev);
    int      (*usb_control)(const struct Device* dev, uint8_t request_type, uint8_t request,
                            uint16_t value, uint16_t index, void* data, uint16_t length);
    struct UsbPipe* (*usb_pipe_open_bulk)(const struct Device* dev, uint8_t endpoint, bool in);
    int      (*usb_pipe_transfer)(struct UsbPipe* pipe, void* data, uint32_t length, uint32_t timeout_ms);
    int      (*usb_pipe_clear_halt)(struct UsbPipe* pipe);
    void     (*usb_pipe_close)(struct UsbPipe* pipe);

    // <home>/sys/firmware/<name>, whole, in a new buffer (free it with
    // `free`) with its size in *length. NULL when there is no such file, it
    // is empty or over `max` bytes, or it can't be read. Thread context, no
    // lock held (a poll): never from probe or remove.
    void*    (*firmware_load)(const char* name, uint32_t max, uint32_t* length);
};

// kapi->system_driver_load
#define KAPI_LOAD_OK            0
#define KAPI_LOAD_NO_HOME       1       // The system disk isn't mounted yet: try again later
#define KAPI_LOAD_NO_FILE       2       // No such file: nothing to load
#define KAPI_LOAD_LOADED        3       // A driver of that name is registered already
#define KAPI_LOAD_UNSAFE        4       // Users can change the file or its folder: refused
#define KAPI_LOAD_FAILED        5       // The loader refused it (see the COM1 log)

// The one table, filled in by the loader (kdrv_loader.c)
const struct KernelApi* kapi_get(void);

#endif

/*
 * devmgr: the hardware in this computer, by kind. A much simpler Device
 * Manager.
 *
 *     devmgr
 *
 * The left pane is a tree of categories (Disk drives, Display adapters,
 * Network adapters, ...) with the devices in each; the right pane shows
 * what is known about the one picked. Nothing can be changed from here.
 *
 * Everything comes from what the kernel already publishes, read like any
 * folder (no new system call, no admin rights):
 *
 *   /dev/pci/bus<n>/<dd:ff>/<class>/<property>/<value>   PCI functions
 *                   (a bridge's class folder holds the bus behind it)
 *   /dev/usb/<name>/<property>/<value>                  USB devices
 *   /dev/<name>                                         character devices
 *   /mnt/<name>                                         disks and partitions
 *                   (size from disk_format_query(), which writes nothing)
 *   /drv/<driver>/devices/<name>                        driver model devices
 *
 * A property is a folder holding one entry whose name is the value, so a
 * value is the first name readdir hands out (16 characters at most).
 *
 * The lists are read again every few seconds, and when a program says files
 * changed, so a USB device plugged in or pulled out shows up on its own.
 *
 * Keys: Up / Down / Home / End / Page Up / Page Down move, Left / Right
 * collapse / expand a category (Left on a device goes to its category),
 * Enter or Space toggles one, F5 reads everything again, Ctrl+C copies the
 * details of the device picked. Mouse: click to pick, click the arrow or
 * double click a category to open or close it, drag the scroll bar or
 * turn the wheel (WHEEL_ROWS rows a notch, Ctrl: a page). Right
 * click for the menu (Refresh, Expand all, Collapse all, Copy details,
 * Exit).
 *
 * Needs SEC_FILE_READ, SEC_GFX and SEC_IPC (the default clearance has them).
 */

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <os/disk.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/os.h>
#include <os/syscall.h>
#include <os/wm.h>
#include <os/wm_app.h>     // Not on the desktop: says so and exits before main()

#ifndef APP_NAME
#define APP_NAME        "devmgr"
#endif

#define MAX_DEVICES     128
#define MAX_PROPS       14
#define NAME_LEN        48
#define ID_LEN          64
#define KEY_LEN         20
#define VALUE_LEN       64
#define PATH_LEN        160
#define PCI_MAX_DEPTH   8           // Bridges behind bridges

#define RESCAN_MS       3000        // How often the lists are read again

// Layout
#define WIN_W           640
#define WIN_H           420
#define MIN_TREE_W      200
#define PAD             8
#define ICON            16
#define INDENT          18
#define SCROLL_W        12
#define THUMB_MIN       16
#define WHEEL_ROWS      3           // Rows a wheel notch scrolls (Ctrl: a page)

enum {
    CMD_REFRESH = 1,
    CMD_EXPAND_ALL,
    CMD_COLLAPSE_ALL,
    CMD_COPY,
    CMD_EXIT,
};

//
// Categories, in the order they are listed (alphabetical, as Windows does)
//

typedef enum {
    CAT_DISK,
    CAT_DISPLAY,
    CAT_HID,
    CAT_KEYBOARD,
    CAT_MOUSE,
    CAT_NETWORK,
    CAT_OTHER,
    CAT_SOFTWARE,
    CAT_SOUND,
    CAT_STORAGE_CTRL,
    CAT_VOLUME,
    CAT_SYSTEM,
    CAT_USB_CTRL,
    CAT_USB_DEV,
    CAT_COUNT
} category_t;

static const char* const category_names[CAT_COUNT] = {
    [CAT_DISK]         = "Disk drives",
    [CAT_DISPLAY]      = "Display adapters",
    [CAT_HID]          = "Human interface devices",
    [CAT_KEYBOARD]     = "Keyboards",
    [CAT_MOUSE]        = "Mice and pointing devices",
    [CAT_NETWORK]      = "Network adapters",
    [CAT_OTHER]        = "Other devices",
    [CAT_SOFTWARE]     = "Software devices",
    [CAT_SOUND]        = "Sound and video controllers",
    [CAT_STORAGE_CTRL] = "Storage controllers",
    [CAT_VOLUME]       = "Storage volumes",
    [CAT_SYSTEM]       = "System devices",
    [CAT_USB_CTRL]     = "USB controllers",
    [CAT_USB_DEV]      = "USB devices",
};

//
// Devices
//

typedef struct {
    char key[KEY_LEN];
    char value[VALUE_LEN];
} prop_t;

typedef struct {
    category_t category;
    char       name[NAME_LEN];      // What the tree shows
    char       id[ID_LEN];          // Stays the same across reads: keeps the selection
    prop_t     props[MAX_PROPS];
    int        prop_count;
    bool       problem;             // No driver: a warning mark on its picture
} device_t;

static device_t devices[MAX_DEVICES];
static int      device_count;
static uint32_t devices_hash;       // Of everything read, to tell when something changed

// Tree rows: a category (device -1) or one of its devices
typedef struct {
    category_t category;
    int        device;
} row_t;

static row_t rows[MAX_DEVICES + CAT_COUNT];
static int   row_count;
static bool  expanded[CAT_COUNT];
static int   cat_counts[CAT_COUNT];

// Selection, kept by what it is rather than by row
static int        sel_row = -1;
static category_t sel_category;
static char       sel_id[ID_LEN];   // "" while a category is picked

// Window
static wm_window_t main_win;
static wm_theme_t  theme;
static bool        running = true;
static bool        dirty = true;
static int32_t     win_w = WIN_W, win_h = WIN_H;
static int32_t     lh = 16, row_h = 20;
static int         scroll_top;      // First row shown
static bool        thumb_drag;
static int32_t     thumb_grab;      // Pointer y inside the thumb when it was grabbed
static uint64_t    next_scan_ms;

//
// Reading the kernel's folders
//

// The value of the property `prop` under `dir`: the first name in dir/prop
static bool read_value(const char* dir, const char* prop, char* out, size_t size) {
    char path[PATH_LEN];
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, prop) >= sizeof(path))
        return false;
    char name[SYS_NAME_MAX + 1];
    int32_t r = syscall4(SYS_READDIR, (uint32_t)path, 0, (uint32_t)name, sizeof(name));
    if (r <= 0 || SYS_IS_ERROR(r))
        return false;
    strlcpy(out, name, size);
    return true;
}

static unsigned long read_number(const char* dir, const char* prop, int base, unsigned long fallback) {
    char text[24];
    if (!read_value(dir, prop, text, sizeof(text)))
        return fallback;
    char* end;
    unsigned long v = strtoul(text, &end, base);
    return (end == text) ? fallback : v;
}

static device_t* add_device(category_t category, const char* id) {
    if (device_count >= MAX_DEVICES)
        return NULL;
    device_t* d = &devices[device_count++];
    memset(d, 0, sizeof(*d));
    d->category = category;
    strlcpy(d->id, id, sizeof(d->id));
    return d;
}

static void add_prop(device_t* d, const char* key, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void add_prop(device_t* d, const char* key, const char* fmt, ...) {
    if (d == NULL || d->prop_count >= MAX_PROPS)
        return;
    prop_t* p = &d->props[d->prop_count++];
    strlcpy(p->key, key, sizeof(p->key));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->value, sizeof(p->value), fmt, ap);
    va_end(ap);
}

// "128 MB", "1.5 GB", "512 KB"
static void format_size(uint64_t bytes, char* out, size_t size) {
    static const char* units[] = { "bytes", "KB", "MB", "GB", "TB" };
    unsigned unit = 0;
    uint64_t whole = bytes, tenth = 0;
    while (whole >= 1024 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        tenth = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        unit++;
    }
    if (unit == 0 || tenth == 0 || whole >= 100)
        snprintf(out, size, "%llu %s", (unsigned long long)whole, units[unit]);
    else
        snprintf(out, size, "%llu.%llu %s", (unsigned long long)whole, (unsigned long long)tenth, units[unit]);
}

//
// PCI
//

typedef struct {
    uint16_t    vendor, device;
    const char* name;
} pci_known_t;

// Devices common in emulators and older PCs, by vendor:device
static const pci_known_t pci_known[] = {
    { 0x8086, 0x1237, "Intel 440FX host bridge" },
    { 0x8086, 0x7000, "Intel PIIX3 ISA bridge" },
    { 0x8086, 0x7010, "Intel PIIX3 IDE controller" },
    { 0x8086, 0x7020, "Intel PIIX3 USB controller" },
    { 0x8086, 0x7113, "Intel PIIX4 power management" },
    { 0x8086, 0x100E, "Intel PRO/1000 MT network" },
    { 0x8086, 0x10D3, "Intel 82574L Gigabit network" },
    { 0x8086, 0x29C0, "Intel Q35 host bridge" },
    { 0x8086, 0x2918, "Intel ICH9 LPC bridge" },
    { 0x8086, 0x2922, "Intel ICH9 SATA controller" },
    { 0x8086, 0x2930, "Intel ICH9 SMBus controller" },
    { 0x8086, 0x2934, "Intel ICH9 USB UHCI controller" },
    { 0x8086, 0x293A, "Intel ICH9 USB EHCI controller" },
    { 0x8086, 0x24CD, "Intel ICH4 USB EHCI controller" },
    { 0x8086, 0x2415, "Intel AC'97 audio" },
    { 0x8086, 0x2668, "Intel High Definition Audio" },
    { 0x8086, 0x293E, "Intel ICH9 HD Audio" },
    { 0x1234, 0x1111, "Standard VGA graphics adapter" },
    { 0x1B36, 0x000D, "QEMU xHCI USB controller" },
    { 0x1B36, 0x0100, "QXL display adapter" },
    { 0x106B, 0x003F, "Apple USB OHCI controller" },
    { 0x10EC, 0x8139, "Realtek RTL8139 Fast Ethernet" },
    { 0x10EC, 0x8168, "Realtek RTL8111 Gigabit Ethernet" },
    { 0x1022, 0x2000, "AMD PCnet Ethernet" },
    { 0x1274, 0x5000, "Ensoniq AudioPCI" },
    { 0x1AF4, 0x1000, "Virtio network adapter" },
    { 0x1AF4, 0x1001, "Virtio block device" },
    { 0x1AF4, 0x1005, "Virtio entropy source" },
    { 0x1AF4, 0x1050, "Virtio GPU" },
    { 0x15AD, 0x0405, "VMware SVGA II adapter" },
    { 0x80EE, 0xBEEF, "VirtualBox graphics adapter" },
    { 0x80EE, 0xCAFE, "VirtualBox guest service" },
};

typedef struct {
    uint16_t    id;
    const char* name;
} vendor_t;

static const vendor_t pci_vendors[] = {
    { 0x8086, "Intel" },     { 0x1022, "AMD" },          { 0x1002, "AMD" },
    { 0x10DE, "NVIDIA" },    { 0x10EC, "Realtek" },      { 0x14E4, "Broadcom" },
    { 0x168C, "Atheros" },   { 0x1234, "QEMU" },         { 0x1B36, "Red Hat" },
    { 0x1AF4, "Virtio" },    { 0x15AD, "VMware" },       { 0x80EE, "VirtualBox" },
    { 0x1106, "VIA" },       { 0x1039, "SiS" },          { 0x106B, "Apple" },
    { 0x1274, "Ensoniq" },   { 0x1B21, "ASMedia" },      { 0x1912, "Renesas" },
    { 0x104C, "TI" },        { 0x1095, "Silicon Image" },{ 0x197B, "JMicron" },
    { 0x1033, "NEC" },       { 0x1013, "Cirrus Logic" }, { 0x5333, "S3" },
};

static const char* pci_vendor_name(uint16_t id) {
    for (size_t i = 0; i < sizeof(pci_vendors) / sizeof(pci_vendors[0]); i++)
        if (pci_vendors[i].id == id)
            return pci_vendors[i].name;
    return NULL;
}

// What a class / subclass / programming interface is
static const char* pci_kind(unsigned cls, unsigned sub, unsigned progif) {
    switch (cls) {
        case 0x00: return sub == 0x01 ? "VGA adapter" : "PCI device";
        case 0x01:
            switch (sub) {
                case 0x00: return "SCSI controller";
                case 0x01: return "IDE controller";
                case 0x02: return "Floppy controller";
                case 0x04: return "RAID controller";
                case 0x05: return "ATA controller";
                case 0x06: return "SATA controller";
                case 0x07: return "SAS controller";
                case 0x08: return "NVMe controller";
                default:   return "Storage controller";
            }
        case 0x02: return sub == 0x00 ? "Ethernet adapter" : "Network adapter";
        case 0x03:
            switch (sub) {
                case 0x00: return "VGA display adapter";
                case 0x02: return "3D display adapter";
                default:   return "Display adapter";
            }
        case 0x04:
            switch (sub) {
                case 0x00: return "Video device";
                case 0x01: return "Audio device";
                case 0x03: return "HD Audio controller";
                default:   return "Multimedia device";
            }
        case 0x05: return "Memory controller";
        case 0x06:
            switch (sub) {
                case 0x00: return "Host bridge";
                case 0x01: return "ISA bridge";
                case 0x04: return "PCI-to-PCI bridge";
                default:   return "System bridge";
            }
        case 0x07: return sub == 0x01 ? "Parallel port" : "Serial controller";
        case 0x08:
            switch (sub) {
                case 0x00: return "Interrupt controller";
                case 0x01: return "DMA controller";
                case 0x02: return "System timer";
                case 0x03: return "Real-time clock";
                default:   return "System peripheral";
            }
        case 0x09:
            switch (sub) {
                case 0x00: return "Keyboard controller";
                case 0x02: return "Mouse controller";
                default:   return "Input controller";
            }
        case 0x0C:
            if (sub == 0x03) {
                switch (progif) {
                    case 0x00: return "USB UHCI controller";
                    case 0x10: return "USB OHCI controller";
                    case 0x20: return "USB EHCI controller";
                    case 0x30: return "USB xHCI controller";
                    default:   return "USB controller";
                }
            }
            return sub == 0x05 ? "SMBus controller" : "Serial bus controller";
        case 0x0D: return "Wireless controller";
        default:   return "PCI device";
    }
}

static category_t pci_category(unsigned cls, unsigned sub) {
    switch (cls) {
        case 0x00: return sub == 0x01 ? CAT_DISPLAY : CAT_OTHER;
        case 0x01: return CAT_STORAGE_CTRL;
        case 0x02: return CAT_NETWORK;
        case 0x03: return CAT_DISPLAY;
        case 0x04: return CAT_SOUND;
        case 0x05: case 0x06: case 0x07: case 0x08:
                   return CAT_SYSTEM;
        case 0x09: return sub == 0x00 ? CAT_KEYBOARD : sub == 0x02 ? CAT_MOUSE : CAT_HID;
        case 0x0C: return sub == 0x03 ? CAT_USB_CTRL : CAT_SYSTEM;
        case 0x0D: return CAT_NETWORK;
        default:   return CAT_OTHER;
    }
}

static void pci_scan_bus(const char* bus_path, unsigned bus, int depth);

// One function: bus_path/<entry>/<class folder>/...
static void pci_add_function(const char* bus_path, unsigned bus, const char* entry, int depth) {
    char fn_path[PATH_LEN], class_folder[SYS_NAME_MAX + 1], dir[PATH_LEN];
    if ((size_t)snprintf(fn_path, sizeof(fn_path), "%s/%s", bus_path, entry) >= sizeof(fn_path))
        return;
    // The function's folder holds one folder, named after its class
    int32_t r = syscall4(SYS_READDIR, (uint32_t)fn_path, 0, (uint32_t)class_folder, sizeof(class_folder));
    if (r <= 0 || SYS_IS_ERROR(r))
        return;
    if ((size_t)snprintf(dir, sizeof(dir), "%s/%s", fn_path, class_folder) >= sizeof(dir))
        return;

    unsigned vendor = read_number(dir, "vendor", 16, 0xFFFF);
    unsigned device = read_number(dir, "device", 16, 0xFFFF);
    unsigned cls    = read_number(dir, "class", 10, 0xFF);
    unsigned sub    = read_number(dir, "subclass", 10, 0xFF);
    unsigned progif = read_number(dir, "progif", 10, 0);

    char id[ID_LEN];
    snprintf(id, sizeof(id), "pci:%u/%s", bus, entry);
    device_t* d = add_device(pci_category(cls, sub), id);
    if (d == NULL)
        return;

    const char* kind = pci_kind(cls, sub, progif);
    const char* vname = pci_vendor_name((uint16_t)vendor);
    const char* known = NULL;
    for (size_t i = 0; i < sizeof(pci_known) / sizeof(pci_known[0]); i++)
        if (pci_known[i].vendor == vendor && pci_known[i].device == device)
            known = pci_known[i].name;
    if (known != NULL)
        strlcpy(d->name, known, sizeof(d->name));
    else if (vname != NULL)
        snprintf(d->name, sizeof(d->name), "%s %s", vname, kind);
    else
        strlcpy(d->name, kind, sizeof(d->name));

    // "dd:ff", both decimal
    unsigned slot = 0, func = 0;
    if (strlen(entry) == 5 && entry[2] == ':') {
        slot = (unsigned)(entry[0] - '0') * 10 + (unsigned)(entry[1] - '0');
        func = (unsigned)(entry[3] - '0') * 10 + (unsigned)(entry[4] - '0');
    }

    add_prop(d, "Type", "%s", kind);
    add_prop(d, "Bus", "PCI");
    add_prop(d, "Location", "Bus %u, slot %u, function %u", bus, slot, func);
    if (vname != NULL)
        add_prop(d, "Vendor", "%s (%04X)", vname, vendor);
    else
        add_prop(d, "Vendor", "%04X", vendor);
    add_prop(d, "Device ID", "%04X", device);
    add_prop(d, "Class", "%02X.%02X.%02X", cls, sub, progif);

    char text[24];
    if (read_value(dir, "sub_vendor", text, sizeof(text))) {
        unsigned sv = (unsigned)strtoul(text, NULL, 16);
        unsigned sd = (unsigned)read_number(dir, "sub_id", 16, 0);
        add_prop(d, "Subsystem", "%04X:%04X", sv, sd);
    }
    unsigned pin = read_number(dir, "pin", 10, 0);
    unsigned irq = read_number(dir, "irq", 10, 0xFFFF);
    if (irq != 0xFFFF && pin >= 1 && pin <= 4)
        add_prop(d, "IRQ", "%u (pin %c)", irq, 'A' + pin - 1);
    else if (pin >= 1 && pin <= 4)
        add_prop(d, "IRQ", "pin %c, no line", 'A' + pin - 1);
    if (read_value(dir, "bar", text, sizeof(text))) {
        char high[24];
        if (read_value(dir, "bar_high", high, sizeof(high)))
            add_prop(d, "Memory", "%s:%s", high, text);
        else
            add_prop(d, "Memory", "%s", text);
    }
    add_prop(d, "Path", "%s", fn_path);

    // A bridge's class folder holds the bus behind it, as "bus<n>"
    if (cls == 0x06 && depth < PCI_MAX_DEPTH) {
        DIR* dd = opendir(dir);
        if (dd == NULL)
            return;
        struct dirent* e;
        while ((e = readdir(dd)) != NULL) {
            if (strncmp(e->d_name, "bus", 3) != 0 || e->d_name[3] < '0' || e->d_name[3] > '9')
                continue;
            char sub_path[PATH_LEN];
            if ((size_t)snprintf(sub_path, sizeof(sub_path), "%s/%s", dir, e->d_name) < sizeof(sub_path)) {
                add_prop(d, "Secondary bus", "%s", &e->d_name[3]);
                pci_scan_bus(sub_path, (unsigned)strtoul(&e->d_name[3], NULL, 10), depth + 1);
            }
        }
        closedir(dd);
    }
}

static void pci_scan_bus(const char* bus_path, unsigned bus, int depth) {
    DIR* dir = opendir(bus_path);
    if (dir == NULL)
        return;
    struct dirent* e;
    while ((e = readdir(dir)) != NULL)
        if (strchr(e->d_name, ':') != NULL)
            pci_add_function(bus_path, bus, e->d_name, depth);
    closedir(dir);
}

static void scan_pci(void) {
    DIR* dir = opendir("/dev/pci");
    if (dir == NULL)
        return;
    struct dirent* e;
    while ((e = readdir(dir)) != NULL) {
        if (strncmp(e->d_name, "bus", 3) != 0)
            continue;
        char path[PATH_LEN];
        snprintf(path, sizeof(path), "/dev/pci/%s", e->d_name);
        pci_scan_bus(path, (unsigned)strtoul(&e->d_name[3], NULL, 10), 0);
    }
    closedir(dir);
}

//
// USB
//

static void scan_usb(void) {
    DIR* dir = opendir("/dev/usb");
    if (dir == NULL)
        return;
    struct dirent* e;
    while ((e = readdir(dir)) != NULL) {
        char path[PATH_LEN];
        if ((size_t)snprintf(path, sizeof(path), "/dev/usb/%s", e->d_name) >= sizeof(path))
            continue;

        char cls[24] = "", name[24] = "", maker[24] = "", speed[16] = "", hub[24] = "", hc[16] = "";
        read_value(path, "class", cls, sizeof(cls));
        read_value(path, "name", name, sizeof(name));
        read_value(path, "maker", maker, sizeof(maker));
        read_value(path, "speed", speed, sizeof(speed));
        read_value(path, "hub", hub, sizeof(hub));
        read_value(path, "controller", hc, sizeof(hc));
        unsigned sub   = read_number(path, "subclass", 10, 0);
        unsigned proto = read_number(path, "protocol", 10, 0);

        category_t cat = CAT_USB_DEV;
        const char* kind = "USB device";
        if (strcmp(cls, "hid") == 0) {
            if (sub == 1 && proto == 1)      { cat = CAT_KEYBOARD; kind = "USB keyboard"; }
            else if (sub == 1 && proto == 2) { cat = CAT_MOUSE;    kind = "USB mouse"; }
            else                             { cat = CAT_HID;      kind = "USB input device"; }
        } else if (strcmp(cls, "hub") == 0) {
            cat = CAT_USB_CTRL; kind = "USB hub";
        } else if (strcmp(cls, "storage") == 0) {
            kind = "USB mass storage";
        } else if (strcmp(cls, "audio") == 0 || strcmp(cls, "video") == 0) {
            cat = CAT_SOUND; kind = strcmp(cls, "audio") == 0 ? "USB audio device" : "USB video device";
        } else if (strcmp(cls, "wireless") == 0 || strcmp(cls, "comm") == 0) {
            cat = CAT_NETWORK; kind = strcmp(cls, "wireless") == 0 ? "USB wireless adapter" : "USB network adapter";
        } else if (strcmp(cls, "printer") == 0) {
            kind = "USB printer";
        }

        char id[ID_LEN];
        snprintf(id, sizeof(id), "usb:%s", e->d_name);
        device_t* d = add_device(cat, id);
        if (d == NULL)
            break;

        // The folder is named after the product string, cut to fit and
        // without the word "USB" ("QEMU_Keyboard"), or after what it is
        // when it has none ("keyboard"): the first reads better than the
        // "name" value, which is the product string cut at 15 characters
        if (name[0] != '\0') {
            strlcpy(d->name, e->d_name, sizeof(d->name));
            for (char* c = d->name; *c; c++)
                if (*c == '_') *c = ' ';
        } else {
            strlcpy(d->name, kind, sizeof(d->name));
        }

        add_prop(d, "Type", "%s", kind);
        add_prop(d, "Bus", "USB");
        unsigned port = read_number(path, "port", 10, 0);
        if (strcmp(hub, "root") == 0 || hub[0] == '\0')
            add_prop(d, "Location", "Port %u, root hub of %s", port, hc[0] ? hc : "?");
        else
            add_prop(d, "Location", "Port %u of hub %s", port, hub);
        if (maker[0] != '\0')
            add_prop(d, "Manufacturer", "%s", maker);
        if (name[0] != '\0')
            add_prop(d, "Product", "%s", name);

        unsigned vendor  = read_number(path, "vendor", 16, 0);
        unsigned product = read_number(path, "product", 16, 0);
        add_prop(d, "Vendor ID", "%04X", vendor);
        add_prop(d, "Product ID", "%04X", product);
        unsigned bcd = read_number(path, "usb", 16, 0);
        if (bcd != 0)
            add_prop(d, "USB version", "%x.%02x", bcd >> 8, bcd & 0xFF);
        add_prop(d, "Class", "%s (%u, %u)", cls[0] ? cls : "?", sub, proto);

        const char* rate = strcmp(speed, "low") == 0   ? "1.5 Mbit/s" :
                           strcmp(speed, "full") == 0  ? "12 Mbit/s"  :
                           strcmp(speed, "high") == 0  ? "480 Mbit/s" :
                           strcmp(speed, "super") == 0 ? "5 Gbit/s"   : NULL;
        if (rate != NULL)
            add_prop(d, "Speed", "%s (%s)", speed, rate);
        add_prop(d, "Address", "%lu", read_number(path, "address", 10, 0));

        char serial[24];
        if (read_value(path, "serial", serial, sizeof(serial)))
            add_prop(d, "Serial", "%s", serial);

        // Kernels that say what drives each device: "hub", "usb-hid",
        // "usb-storage", a driver model driver's name, or "none"
        char driver[24];
        if (read_value(path, "driver", driver, sizeof(driver))) {
            if (strcmp(driver, "none") == 0) {
                add_prop(d, "Driver", "None");
                add_prop(d, "Status", "No driver for this device");
                d->problem = true;
            } else {
                add_prop(d, "Driver", "%s", driver);
                add_prop(d, "Status", "Working");
            }
        }
        add_prop(d, "Path", "%s", path);
    }
    closedir(dir);
}

//
// Disks and partitions (/mnt)
//

// "ata0" -> "ATA"; what the disk is attached through
static const char* disk_bus(const char* name) {
    if (strncmp(name, "ahci", 4) == 0) return "SATA (AHCI)";
    if (strncmp(name, "ata", 3) == 0)  return "ATA (IDE)";
    if (strncmp(name, "usb", 3) == 0)  return "USB mass storage";
    if (strncmp(name, "nvme", 4) == 0) return "NVMe";
    return "Block device";
}

static void scan_disks(void) {
    DIR* dir = opendir("/mnt");
    if (dir == NULL)
        return;
    struct dirent* e;
    while ((e = readdir(dir)) != NULL) {
        disk_format_t f;
        disk_format_init(&f);
        bool system = false;
        if (disk_format_query(e->d_name, &f) < 0) {
            if (errno != EACCES)
                continue;       // Not a device (or a partition that isn't mounted)
            system = true;      // The system device: still filled in
        }
        system = system || (f.info & DISK_INFO_SYSTEM);
        bool whole = (f.info & DISK_INFO_WHOLE_DISK) != 0;

        char id[ID_LEN];
        snprintf(id, sizeof(id), "mnt:%s", e->d_name);
        device_t* d = add_device(whole ? CAT_DISK : CAT_VOLUME, id);
        if (d == NULL)
            break;

        char size[24];
        format_size(f.device_bytes, size, sizeof(size));
        const char* bus = disk_bus(e->d_name);
        if (whole) {
            const char* what = strncmp(e->d_name, "usb", 3) == 0 ? "USB disk" :
                               strncmp(e->d_name, "ahci", 4) == 0 ? "SATA disk" :
                               strncmp(e->d_name, "ata", 3) == 0 ? "ATA disk" : "Disk";
            snprintf(d->name, sizeof(d->name), "%s %s (%s)", what, e->d_name, size);
        } else {
            snprintf(d->name, sizeof(d->name), "Volume %s (%s)", e->d_name, size);
        }

        add_prop(d, "Type", "%s", whole ? "Whole disk" : "Partition");
        add_prop(d, "Bus", "%s", bus);
        add_prop(d, "Capacity", "%s (%llu bytes)", size, (unsigned long long)f.device_bytes);
        if (f.device_sector_size != 0)
            add_prop(d, "Sector size", "%lu bytes", (unsigned long)f.device_sector_size);
        // A partition's query names the file system it is driven as; a whole
        // disk's names the default one, so only "formatted" is known there
        if (!(f.info & DISK_INFO_FORMATTED))
            add_prop(d, "File system", "None (unformatted)");
        else if (!whole && f.filesystem[0] != '\0')
            add_prop(d, "File system", "%s", f.filesystem);
        else
            add_prop(d, "File system", "Formatted");
        if (system)
            add_prop(d, "System", "Holds the system folders");
        add_prop(d, "Path", "/mnt/%s", e->d_name);
    }
    closedir(dir);
}

//
// Character devices (/dev/<name>) and anything else under /dev
//

static void scan_dev(void) {
    DIR* dir = opendir("/dev");
    if (dir == NULL)
        return;
    struct dirent* e;
    while ((e = readdir(dir)) != NULL) {
        if (strcmp(e->d_name, "pci") == 0 || strcmp(e->d_name, "usb") == 0)
            continue;
        char id[ID_LEN];
        snprintf(id, sizeof(id), "dev:%s", e->d_name);
        bool folder = e->d_type == DT_DIR;
        bool wlan = strncmp(e->d_name, "wlan", 4) == 0;
        bool eth  = strncmp(e->d_name, "eth", 3) == 0;
        category_t cat = (wlan || eth) ? CAT_NETWORK : folder ? CAT_OTHER : CAT_SOFTWARE;
        device_t* d = add_device(cat, id);
        if (d == NULL)
            break;

        if (strcmp(e->d_name, "null") == 0)
            strlcpy(d->name, "Null device", sizeof(d->name));
        else if (strcmp(e->d_name, "zero") == 0)
            strlcpy(d->name, "Zero device", sizeof(d->name));
        else if (wlan)
            snprintf(d->name, sizeof(d->name), "Wi-Fi interface %s", e->d_name);
        else if (eth)
            snprintf(d->name, sizeof(d->name), "Ethernet interface %s", e->d_name);
        else
            strlcpy(d->name, e->d_name, sizeof(d->name));

        add_prop(d, "Type", "%s", folder ? "Device folder" : (wlan || eth) ? "Network interface" : "Character device");
        if (strcmp(e->d_name, "null") == 0)
            add_prop(d, "Behaviour", "Reads nothing, discards writes");
        else if (strcmp(e->d_name, "zero") == 0)
            add_prop(d, "Behaviour", "Reads zeros, discards writes");
        add_prop(d, "Path", "/dev/%s", e->d_name);
    }
    closedir(dir);
}

//
// Driver model devices (/drv): the driver each one is bound to
//

static void scan_drivers(void) {
    DIR* dir = opendir("/drv");
    if (dir == NULL)
        return;
    struct dirent* e;
    while ((e = readdir(dir)) != NULL) {
        char drv_path[PATH_LEN], dev_path[PATH_LEN];
        if ((size_t)snprintf(drv_path, sizeof(drv_path), "/drv/%s", e->d_name) >= sizeof(drv_path) ||
            (size_t)snprintf(dev_path, sizeof(dev_path), "/drv/%s/devices", e->d_name) >= sizeof(dev_path))
            continue;
        char version[24] = "", bus[24] = "", origin[24] = "", type[24] = "device";
        read_value(drv_path, "version", version, sizeof(version));
        read_value(drv_path, "bus", bus, sizeof(bus));
        read_value(drv_path, "origin", origin, sizeof(origin));
        read_value(drv_path, "type", type, sizeof(type));

        // Library and service drivers hold no devices: list the driver itself
        if (strcmp(type, "device") != 0) {
            char id[ID_LEN];
            snprintf(id, sizeof(id), "drvsw:%s", e->d_name);
            device_t* d = add_device(CAT_SOFTWARE, id);
            if (d == NULL)
                break;
            bool library = strcmp(type, "library") == 0;
            snprintf(d->name, sizeof(d->name), "%s (%s driver)", e->d_name, type);
            add_prop(d, "Type", "%s", library ? "Library driver (exports functions)" : "Service driver");
            add_prop(d, "Driver", "%s %s", e->d_name, version);
            add_prop(d, "Driver origin", "%s", strcmp(origin, "builtin") == 0 ? "Built into the kernel" : "Loaded");
            if (library) {
                char exports_path[PATH_LEN];
                int functions = 0;
                if ((size_t)snprintf(exports_path, sizeof(exports_path), "%s/exports", drv_path) < sizeof(exports_path)) {
                    DIR* ex = opendir(exports_path);
                    if (ex != NULL) {
                        while (readdir(ex) != NULL)
                            functions++;
                        closedir(ex);
                    }
                }
                add_prop(d, "Functions", "%d (rundrv %s lists them)", functions, e->d_name);
            }
            add_prop(d, "Status", "Running");
            add_prop(d, "Path", "%s", drv_path);
            continue;
        }

        DIR* held = opendir(dev_path);
        if (held == NULL)
            continue;
        struct dirent* h;
        while ((h = readdir(held)) != NULL) {
            char id[ID_LEN];
            snprintf(id, sizeof(id), "drv:%s", h->d_name);
            device_t* d = add_device(strcmp(bus, "usb") == 0 ? CAT_USB_DEV : CAT_SYSTEM, id);
            if (d == NULL)
                break;
            if (strcmp(h->d_name, "mem") == 0)
                strlcpy(d->name, "Memory devices", sizeof(d->name));
            else
                snprintf(d->name, sizeof(d->name), "%s (%s)", h->d_name, e->d_name);
            add_prop(d, "Type", "Driver model device");
            add_prop(d, "Bus", "%s", bus[0] ? bus : "?");
            add_prop(d, "Name", "%s", h->d_name);
            add_prop(d, "Driver", "%s %s", e->d_name, version);
            add_prop(d, "Driver origin", "%s", strcmp(origin, "builtin") == 0 ? "Built into the kernel" : "Loaded");
            add_prop(d, "Status", "Working");
            add_prop(d, "Path", "%s", drv_path);
        }
        closedir(held);
    }
    closedir(dir);
}

//
// The tree
//

static int compare_devices(const void* a, const void* b) {
    const device_t* x = a;
    const device_t* y = b;
    if (x->category != y->category)
        return (int)x->category - (int)y->category;
    int c = strcasecmp(x->name, y->name);
    return c != 0 ? c : strcmp(x->id, y->id);
}

static uint32_t hash_bytes(uint32_t h, const char* s) {
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h * 16777619u;
}

static void build_rows(void) {
    row_count = 0;
    int d = 0;
    for (int c = 0; c < CAT_COUNT; c++) {
        cat_counts[c] = 0;
        int first = d;
        while (d < device_count && devices[d].category == (category_t)c)
            d++;
        cat_counts[c] = d - first;
        if (cat_counts[c] == 0)
            continue;
        rows[row_count++] = (row_t){ (category_t)c, -1 };
        if (expanded[c])
            for (int i = first; i < d; i++)
                rows[row_count++] = (row_t){ (category_t)c, i };
    }

    // The selection by what it was: the device, else its category
    sel_row = -1;
    for (int r = 0; r < row_count && sel_row < 0; r++) {
        if (rows[r].category != sel_category)
            continue;
        if (sel_id[0] == '\0' ? rows[r].device < 0
                              : (rows[r].device >= 0 && strcmp(devices[rows[r].device].id, sel_id) == 0))
            sel_row = r;
    }
    if (sel_row < 0) {
        for (int r = 0; r < row_count && sel_row < 0; r++)
            if (rows[r].category == sel_category && rows[r].device < 0)
                sel_row = r;
    }
    if (sel_row < 0 && row_count > 0)
        sel_row = 0;
    dirty = true;
}

// Read everything again. True if anything changed.
static bool scan(void) {
    device_count = 0;
    scan_disks();
    scan_pci();
    scan_usb();
    scan_dev();
    scan_drivers();
    qsort(devices, (size_t)device_count, sizeof(devices[0]), compare_devices);

    uint32_t h = 2166136261u;
    for (int i = 0; i < device_count; i++) {
        h = hash_bytes(h, devices[i].id);
        h = hash_bytes(h, devices[i].name);
        for (int p = 0; p < devices[i].prop_count; p++)
            h = hash_bytes(h, devices[i].props[p].value);
    }
    bool changed = h != devices_hash;
    devices_hash = h;
    if (changed)
        build_rows();
    return changed;
}

static void select_row(int r) {
    if (row_count == 0)
        return;
    if (r < 0) r = 0;
    if (r >= row_count) r = row_count - 1;
    sel_row = r;
    sel_category = rows[r].category;
    if (rows[r].device >= 0)
        strlcpy(sel_id, devices[rows[r].device].id, sizeof(sel_id));
    else
        sel_id[0] = '\0';
    dirty = true;
}

static void set_expanded(category_t c, bool open) {
    if (expanded[c] == open)
        return;
    expanded[c] = open;
    // Closing a category with its device picked picks the category
    if (!open && sel_category == c)
        sel_id[0] = '\0';
    build_rows();
}

static void set_all_expanded(bool open) {
    for (int c = 0; c < CAT_COUNT; c++)
        expanded[c] = open;
    if (!open)
        sel_id[0] = '\0';
    build_rows();
}

//
// Layout
//

static int32_t tree_w(void) {
    int32_t w = win_w * 11 / 20;
    if (w < MIN_TREE_W) w = MIN_TREE_W;
    if (w > win_w - 120) w = win_w - 120;
    return w;
}

static int32_t status_h(void) { return lh + 8; }
static int32_t tree_h(void)   { return win_h - status_h(); }
static int     rows_visible(void) { int n = (int)(tree_h() / row_h); return n > 0 ? n : 1; }
static bool    needs_scroll(void) { return row_count > rows_visible(); }

static void clamp_scroll(void) {
    int max = row_count - rows_visible();
    if (scroll_top > max) scroll_top = max;
    if (scroll_top < 0) scroll_top = 0;
}

static void scroll_to_selection(void) {
    if (sel_row < 0)
        return;
    if (sel_row < scroll_top)
        scroll_top = sel_row;
    else if (sel_row >= scroll_top + rows_visible())
        scroll_top = sel_row - rows_visible() + 1;
    clamp_scroll();
}

// The scroll bar's track and thumb (the thumb's top and height in *ty, *th)
static void thumb_rect(int32_t* ty, int32_t* th) {
    int32_t track = tree_h();
    int32_t h = track * rows_visible() / (row_count > 0 ? row_count : 1);
    if (h < THUMB_MIN) h = THUMB_MIN;
    if (h > track) h = track;
    int max = row_count - rows_visible();
    *ty = max > 0 ? (track - h) * scroll_top / max : 0;
    *th = h;
}

//
// Drawing
//

static void default_theme(void) {
    memset(&theme, 0, sizeof(theme));
    theme.client.background = 0xFF141420;  theme.client.text = 0xFFFFFFFF;
    theme.client.text_soft = 0xFFD0D0DF;   theme.client.text_muted = 0xFF8888AA;
    theme.client.accent = 0xFF04C004;      theme.client.divider_soft = 0xFF086008;
    theme.client.text_value = 0xFF3FFF3F;   theme.client.text_danger = 0xFFC00404;
    theme.edit.selection = 0xFF2E5C2E;     theme.edit.background = 0xFF202020;
    theme.button.fill = 0xFF1C1C2A;        theme.button.border = 0xFF444466;
}

static void load_theme(void) {
    if (wm_get_theme(&theme) < 0)
        default_theme();
}

// Draw text cut with ".." to fit in `width` pixels
static void draw_text_fit(gfx_surface_t* s, int32_t x, int32_t y, const char* text, int32_t width, gfx_color_t color) {
    if (width <= 0)
        return;
    if (gfx_text_width(s, text) <= width) {
        gfx_draw_text(s, x, y, text, color);
        return;
    }
    char buf[VALUE_LEN + 4];
    size_t n = strlen(text);
    if (n > VALUE_LEN) n = VALUE_LEN;
    while (n > 0) {
        memcpy(buf, text, n);
        memcpy(buf + n, "..", 3);
        if (gfx_text_width(s, buf) <= width)
            break;
        n--;
    }
    if (n > 0)
        gfx_draw_text(s, x, y, buf, color);
}

// Small pictures, one per category, in a 16x16 cell
static void draw_chip(gfx_surface_t* s, int32_t x, int32_t y, gfx_color_t body, gfx_color_t pins) {
    for (int i = 0; i < 4; i++) {
        gfx_draw_vline(s, x + 4 + i * 3, y + 1, 2, pins);
        gfx_draw_vline(s, x + 4 + i * 3, y + 13, 2, pins);
        gfx_draw_hline(s, x + 1, y + 4 + i * 3, 2, pins);
        gfx_draw_hline(s, x + 13, y + 4 + i * 3, 2, pins);
    }
    gfx_fill_rect(s, x + 3, y + 3, 10, 10, body);
    gfx_draw_rect(s, x + 3, y + 3, 10, 10, GFX_RGB(20, 20, 24));
    gfx_fill_circle(s, x + 5, y + 5, 1, pins);
}

static void draw_icon(gfx_surface_t* s, category_t c, int32_t x, int32_t y) {
    const gfx_color_t dark = GFX_RGB(24, 24, 30);
    switch (c) {
        case CAT_DISK:
            gfx_fill_rect(s, x + 1, y + 4, 14, 9, GFX_RGB(150, 156, 168));
            gfx_draw_rect(s, x + 1, y + 4, 14, 9, dark);
            gfx_draw_hline(s, x + 2, y + 5, 12, GFX_RGB(210, 214, 222));
            gfx_fill_rect(s, x + 11, y + 9, 2, 2, GFX_RGB(60, 230, 80));
            gfx_draw_hline(s, x + 3, y + 10, 6, GFX_RGB(90, 94, 104));
            break;
        case CAT_VOLUME:
            gfx_fill_rect(s, x + 1, y + 4, 14, 9, GFX_RGB(90, 130, 200));
            gfx_draw_rect(s, x + 1, y + 4, 14, 9, dark);
            gfx_fill_rect(s, x + 2, y + 5, 5, 7, GFX_RGB(150, 190, 250));
            gfx_fill_rect(s, x + 11, y + 9, 2, 2, GFX_RGB(60, 230, 80));
            break;
        case CAT_DISPLAY:
            gfx_fill_rect(s, x + 1, y + 2, 14, 10, GFX_RGB(70, 74, 86));
            gfx_fill_rect(s, x + 3, y + 4, 10, 6, GFX_RGB(70, 150, 240));
            gfx_draw_hline(s, x + 4, y + 5, 3, GFX_RGB(180, 220, 255));
            gfx_fill_rect(s, x + 7, y + 12, 2, 2, GFX_RGB(70, 74, 86));
            gfx_draw_hline(s, x + 4, y + 14, 8, GFX_RGB(110, 114, 126));
            break;
        case CAT_HID:
            gfx_fill_rect(s, x + 1, y + 5, 14, 7, GFX_RGB(120, 120, 140));
            gfx_fill_circle(s, x + 3, y + 10, 2, GFX_RGB(120, 120, 140));
            gfx_fill_circle(s, x + 12, y + 10, 2, GFX_RGB(120, 120, 140));
            gfx_draw_hline(s, x + 3, y + 8, 3, dark);
            gfx_draw_vline(s, x + 4, y + 7, 3, dark);
            gfx_set_pixel(s, x + 11, y + 7, GFX_RGB(230, 60, 60));
            gfx_set_pixel(s, x + 12, y + 9, GFX_RGB(60, 200, 80));
            break;
        case CAT_KEYBOARD:
            gfx_fill_rect(s, x, y + 4, 16, 9, GFX_RGB(200, 202, 210));
            gfx_draw_rect(s, x, y + 4, 16, 9, dark);
            for (int r = 0; r < 2; r++)
                for (int k = 0; k < 6; k++)
                    gfx_fill_rect(s, x + 2 + k * 2, y + 6 + r * 2, 1, 1, dark);
            gfx_draw_hline(s, x + 4, y + 10, 8, dark);
            break;
        case CAT_MOUSE:
            gfx_fill_circle(s, x + 8, y + 9, 5, GFX_RGB(200, 202, 210));
            gfx_fill_rect(s, x + 3, y + 5, 11, 5, GFX_RGB(200, 202, 210));
            gfx_draw_vline(s, x + 8, y + 4, 5, dark);
            gfx_draw_hline(s, x + 3, y + 8, 11, dark);
            gfx_draw_line(s, x + 8, y + 4, x + 10, y + 1, dark);
            break;
        case CAT_NETWORK:
            gfx_fill_rect(s, x + 5, y + 1, 6, 5, GFX_RGB(70, 150, 240));
            gfx_fill_rect(s, x, y + 10, 6, 5, GFX_RGB(70, 150, 240));
            gfx_fill_rect(s, x + 10, y + 10, 6, 5, GFX_RGB(70, 150, 240));
            gfx_draw_vline(s, x + 8, y + 6, 2, GFX_RGB(200, 200, 210));
            gfx_draw_hline(s, x + 3, y + 8, 11, GFX_RGB(200, 200, 210));
            gfx_draw_vline(s, x + 3, y + 8, 2, GFX_RGB(200, 200, 210));
            gfx_draw_vline(s, x + 13, y + 8, 2, GFX_RGB(200, 200, 210));
            break;
        case CAT_SOUND:
            gfx_fill_rect(s, x + 1, y + 6, 4, 5, GFX_RGB(200, 202, 210));
            for (int i = 0; i < 5; i++)
                gfx_draw_vline(s, x + 5 + i, y + 6 - i, 5 + 2 * i, GFX_RGB(200, 202, 210));
            gfx_draw_vline(s, x + 12, y + 6, 5, GFX_RGB(240, 190, 60));
            gfx_draw_vline(s, x + 14, y + 4, 9, GFX_RGB(240, 190, 60));
            break;
        case CAT_STORAGE_CTRL:
            draw_chip(s, x, y, GFX_RGB(200, 120, 40), GFX_RGB(220, 200, 120));
            break;
        case CAT_SYSTEM:
            draw_chip(s, x, y, GFX_RGB(80, 90, 110), GFX_RGB(200, 200, 210));
            break;
        case CAT_USB_CTRL:
            draw_chip(s, x, y, GFX_RGB(40, 110, 200), GFX_RGB(200, 200, 210));
            break;
        case CAT_USB_DEV: {
            gfx_color_t c2 = GFX_RGB(220, 222, 230);
            gfx_draw_vline(s, x + 8, y + 1, 11, c2);
            gfx_draw_line(s, x + 6, y + 3, x + 8, y + 1, c2);
            gfx_draw_line(s, x + 10, y + 3, x + 8, y + 1, c2);
            gfx_draw_line(s, x + 8, y + 9, x + 3, y + 6, c2);
            gfx_draw_line(s, x + 8, y + 8, x + 13, y + 5, c2);
            gfx_fill_rect(s, x + 2, y + 4, 3, 3, c2);
            gfx_fill_circle(s, x + 13, y + 5, 1, c2);
            gfx_fill_circle(s, x + 8, y + 13, 2, c2);
            break;
        }
        case CAT_SOFTWARE:
            gfx_fill_circle(s, x + 8, y + 8, 6, GFX_RGB(150, 156, 168));
            for (int i = 0; i < 4; i++) {
                static const int8_t dx[4] = { 0, 7, 0, -7 }, dy[4] = { -7, 0, 7, 0 };
                gfx_fill_rect(s, x + 7 + dx[i], y + 7 + dy[i], 2, 2, GFX_RGB(150, 156, 168));
            }
            gfx_fill_circle(s, x + 8, y + 8, 2, theme.client.background);
            break;
        default:
            gfx_fill_circle(s, x + 8, y + 8, 7, GFX_RGB(230, 190, 40));
            gfx_draw_text(s, x + 8 - gfx_text_width(s, "?") / 2, gfx_text_y(s, y, 16), "?", dark);
            break;
    }
}

// A device with a problem: a yellow triangle with "!" over its picture
static void draw_warning(gfx_surface_t* s, int32_t x, int32_t y) {
    // Dark edge, then the yellow inside, then the "!"
    for (int i = 0; i < 9; i++)
        gfx_draw_hline(s, x + 11 - i / 2 - 1, y + 7 + i, (i / 2) * 2 + 3, GFX_RGB(20, 20, 20));
    for (int i = 1; i < 8; i++)
        gfx_draw_hline(s, x + 11 - i / 2, y + 7 + i, (i / 2) * 2 + 1, GFX_RGB(250, 200, 30));
    gfx_draw_vline(s, x + 11, y + 10, 3, GFX_RGB(20, 20, 20));
    gfx_set_pixel(s, x + 11, y + 14, GFX_RGB(20, 20, 20));
}

// Open: pointing down; closed: pointing right
static void draw_expander(gfx_surface_t* s, int32_t x, int32_t y, bool open, gfx_color_t color) {
    for (int i = 0; i < 4; i++) {
        if (open)
            gfx_draw_hline(s, x + 1 + i, y + 6 + i, 7 - 2 * i, color);
        else
            gfx_draw_vline(s, x + 3 + i, y + 4 + i, 7 - 2 * i, color);
    }
}

static void draw_tree(gfx_surface_t* s) {
    int32_t w = tree_w();
    bool bar = needs_scroll();
    int32_t list_w = bar ? w - SCROLL_W : w;
    gfx_rect_t clip = gfx_rect(0, 0, list_w, tree_h());
    gfx_set_clip(s, &clip);

    int visible = rows_visible();
    for (int i = 0; i <= visible && scroll_top + i < row_count; i++) {
        int r = scroll_top + i;
        const row_t* row = &rows[r];
        int32_t y = i * row_h;
        bool picked = r == sel_row;
        if (picked)
            gfx_fill_rect(s, 0, y, list_w, row_h, theme.edit.selection);

        int32_t ty = gfx_text_y(s, y, row_h);
        int32_t iy = y + (row_h - ICON) / 2;
        if (row->device < 0) {
            int32_t x = 4;
            draw_expander(s, x, iy, expanded[row->category], theme.client.text_soft);
            x += 14;
            draw_icon(s, row->category, x, iy);
            x += ICON + 6;
            char label[64];
            snprintf(label, sizeof(label), "%s (%d)", category_names[row->category], cat_counts[row->category]);
            draw_text_fit(s, x, ty, label, list_w - x - 4, theme.client.text);
        } else {
            int32_t x = 4 + 14 + INDENT;
            draw_icon(s, row->category, x, iy);
            if (devices[row->device].problem)
                draw_warning(s, x, iy);
            x += ICON + 6;
            draw_text_fit(s, x, ty, devices[row->device].name, list_w - x - 4,
                          picked ? theme.client.text : theme.client.text_soft);
        }
    }

    if (row_count == 0)
        gfx_draw_text(s, PAD, gfx_text_y(s, 0, row_h), "No devices found", theme.client.text_muted);
    gfx_set_clip(s, NULL);

    if (bar) {
        int32_t ty, th;
        thumb_rect(&ty, &th);
        gfx_fill_rect(s, list_w, 0, SCROLL_W, tree_h(), theme.edit.background);
        gfx_fill_rect(s, list_w + 2, ty + 1, SCROLL_W - 4, th - 2,
                      thumb_drag ? theme.client.accent : theme.button.border);
    }
    gfx_draw_vline(s, w, 0, tree_h(), theme.client.divider_soft);
}

// The right pane: what is known about the device (or category) picked
static void draw_details(gfx_surface_t* s) {
    int32_t x0 = tree_w() + 1 + PAD;
    int32_t w = win_w - x0 - PAD;
    int32_t y = PAD;
    if (w <= 0)
        return;
    gfx_rect_t clip = gfx_rect(x0, 0, w, tree_h());
    gfx_set_clip(s, &clip);

    if (sel_row < 0 || sel_row >= row_count) {
        gfx_set_clip(s, NULL);
        return;
    }
    const row_t* row = &rows[sel_row];

    // Heading: picture and name, then the category under it
    draw_icon(s, row->category, x0, y + (lh - ICON) / 2);
    if (row->device >= 0 && devices[row->device].problem)
        draw_warning(s, x0, y + (lh - ICON) / 2);
    const char* title = row->device >= 0 ? devices[row->device].name : category_names[row->category];
    draw_text_fit(s, x0 + ICON + 6, y, title, w - ICON - 6, theme.client.text);
    draw_text_fit(s, x0 + ICON + 7, y, title, w - ICON - 7, theme.client.text);    // Bold
    y += lh + 2;
    if (row->device >= 0)
        draw_text_fit(s, x0 + ICON + 6, y, category_names[row->category], w - ICON - 6, theme.client.text_muted);
    y += lh + 6;
    gfx_draw_hline(s, x0, y, w, theme.client.divider_soft);
    y += 8;

    if (row->device < 0) {
        char line[64];
        int n = cat_counts[row->category];
        snprintf(line, sizeof(line), "%d device%s", n, n == 1 ? "" : "s");
        gfx_draw_text(s, x0, y, line, theme.client.text_soft);
        y += lh + 4;
        gfx_draw_text(s, x0, y, expanded[row->category] ? "Pick one to see its details."
                                                         : "Press Right or Enter to open.", theme.client.text_muted);
        gfx_set_clip(s, NULL);
        return;
    }

    const device_t* d = &devices[row->device];
    int32_t key_w = 0;
    for (int i = 0; i < d->prop_count; i++) {
        int32_t kw = gfx_text_width(s, d->props[i].key);
        if (kw > key_w) key_w = kw;
    }
    key_w += 12;
    if (key_w > w / 2) key_w = w / 2;

    for (int i = 0; i < d->prop_count && y + lh <= tree_h(); i++) {
        draw_text_fit(s, x0, y, d->props[i].key, key_w - 8, theme.client.text_muted);
        gfx_color_t vc = theme.client.text_soft;
        if (strcmp(d->props[i].key, "Status") == 0)
            vc = d->problem ? theme.client.text_danger : theme.client.text_value;
        draw_text_fit(s, x0 + key_w, y, d->props[i].value, w - key_w, vc);
        y += lh + 4;
    }
    gfx_set_clip(s, NULL);
}

static void draw_status(gfx_surface_t* s) {
    int32_t top = tree_h();
    gfx_fill_rect(s, 0, top, win_w, status_h(), theme.button.fill);
    gfx_draw_hline(s, 0, top, win_w, theme.client.divider_soft);
    int cats = 0;
    for (int c = 0; c < CAT_COUNT; c++)
        if (cat_counts[c] > 0) cats++;
    char text[64];
    snprintf(text, sizeof(text), "%d device%s in %d categor%s", device_count, device_count == 1 ? "" : "s",
             cats, cats == 1 ? "y" : "ies");
    int32_t ty = gfx_text_y(s, top, status_h());
    gfx_draw_text(s, PAD, ty, text, theme.client.text_soft);
    const char* hint = "F5 refresh  Right click: menu";
    int32_t hw = gfx_text_width(s, hint);
    if (gfx_text_width(s, text) + hw + 3 * PAD < win_w)
        gfx_draw_text(s, win_w - PAD - hw, ty, hint, theme.client.text_muted);
}

static void draw_main(void) {
    dirty = false;
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    lh = gfx_line_height(s);
    row_h = lh + 6 > ICON + 4 ? lh + 6 : ICON + 4;
    clamp_scroll();

    gfx_clear(s, theme.client.background);
    draw_tree(s);
    draw_details(s);
    draw_status(s);
    wm_window_commit(main_win, NULL);
}

//
// Copying the details
//

static void copy_details(void) {
    if (sel_row < 0 || sel_row >= row_count || rows[sel_row].device < 0)
        return;
    const device_t* d = &devices[rows[sel_row].device];
    size_t size = (size_t)(MAX_PROPS + 2) * (KEY_LEN + VALUE_LEN + 4) + NAME_LEN * 2;
    char* text = malloc(size);
    if (text == NULL)
        return;
    size_t n = (size_t)snprintf(text, size, "%s\n%s\n", d->name, category_names[d->category]);
    for (int i = 0; i < d->prop_count && n < size; i++)
        n += (size_t)snprintf(text + n, size - n, "%s: %s\n", d->props[i].key, d->props[i].value);
    wm_clip_set_text(text);
    free(text);
}

//
// Events
//

static void toggle_row(int r) {
    if (r < 0 || r >= row_count)
        return;
    if (rows[r].device < 0)
        set_expanded(rows[r].category, !expanded[rows[r].category]);
}

static void main_key(const wm_event_t* ev) {
    if (ev->mods & INPUT_MOD_CTRL) {
        if (ev->key == 'c' || ev->key == 'C')
            copy_details();
        return;
    }
    if (ev->mods & INPUT_MOD_ALT)
        return;
    int page = rows_visible() - 1 > 1 ? rows_visible() - 1 : 1;
    switch (ev->key) {
        case KEY_UP:        select_row(sel_row - 1); break;
        case KEY_DOWN:      select_row(sel_row + 1); break;
        case KEY_HOME:      select_row(0); break;
        case KEY_END:       select_row(row_count - 1); break;
        case KEY_PAGE_UP:   select_row(sel_row - page); break;
        case KEY_PAGE_DOWN: select_row(sel_row + page); break;

        case KEY_RIGHT:
            if (sel_row >= 0 && rows[sel_row].device < 0) {
                if (!expanded[rows[sel_row].category])
                    set_expanded(rows[sel_row].category, true);
                else
                    select_row(sel_row + 1);
            }
            break;

        case KEY_LEFT:
            if (sel_row >= 0) {
                if (rows[sel_row].device >= 0) {
                    sel_id[0] = '\0';           // Up to its category
                    build_rows();
                } else {
                    set_expanded(rows[sel_row].category, false);
                }
            }
            break;

        case KEY_ENTER:
        case '\r':
        case '\n':
        case ' ':
            toggle_row(sel_row);
            break;

        case KEY_F5:
            devices_hash = 0;
            scan();
            break;

        default:
            return;
    }
    scroll_to_selection();
}

static void scroll_thumb_to(int32_t pointer_y) {
    int32_t ty, th;
    thumb_rect(&ty, &th);
    int32_t span = tree_h() - th;
    int max = row_count - rows_visible();
    if (span <= 0 || max <= 0)
        return;
    int32_t top = pointer_y - thumb_grab;
    scroll_top = (int)((top * max + span / 2) / span);
    clamp_scroll();
    dirty = true;
}

static void main_mouse_down(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT || ev->y < 0 || ev->y >= tree_h())
        return;
    int32_t w = tree_w();
    if (ev->x >= w)
        return;

    if (needs_scroll() && ev->x >= w - SCROLL_W) {
        int32_t ty, th;
        thumb_rect(&ty, &th);
        if (ev->y < ty) {
            scroll_top -= rows_visible();
        } else if (ev->y >= ty + th) {
            scroll_top += rows_visible();
        } else {
            thumb_drag = true;
            thumb_grab = ev->y - ty;
        }
        clamp_scroll();
        dirty = true;
        return;
    }

    int r = scroll_top + ev->y / row_h;
    if (r >= row_count)
        return;
    select_row(r);
    // The arrow, or a double click, opens and closes a category
    if (rows[r].device < 0 && (ev->x < 4 + 14 || ev->clicks >= 2))
        toggle_row(r);
    scroll_to_selection();
}

// Wheel over the tree: scrolls it (the selection stays put)
static void main_wheel(const wm_event_t* ev) {
    if (ev->wheel == 0 || thumb_drag || ev->x >= tree_w() || ev->y >= tree_h())
        return;
    int step = (ev->mods & INPUT_MOD_CTRL) ? rows_visible() : WHEEL_ROWS;
    int before = scroll_top;
    scroll_top += ev->wheel * step;
    clamp_scroll();
    if (scroll_top != before)
        dirty = true;
}

static void show_menu(int32_t x, int32_t y) {
    bool can_copy = sel_row >= 0 && sel_row < row_count && rows[sel_row].device >= 0;
    wm_menu_item_t items[] = {
        { "Refresh",        CMD_REFRESH,      0 },
        { "Expand all",     CMD_EXPAND_ALL,   0 },
        { "Collapse all",   CMD_COLLAPSE_ALL, 0 },
        { NULL,             0,                WM_MENU_SEPARATOR },
        { "Copy details",   CMD_COPY,         can_copy ? 0 : WM_MENU_DISABLED },
        { NULL,             0,                WM_MENU_SEPARATOR },
        { "Exit",           CMD_EXIT,         0 },
    };
    wm_menu_show(main_win, x, y, items, sizeof(items) / sizeof(items[0]));
}

static void main_event(const wm_event_t* ev) {
    switch (ev->type) {
        case WM_EVENT_KEY:
            main_key(ev);
            break;

        case WM_EVENT_MOUSE_DOWN:
            main_mouse_down(ev);
            break;

        case WM_EVENT_MOUSE_MOVE:
            if (thumb_drag) {
                if (ev->buttons & INPUT_BTN_LEFT)
                    scroll_thumb_to(ev->y);
                else
                    thumb_drag = false;
            }
            break;

        case WM_EVENT_MOUSE_UP:
            if (thumb_drag) {
                thumb_drag = false;
                dirty = true;
            }
            break;

        case WM_EVENT_MOUSE_WHEEL:
            main_wheel(ev);
            break;

        case WM_EVENT_CONTEXT_MENU:
            show_menu(ev->x, ev->y);
            break;

        case WM_EVENT_MENU_COMMAND:
            switch (ev->param) {
                case CMD_REFRESH:      devices_hash = 0; scan(); break;
                case CMD_EXPAND_ALL:   set_all_expanded(true);  break;
                case CMD_COLLAPSE_ALL: set_all_expanded(false); break;
                case CMD_COPY:         copy_details(); break;
                case CMD_EXIT:         running = false; break;
            }
            scroll_to_selection();
            break;

        case WM_EVENT_RESIZED:
            win_w = ev->rect_w;
            win_h = ev->rect_h;
            dirty = true;
            break;

        case WM_EVENT_CLOSE:
            running = false;
            break;

        case WM_EVENT_DESTROYED:
            main_win = 0;
            running = false;
            break;
    }
}

//
// Main
//

int main(int argc, char** argv) {
    (void)argv;
    if (argc > 1) {
        fputs("usage: " APP_NAME "\n", stderr);
        return 1;
    }
    load_theme();
    for (int c = 0; c < CAT_COUNT; c++)
        expanded[c] = true;
    scan();
    select_row(0);

    int32_t sw = 0, sh = 0;
    wm_screen_size(&sw, &sh);
    if (sw > 0 && win_w + 32 > sw) win_w = sw - 32;
    if (sh > 0 && win_h + 64 > sh) win_h = sh - 64;

    wm_window_desc_t d;
    memset(&d, 0, sizeof(d));
    d.title  = "Device Manager";
    d.x      = WM_POS_DEFAULT;
    d.y      = WM_POS_DEFAULT;
    d.width  = win_w;
    d.height = win_h;
    d.style  = WM_STYLE_RESIZABLE | WM_STYLE_CONTEXT_MENU;

    main_win = wm_window_create(&d);
    if (main_win == 0) {
        fprintf(stderr, APP_NAME ": can't create a window: %s\n", strerror(errno));
        wm_disconnect();
        return 1;
    }
    wm_window_get_size(main_win, &win_w, &win_h);
    draw_main();
    wm_window_focus(main_win);

    // The lists are read again every RESCAN_MS; the window is repainted
    // once the queue runs dry
    next_scan_ms = os_time_ms() + RESCAN_MS;
    wm_event_t ev;
    while (running) {
        uint64_t now = os_time_ms();
        int32_t timeout = next_scan_ms > now ? (int32_t)(next_scan_ms - now) : 0;
        if (dirty)
            timeout = 0;
        int r = wm_wait_event(&ev, timeout);
        if (r < 0)
            break;                          // The dwm went away
        if (r == 0) {
            if (os_time_ms() >= next_scan_ms) {
                scan();
                next_scan_ms = os_time_ms() + RESCAN_MS;
            }
            if (dirty) draw_main();
            continue;
        }

        if (ev.type == WM_EVENT_THEME_CHANGED) {
            load_theme();
            dirty = true;
        } else if (ev.type == WM_EVENT_FS_CHANGED) {
            scan();
        } else if (ev.window == main_win) {
            main_event(&ev);
        }
    }

    wm_disconnect();
    return 0;
}

/*
 * USB devices nothing drives (usbwatch.h): /dev/usb/<device>/driver.
 */

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dwm/dwm.h"
#include "usbwatch.h"

#ifndef USB_DIR
#define USB_DIR         "/dev/usb"     // -D for the host test
#endif
#define USB_DEVICES_MAX 64              // The kernel's USB_MAX_DEVICES
#define PROP_MAX        32              // Kernel values hold 15 characters

typedef struct {
    char name[PROP_MAX];                // Its /dev/usb folder
    char key[4 * PROP_MAX];             // "<controller>/<address>/<vendor>/<product>"
} usb_seen_t;

// The devices at the last complete look
static usb_seen_t known[USB_DEVICES_MAX];
static size_t     known_count = 0;

// A property is a folder holding one entry whose name is the value.
// False when it isn't there (or the device went away meanwhile).
static bool read_prop(const char* device, const char* prop, char* out, size_t size) {
    char path[160];
    int n = snprintf(path, sizeof(path), "%s/%s/%s", USB_DIR, device, prop);
    if (n < 0 || (size_t)n >= sizeof(path))
        return false;

    DIR* d = opendir(path);
    if (d == NULL)
        return false;
    bool found = false;
    struct dirent* e;
    while (!found && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        strlcpy(out, e->d_name, size);
        found = true;
    }
    closedir(d);
    return found;
}

// The devices in /dev/usb into `seen`. -1 when a device could not be read
// all the way (the kernel rebuilds the folder after every plug and unplug):
// try again next time rather than count it gone.
static int read_devices(usb_seen_t* seen) {
    DIR* d = opendir(USB_DIR);
    if (d == NULL)
        return 0;           // No USB, or no /dev/usb yet

    int count = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && count < USB_DEVICES_MAX) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        char controller[PROP_MAX], address[PROP_MAX], vendor[PROP_MAX], product[PROP_MAX];
        if (!read_prop(e->d_name, "controller", controller, sizeof(controller)) ||
            !read_prop(e->d_name, "address", address, sizeof(address)) ||
            !read_prop(e->d_name, "vendor", vendor, sizeof(vendor)) ||
            !read_prop(e->d_name, "product", product, sizeof(product))) {
            count = -1;
            break;
        }

        usb_seen_t* s = &seen[count++];
        strlcpy(s->name, e->d_name, sizeof(s->name));
        snprintf(s->key, sizeof(s->key), "%s/%s/%s/%s", controller, address, vendor, product);
    }
    closedir(d);
    return count;
}

static bool was_known(const char* key) {
    for (size_t i = 0; i < known_count; i++) {
        if (strcmp(known[i].key, key) == 0)
            return true;
    }
    return false;
}

// "0x0bda" -> "0bda"
static const char* hex_digits(const char* value) {
    return (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) ? value + 2 : value;
}

// A new device: say so if nothing drives it
static void tell_if_driverless(const char* device) {
    char driver[PROP_MAX];
    if (!read_prop(device, "driver", driver, sizeof(driver)) || strcmp(driver, "none") != 0)
        return;         // Driven, or a kernel that doesn't say

    char vendor[PROP_MAX] = "?", product[PROP_MAX] = "?", kind[PROP_MAX] = "";
    read_prop(device, "vendor", vendor, sizeof(vendor));
    read_prop(device, "product", product, sizeof(product));
    read_prop(device, "class", kind, sizeof(kind));

    // The folder name is the product string, spaces made '_'
    char name[PROP_MAX];
    strlcpy(name, device, sizeof(name));
    for (char* c = name; *c != '\0'; c++) {
        if (*c == '_')
            *c = ' ';
    }

    bool say_kind = kind[0] != '\0' && strcmp(kind, "other") != 0 && strcmp(kind, "vendor") != 0 &&
                    strcmp(kind, "composite") != 0;
    dwm_notifyf(DWM_NOTIFY_INFO, "Unrecognized USB device", "%s (%s:%s)\nNo driver for this %s%sdevice",
                name, hex_digits(vendor), hex_digits(product), say_kind ? kind : "", say_kind ? " " : "");
}

void usbwatch_remember(void) {
    int count = read_devices(known);
    known_count = count > 0 ? (size_t)count : 0;
}

void usbwatch_check(void) {
    static usb_seen_t now[USB_DEVICES_MAX];
    int count = read_devices(now);
    if (count < 0)
        return;

    for (int i = 0; i < count; i++) {
        if (!was_known(now[i].key))
            tell_if_driverless(now[i].name);
    }

    memcpy(known, now, sizeof(now[0]) * (size_t)count);
    known_count = (size_t)count;
}

#ifndef CONSOLE_CMD_LSUSB_H
#define CONSOLE_CMD_LSUSB_H

#include <stdint.h>
#include <kernel/console/print.h>
#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/util/string.h>

// lsusb   list enumerated USB devices, hubs before what hangs off them
//
//   xhci0 1     0627:0001 hid     full  QEMU USB Tablet
//   uhci0 2     0409:55AA hub     full  QEMU USB Hub
//   uhci0 2.1   46F4:0001 storage full  QEMU USB HARDDRIVE
static void console_lsusb_pad(size_t used, size_t width) {
    for (; used < width; used++)
        print(" ");
}

int console_cmd_lsusb(int arg_count, char** args) {
    (void)arg_count;
    (void)args;

    uint32_t count = usb_device_count();
    if (count == 0) {
        print(usb_controller_count() == 0 ? "No USB host controllers\n" : "No USB devices\n");
        return 0;
    }

    for (uint32_t i = 0; i < count; i++) {
        const struct UsbDevice* dev = usb_get_device(i);
        if (dev == NULL) continue;

        // Controller name and number, e.g. "xhci0"
        char text[24];
        strncpy(text, usb_controller_name(dev->hc), 8);
        text[8] = '\0';
        utos(usb_controller_index(dev->hc), &text[strlen(text)]);
        print(text);
        console_lsusb_pad(strlen(text), 6);

        // Port path from the root hub down, e.g. "2.1"
        const struct UsbDevice* chain[USB_MAX_HUB_DEPTH + 1];
        uint32_t depth = 0;
        for (const struct UsbDevice* d = dev; d != NULL && depth <= USB_MAX_HUB_DEPTH; d = d->parent)
            chain[depth++] = d;

        text[0] = '\0';
        while (depth > 0) {
            char num[12];
            utos(chain[--depth]->port, num);
            strlcat(text, num, sizeof(text));
            if (depth > 0) strlcat(text, ".", sizeof(text));
        }
        print(text);
        console_lsusb_pad(strlen(text), 12);

        print_hex16(dev->descriptor.vendor_id);
        print(":");
        print_hex16(dev->descriptor.product_id);
        print(" ");

        uint8_t cls = dev->descriptor.device_class ? dev->descriptor.device_class : dev->if_class;
        const char* class_name = usb_class_name(cls);
        print(class_name);
        console_lsusb_pad(strlen(class_name), 8);

        const char* speed = usb_speed_name(dev->speed);
        print(speed);
        console_lsusb_pad(strlen(speed), 6);

        if (dev->manufacturer[0] != '\0') {
            print(dev->manufacturer);
            print(" ");
        }
        print(dev->product);
        print("\n");
    }
    return 0;
}

#endif

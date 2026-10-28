#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/bus/usb/usb_internal.h>
#include <kernel/arch/x86/drivers/keyboard.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/boot/x86/interrupt.h>
#include <kernel/console/mouse.h>
#include <kernel/ipc/ipc.h>
#include <kernel/scheduler/scheduler.h>

#include <kernel/malloc.h>
#include <kernel/console/print.h>
#include <kernel/util/string.h>

//
// USB keyboards and mice, boot protocol (HID 1.11 appendix B)
//
// Every interface of class HID, subclass boot, protocol keyboard or mouse
// is switched to the boot protocol, whose reports have a fixed layout, and
// its interrupt IN endpoint is polled by the "usb" thread (usb_hotplug.c).
// Key reports are turned into PS/2 Scan Code Set 1 bytes and go through the
// same decoder as the PS/2 keyboard; mouse reports go through the PS/2
// mouse's motion path.
// Everything above that (console, input grab, virtual keys) sees no
// difference.
//
// USB keyboards do not repeat keys by themselves, so held keys are
// repeated here.
//

#define USB_HID_SUBCLASS_BOOT   1
#define USB_HID_PROTOCOL_KBD    1
#define USB_HID_PROTOCOL_MOUSE  2

#define USB_HID_REQ_SET_IDLE        0x0A
#define USB_HID_REQ_SET_PROTOCOL    0x0B
#define USB_HID_BOOT_PROTOCOL       0

#define USB_HID_MAX_DEVICES     8
#define USB_HID_ERROR_LIMIT     50      // Consecutive errors before a device is dropped

#define USB_HID_REPEAT_DELAY_MS 500
#define USB_HID_REPEAT_RATE_MS  33

#define USB_KBD_REPORT          8       // Modifiers, reserved, six key usages
#define USB_KBD_ROLLOVER        0x01    // Usage in every slot: too many keys down

struct UsbHid {
    struct UsbIntPipe pipe;
    bool     keyboard;
    bool     dropped;
    uint32_t errors;

    // Keyboard: the last report, and the key being repeated
    uint8_t  last[USB_KBD_REPORT];
    uint8_t  repeat_usage;
    uint64_t repeat_at;
};

// Only the "usb" thread touches the list after boot (attach runs there
// too, from hot plug enumeration)
static struct UsbHid* hids[USB_HID_MAX_DEVICES];
static uint32_t hid_count = 0;

//
// Keyboard usages (HID usage page 7) to Scan Code Set 1. 0xE0xx is an
// extended key, sent as E0 xx. 0 means no PS/2 equivalent.
//

static const uint16_t usb_kbd_scancodes[0x66] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20,     // a b c d
    [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,     // e f g h
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,     // i j k l
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,     // m n o p
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,     // q r s t
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,     // u v w x
    [0x1C] = 0x15, [0x1D] = 0x2C,                                   // y z
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05,     // 1 2 3 4
    [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09,     // 5 6 7 8
    [0x26] = 0x0A, [0x27] = 0x0B,                                   // 9 0
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F,     // Enter Esc Backspace Tab
    [0x2C] = 0x39, [0x2D] = 0x0C, [0x2E] = 0x0D, [0x2F] = 0x1A,     // Space - = [
    [0x30] = 0x1B, [0x31] = 0x2B, [0x32] = 0x2B, [0x33] = 0x27,     // ] \ (non-US #) ;
    [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34,     // ' ` , .
    [0x38] = 0x35, [0x39] = 0x3A,                                   // / CapsLock
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E,     // F1 - F4
    [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,     // F5 - F8
    [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,     // F9 - F12
    [0x46] = 0xE037, [0x47] = 0x46,                                 // PrintScreen ScrollLock
    [0x49] = 0xE052, [0x4A] = 0xE047, [0x4B] = 0xE049,              // Insert Home PgUp
    [0x4C] = 0xE053, [0x4D] = 0xE04F, [0x4E] = 0xE051,              // Delete End PgDn
    [0x4F] = 0xE04D, [0x50] = 0xE04B, [0x51] = 0xE050, [0x52] = 0xE048,    // Right Left Down Up
    [0x53] = 0x45, [0x54] = 0xE035, [0x55] = 0x37, [0x56] = 0x4A,   // NumLock KP/ KP* KP-
    [0x57] = 0x4E, [0x58] = 0xE01C,                                 // KP+ KPEnter
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51, [0x5C] = 0x4B,     // KP1 - KP4
    [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47, [0x60] = 0x48,     // KP5 - KP8
    [0x61] = 0x49, [0x62] = 0x52, [0x63] = 0x53,                    // KP9 KP0 KP.
    [0x64] = 0x56, [0x65] = 0xE05D,                                 // non-US \ Application
};

// Modifier byte bits, in order: LCtrl LShift LAlt LGui RCtrl RShift RAlt RGui
static const uint16_t usb_kbd_modifiers[8] = {
    0x1D, 0x2A, 0x38, 0xE05B, 0xE01D, 0x36, 0xE038, 0xE05C,
};

// Interrupts are off
static void usb_kbd_send(uint16_t scancode, bool pressed) {
    if (scancode == 0)
        return;

    if (scancode & 0xFF00)
        kb_decode_scancode(0xE0);
    uint8_t code = (uint8_t)(scancode & 0x7F);
    uint16_t key = kb_decode_scancode(pressed ? code : (uint8_t)(code | 0x80));
    if (key != 0)
        input_key_push(key);
}

static uint16_t usb_kbd_scancode(uint8_t usage) {
    return usage < sizeof(usb_kbd_scancodes) / sizeof(usb_kbd_scancodes[0]) ? usb_kbd_scancodes[usage] : 0;
}

static bool usb_kbd_has(const uint8_t* report, uint8_t usage) {
    for (int i = 2; i < USB_KBD_REPORT; i++)
        if (report[i] == usage) return true;
    return false;
}

static bool usb_kbd_is_modifier(uint16_t scancode) {
    switch (scancode) {
        case 0x1D: case 0x2A: case 0x36: case 0x38: case 0x3A:
        case 0xE01D: case 0xE038:
            return true;
    }
    return false;
}

static void usb_kbd_report(struct UsbHid* hid, const uint8_t* report) {
    if (report[2] == USB_KBD_ROLLOVER)
        return;     // Phantom state: keep the last good one

    uint32_t flags = irq_save();

    uint8_t changed = (uint8_t)(report[0] ^ hid->last[0]);
    for (int bit = 0; bit < 8; bit++) {
        if (changed & (1U << bit))
            usb_kbd_send(usb_kbd_modifiers[bit], (report[0] >> bit) & 1);
    }

    // Releases first, then presses
    for (int i = 2; i < USB_KBD_REPORT; i++) {
        uint8_t usage = hid->last[i];
        if (usage > USB_KBD_ROLLOVER && !usb_kbd_has(report, usage)) {
            usb_kbd_send(usb_kbd_scancode(usage), false);
            if (usage == hid->repeat_usage)
                hid->repeat_usage = 0;
        }
    }
    for (int i = 2; i < USB_KBD_REPORT; i++) {
        uint8_t usage = report[i];
        if (usage > USB_KBD_ROLLOVER && !usb_kbd_has(hid->last, usage)) {
            uint16_t scancode = usb_kbd_scancode(usage);
            usb_kbd_send(scancode, true);
            if (scancode != 0 && !usb_kbd_is_modifier(scancode)) {
                hid->repeat_usage = usage;
                hid->repeat_at = usb_time_ms() + USB_HID_REPEAT_DELAY_MS;
            }
        }
    }

    event_signal(&input_event);
    ipc_input_irq();
    irq_restore(flags);

    memcpy(hid->last, report, USB_KBD_REPORT);
}

static void usb_kbd_repeat(struct UsbHid* hid) {
    if (hid->repeat_usage == 0 || usb_time_ms() < hid->repeat_at)
        return;
    hid->repeat_at = usb_time_ms() + USB_HID_REPEAT_RATE_MS;

    uint32_t flags = irq_save();
    usb_kbd_send(usb_kbd_scancode(hid->repeat_usage), true);
    event_signal(&input_event);
    ipc_input_irq();
    irq_restore(flags);
}

// Boot mouse report: buttons, X, Y (signed, +Y down), then optional extras
static void usb_mouse_report(const uint8_t* report, int length) {
    if (length < 3)
        return;

    uint32_t flags = irq_save();
    mouse_motion((int8_t)report[1], (int8_t)report[2], report[0] & 0x07);
    event_signal(&input_event);
    ipc_input_irq();
    irq_restore(flags);
}

//
// Polling ("usb" thread)
//

void usb_hid_poll(void) {
    uint8_t report[USB_INT_MAX_LENGTH];

    for (uint32_t i = 0; i < hid_count; i++) {
        struct UsbHid* hid = hids[i];
        if (hid->dropped)
            continue;

        struct UsbController* hc = hid->pipe.dev->hc;
        mutex_lock(&hc->lock);
        int length = hc->ops->int_poll(hc, &hid->pipe, report);
        mutex_unlock(&hc->lock);

        if (length < 0) {
            // Unplugged devices are removed by hot plug before this; a
            // device that keeps failing while plugged in is given up on
            if (++hid->errors >= USB_HID_ERROR_LIMIT) {
                hid->dropped = true;
                usb_print(hid->keyboard ? "USB: keyboard stopped answering\n" : "USB: mouse stopped answering\n");
            }
            continue;
        }
        if (length > 0) {
            hid->errors = 0;
            if (hid->keyboard && length >= USB_KBD_REPORT)
                usb_kbd_report(hid, report);
            else if (!hid->keyboard)
                usb_mouse_report(report, length);
        }
        if (hid->keyboard)
            usb_kbd_repeat(hid);
    }
}

// hc->lock held
void usb_hid_detach(struct UsbDevice* dev) {
    for (uint32_t i = 0; i < hid_count; ) {
        struct UsbHid* hid = hids[i];
        if (hid->pipe.dev != dev) {
            i++;
            continue;
        }

        // Release whatever it still held down
        if (hid->keyboard) {
            uint8_t none[USB_KBD_REPORT] = { 0 };
            usb_kbd_report(hid, none);
        } else {
            uint32_t flags = irq_save();
            mouse_motion(0, 0, 0);
            irq_restore(flags);
        }

        dev->hc->ops->int_close(dev->hc, &hid->pipe);
        free(hid);
        for (uint32_t j = i; j + 1 < hid_count; j++)
            hids[j] = hids[j + 1];
        hid_count--;
    }
}

//
// Attach (from enumeration, hc->lock held)
//

static int usb_hid_request(struct UsbDevice* dev, uint8_t request, uint16_t value, uint8_t interface) {
    return usb_control_locked(dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                              request, value, interface, NULL, 0);
}

void usb_hid_attach(struct UsbDevice* dev) {
    struct UsbController* hc = dev->hc;
    if (hc->ops->int_open == NULL || hc->ops->int_poll == NULL || hc->ops->int_close == NULL)
        return;

    for (uint32_t i = 0; i < dev->interfaces_known; i++) {
        const struct UsbInterface* itf = &dev->interfaces[i];
        if (itf->if_class != USB_CLASS_HID || itf->if_subclass != USB_HID_SUBCLASS_BOOT)
            continue;
        if (itf->if_protocol != USB_HID_PROTOCOL_KBD && itf->if_protocol != USB_HID_PROTOCOL_MOUSE)
            continue;
        if (itf->int_in_endpoint == 0)
            continue;
        if (hid_count >= USB_HID_MAX_DEVICES)
            return;

        bool keyboard = itf->if_protocol == USB_HID_PROTOCOL_KBD;

        // Boot protocol, and for keyboards a report only when something
        // changes. Devices that are always in boot protocol may stall these.
        usb_hid_request(dev, USB_HID_REQ_SET_PROTOCOL, USB_HID_BOOT_PROTOCOL, itf->number);
        if (keyboard)
            usb_hid_request(dev, USB_HID_REQ_SET_IDLE, 0, itf->number);

        struct UsbHid* hid = (struct UsbHid*)malloc(sizeof(struct UsbHid));
        if (hid == NULL)
            return;
        memset(hid, 0, sizeof(*hid));
        hid->keyboard = keyboard;
        hid->pipe.dev        = dev;
        hid->pipe.endpoint   = itf->int_in_endpoint;
        hid->pipe.interval   = itf->int_in_interval;
        hid->pipe.max_packet = itf->int_in_max_packet;

        uint16_t length = itf->int_in_max_packet;
        if (length == 0 || length > USB_INT_MAX_LENGTH) length = USB_INT_MAX_LENGTH;
        if (keyboard && length < USB_KBD_REPORT) length = USB_KBD_REPORT;
        hid->pipe.length = length;

        if (!hc->ops->int_open(hc, &hid->pipe)) {
            usb_print("USB: could not open a keyboard / mouse endpoint\n");
            free(hid);
            continue;
        }

        hids[hid_count++] = hid;
    }
}

#ifndef DESKTOP_USBWATCH_H
#define DESKTOP_USBWATCH_H

/*
 * USB devices nothing drives.
 *
 * The kernel lists every USB device in /dev/usb, one folder each, and its
 * "driver" property says what took it ("usb-hid", "usb-storage", "hub",
 * a driver's name from /drv) or "none". A device shows up there only once
 * the drivers had their chance, so "none" is final:
 *
 *     /dev/usb/802.11n_NIC/driver/none
 *
 * usbwatch_check() runs on the desktop's drive timer and shows a
 * notification for each device that appeared since the last look and has
 * no driver. Devices are told apart by controller, address, vendor and
 * product, so one pulled out and plugged back in is new again.
 *
 * Nothing is said for what was plugged in before the desktop started, and
 * nothing at all on a kernel without the "driver" property (or without
 * USB): /dev/usb missing is not an error.
 */

// Note what is plugged in now, without saying anything
void usbwatch_remember(void);

// Say which new devices have no driver
void usbwatch_check(void);

#endif

#ifndef _KERNEL_DRIVER_AUTOLOAD_H_
#define _KERNEL_DRIVER_AUTOLOAD_H_

// Driver files for hardware nothing took
//
// The drivers a machine can't do without (storage, USB, keyboard, display)
// are built in; the rest are files in <home>/sys/drivers. Once the system
// disk is mounted, the built-in "autoload" service offers every device no
// driver holds a file, by name:
//
//   PCI devices   pci-<vendor><device>            pci-80862668
//                 then the name its IDs have      athn-pci (168c:0032, AR9485 Wi-Fi)
//                 then the name its class has     hda (multimedia / HD Audio)
//   USB devices   usb-<vendor><product>           usb-0bda818b
//   (interfaces)  then the name its IDs have      rtw-usb (Realtek Wi-Fi sticks)
//
// The kernel only keeps the list of IDs and names (autoload.c); what talks
// to the hardware is in the file. One look per device and name per boot (a
// device that comes later, hot plugged, gets its look then). A file is loaded only when it and its
// folder can be changed by an admin alone (sys/ is installed write=admin):
// it runs in the kernel. A driver can load more files the same way, for
// what it finds itself (the HDA driver: hda-<codec id>), through
// kapi->system_driver_load.
//
// Results on COM1: "autoload: <path>: loaded" or why not.

#include <stdint.h>

// <home>/sys/drivers/<name>: KAPI_LOAD_* (driver/kapi.h). Thread context,
// no lock held.
int32_t driver_autoload_file(const char* name);

#endif

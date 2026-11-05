#ifndef _KERNEL_DRIVER_H_
#define _KERNEL_DRIVER_H_

// Drivers
//
// A driver comes in one of three types (Driver.type):
//
//   DRIVER_TYPE_DEVICE   says which devices it handles (a match table) and
//                        what to do when one appears or goes away. It does
//                        no I/O through this table: once bound, it hands
//                        the device to a class (block_register,
//                        chardev_register) and the rest of the kernel only
//                        ever talks to that class. The default (0).
//   DRIVER_TYPE_LIBRARY  holds no devices. It exports a table of functions
//                        (struct DriverExport) that programs call by name,
//                        like a DLL: SYS_DRV_CALL, drv_call() in the libc,
//                        <home>/sys/rundrv on the command line, and other
//                        drivers through KernelApi.driver_call.
//   DRIVER_TYPE_SERVICE  holds no devices and exports nothing: background
//                        work in poll, on the "drivers" thread.
//
// Any type may have a poll. Only device drivers have match / probe /
// remove; only libraries have exports.
//
//   driver.h / driver.c    this table, the registry, the "drivers" thread
//   device.h / device.c    devices the buses find, and binding them
//   chardev.h / chardev.c  character devices: /dev files with read / write / ioctl
//   driver_tree.c          the /drv tree
//   builtin/               drivers linked into the kernel
//
// Every registered driver and every device it holds is listed in /drv.
//
// Locking
//
// One mutex (the driver lock) covers the driver and device tables. probe
// and remove are called with it held, so they never run at the same time
// as each other. Lock order:
//
//   kernel_big_lock -> driver lock -> VFS lock -> hardware / bus locks
//
// so probe and remove may register class devices (which takes the VFS
// lock), but a bus must not call device_add / device_remove while it holds
// its own lock (USB: hc->lock). Slow work, and anything that reads files
// or mounts disks, goes in poll, which runs on the "drivers" thread with no
// lock held.

#include <stdint.h>
#include <stdbool.h>

#define DRIVER_NAME_MAX     16      // Including the terminator
#define DRIVER_MAX          32      // Drivers registered at once
#define DRIVER_ABI          2       // Bumped when Driver / Device / DriverExport change layout

// Driver.bus / Device.bus
#define BUS_PLATFORM        1       // Fixed hardware and virtual devices: matched by name
#define BUS_PCI             2
#define BUS_USB             3       // Matched per interface, not per device

#define MATCH_ANY           0xFFFF

// One row of a match table. The table ends with a row whose bus is 0.
//   PCI       vendor / product = vendor / device ID; class_code / subclass /
//             protocol = class code / subclass / prog_if
//   USB       vendor / product = idVendor / idProduct; class_code /
//             subclass / protocol = the interface's
//   PLATFORM  `name` against the device's name ("mem", "ps2kbd"); the
//             numbers are not looked at
// MATCH_ANY in a number matches anything.
struct DeviceMatch {
    uint8_t     bus;
    uint16_t    vendor;
    uint16_t    product;
    uint16_t    class_code;
    uint16_t    subclass;
    uint16_t    protocol;
    const char* name;
};

struct Device;

// Driver.flags
#define DRIVER_BUILTIN      0x0001  // Linked into the kernel (never unregistered)
#define DRIVER_HOTPLUG      0x0002  // remove() may be called while the system runs

// Driver.type
#define DRIVER_TYPE_DEVICE  0       // Binds to devices (match, probe, remove)
#define DRIVER_TYPE_LIBRARY 1       // Exports functions (exports)
#define DRIVER_TYPE_SERVICE 2       // Runs poll, nothing else
#define DRIVER_TYPE_COUNT   3

// probe() results
#define PROBE_BOUND         0       // Mine, and it is running: dev->driver_data is set
#define PROBE_NOT_MINE      1       // Not mine after all: try the next driver
#define PROBE_FAILED        2       // Mine, but it would not start: left unbound

//
// Library drivers: exported functions
//
// Every exported function has one shape:
//
//   int32_t fn(struct DriverCall* call);
//
// call->args holds exactly the parameters DriverExport.params lists, already
// checked against it: a function never sees the wrong count or type. Its
// return value goes back to the caller as the call's result (any int32_t:
// it is not an error code to the kernel). It may also answer with text: a
// NUL-terminated string in call->out, at most call->out_size bytes with the
// terminator (out starts as "", out_size is 0 when the caller wants no
// text). The kernel terminates out again afterwards and sets out_length.
//
// A function runs on the caller's thread with no lock held, in the kernel:
// it must not trust a string argument's contents (they are the caller's), and
// must not keep pointers to call or its strings after it returns.
//

#define DRIVER_EXPORT_NAME_MAX  24      // DriverExport.name, terminator included
#define DRIVER_EXPORT_HELP_MAX  64      // DriverExport.help, terminator included
#define DRIVER_EXPORT_MAX       64      // Rows in one exports table
#define DRIVER_CALL_ARGS_MAX    8       // Parameters per function
#define DRIVER_CALL_STRING_MAX  256     // Bytes per string argument, terminator included
#define DRIVER_CALL_OUTPUT_MAX  1024    // Bytes of text answer, terminator included

// Parameter types, one letter each in DriverExport.params
#define DRIVER_ARG_INT      'i'     // int32_t   (rundrv: 42, -7, 0x2A)
#define DRIVER_ARG_STRING   's'     // const char*, NUL-terminated, < DRIVER_CALL_STRING_MAX

struct DriverArg {
    char        type;               // DRIVER_ARG_*
    int32_t     i;                  // DRIVER_ARG_INT
    const char* s;                  // DRIVER_ARG_STRING
};

struct DriverCall {
    uint32_t         argc;
    struct DriverArg args[DRIVER_CALL_ARGS_MAX];
    char*            out;           // Text answer, NUL-terminated by the kernel
    uint32_t         out_size;      // Bytes in out, terminator included (0: none wanted)
    uint32_t         out_length;    // Set by the kernel after the call: strlen(out)
};

struct DriverExport {
    const char* name;               // "add": letters, digits, '_' and '-'
    const char* params;             // "ii": one DRIVER_ARG_* per parameter, "" for none
    const char* help;               // One line for listings, or NULL
    uint32_t    security;           // SYS_SEC_* bits the calling process must hold
                                    // (0: anyone). Drivers and the console always may.
    int32_t   (*fn)(struct DriverCall* call);
};

struct Driver {
    char        name[DRIVER_NAME_MAX];  // "memdev", "ahci", "usb-storage"
    uint16_t    abi;                    // DRIVER_ABI
    uint16_t    flags;                  // DRIVER_*
    const char* version;                // "1.0", shown in /drv
    const struct DeviceMatch* match;

    // A device matched. Check it, bring it up and register its class
    // devices. Returns PROBE_*. Driver lock held.
    int  (*probe)(struct Device* dev);

    // The device is going away (unplugged, or the driver unregistered).
    // Stop the hardware, unregister its class devices, free driver_data.
    // Optional (NULL: nothing to undo). Driver lock held.
    void (*remove)(struct Device* dev);

    // Optional. Called on the "drivers" thread about every poll_ms
    // (0: DRIVER_POLL_DEFAULT_MS), no lock held, for as long as the driver
    // is registered: deferred work, polled hardware, timeouts.
    void     (*poll)(void);
    uint32_t poll_ms;

    // DRIVER_TYPE_* (0: a device driver, so older initializers stay right)
    uint32_t type;

    // DRIVER_TYPE_LIBRARY: the functions it exports, ended by a row whose
    // name is NULL. NULL for the other types.
    const struct DriverExport* exports;
};

#define DRIVER_POLL_DEFAULT_MS  100

// Register a driver and offer it every device nothing holds yet (device
// drivers only). False: wrong ABI, no name, a table that does not fit its
// type (driver_shape_ok), a driver of that name already registered, or the
// table is full.
bool driver_register(const struct Driver* drv);

// remove() on every device the driver holds, then forget it. Those devices
// are offered to the other drivers. False for a driver that is not
// registered or is DRIVER_BUILTIN.
bool driver_unregister(const struct Driver* drv);

// Whether drv's fields fit its type: a device driver has a match table and
// probe and no exports; a library has a well formed exports table (names,
// params, a function on every row, no name twice) and no match table; a
// service has poll and neither. Changes nothing.
bool driver_shape_ok(const struct Driver* drv);

// "device", "library", "service"
const char* driver_type_name(uint32_t type);

uint32_t             driver_count(void);
const struct Driver* driver_get(uint32_t index);
const struct Driver* driver_find(const char* name);

// Devices `drv` holds right now
uint32_t driver_device_count(const struct Driver* drv);

// Register every built-in driver, add the platform devices and start the
// "drivers" thread. Once, at boot, after kernel_init() (needs /dev).
void driver_init(void);

// Built-in drivers are collected by the linker (section .kdrivers, see
// boot/x86/linker.ld), so adding one never means editing a table:
//
//   static const struct Driver memdev_driver = { ... };
//   DRIVER_BUILTIN_DECLARE(memdev_driver);
#define DRIVER_BUILTIN_DECLARE(drv) \
    static const struct Driver* const __kdriver_##drv \
        __attribute__((used, section(".kdrivers"))) = &(drv)

#endif

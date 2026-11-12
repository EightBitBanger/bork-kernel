#ifndef _KERNEL_DRVCALL_H_
#define _KERNEL_DRVCALL_H_

// Calling a library driver's exported functions by name
//
// A library driver (DRIVER_TYPE_LIBRARY, driver.h) is a table of functions.
// This is the one way in: SYS_DRV_CALL (syscall/calls/sys_driver.h), the
// KernelApi.driver_call entry other drivers get, and the host tests all
// come through driver_call(), which finds the driver and the function,
// checks the arguments against the function's params and the caller's
// clearance against its security, and keeps a loaded driver from being
// unloaded while the call runs (kdrv_hold).
//
// Thread context only. Not from inside a kdrv_entry, probe or remove: a load
// holds the loader's lock while those run, and a call takes it briefly.

#include <stdint.h>
#include <stdbool.h>

#include <kernel/driver/driver.h>

// clearance for callers that are the kernel (console, other drivers)
#define DRVCALL_TRUSTED     0xFFFFFFFFu

typedef enum {
    DRVCALL_OK = 0,
    DRVCALL_NO_DRIVER,      // No registered driver of that name
    DRVCALL_NOT_LIBRARY,    // It is a device driver or a service: it exports nothing
    DRVCALL_NO_FUNCTION,    // The library has no function of that name / index
    DRVCALL_BAD_ARGS,       // Wrong number or types of arguments, or a string too long
    DRVCALL_DENIED,         // The caller's clearance lacks the function's security bits
} DrvCallResult;

// What a listing shows of one export: copies, so nothing points into a
// driver that may be unloaded afterwards
typedef struct {
    char     name[DRIVER_EXPORT_NAME_MAX];
    char     params[DRIVER_CALL_ARGS_MAX + 1];
    char     help[DRIVER_EXPORT_HELP_MAX];
    uint32_t security;
} DrvExportInfo;

// Call `function` of the library `driver`. call->argc / args are the
// arguments (each with its type); call->out / out_size where the text answer
// goes (out may be NULL with out_size 0). `result` gets what the function
// returned. On anything but DRVCALL_OK nothing ran.
DrvCallResult driver_call(const char* driver, const char* function, struct DriverCall* call,
                          uint32_t clearance, int32_t* result);

// The library's index'th export (0, 1, ...), DRVCALL_NO_FUNCTION past the last
DrvCallResult driver_export_info(const char* driver, uint32_t index, DrvExportInfo* info);

// Short, static text ("no such function")
const char* drvcall_error_string(DrvCallResult result);

// The export named `function` in a library's table, NULL if none
const struct DriverExport* driver_export_find(const struct Driver* drv, const char* function);

#endif

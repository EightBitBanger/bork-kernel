#ifndef _OS_DRIVER_H
#define _OS_DRIVER_H

/*
 * Loadable drivers.
 *
 * A driver file is code that runs inside the kernel. It is known by its
 * first four bytes, "kdrv", not by its name: the drivers that come with the
 * system are in <home>/sys/drivers with no extension, and filetype_detect()
 * calls any such file "Driver". Building one: drivers/ and build-drivers.sh
 * in the kernel source.
 *
 * Loading kernel code needs SEC_ADMIN (and SEC_FILE_READ for the file). A
 * program that may not have it asks the user first:
 *
 *     drv_info_t info;
 *     if (!sec_is_admin() && sec_elevate("Load the hello driver") < 0)
 *         return -1;                       // errno says why (EACCES: said no)
 *     if (LoadLibrary("/mnt/ata0p1/sys/drivers/hello", &info) < 0)
 *         perror("hello");
 *     else
 *         printf("%s %s took %u device(s)\n", info.name, info.version, info.devices);
 *     sec_drop(SEC_ADMIN);
 *
 * Once loaded, a driver is registered like a built-in one: it binds to
 * the devices it is for, and shows up in /drv with origin "loaded". It
 * stays loaded when the program that loaded it ends.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os/syscall.h>

typedef SysDrvInfo drv_info_t;

#define DRV_NAME_MAX    SYS_DRV_NAME_MAX    // drv_info_t.name, terminator included

/*
 * Load the driver file at `path` and start it. `info` (may be NULL) gets
 * the driver's name, version and how many devices it took.
 *
 * 0, or -1 with errno:
 *   EPERM    this process has no SEC_ADMIN (or no SEC_FILE_READ)
 *   ENOENT   no such file
 *   EACCES   the file can't be read by this process
 *   ENOEXEC  not a driver file, a damaged one, or one built for another
 *            CPU or kernel version (drv_check() says which)
 *   EEXIST   a driver of that name is loaded already
 *   ENOSPC   too many drivers loaded
 *   ENOMEM   the kernel is out of memory
 *   EIO      the file could not be read, or the driver would not start
 */
int LoadLibrary(const char* path, drv_info_t* info);

/*
 * Stop a driver LoadLibrary started, by its name (drv_info_t.name, or as
 * listed in /drv): it lets go of its devices and its code is freed.
 *
 * 0, or -1 with errno: EPERM (no SEC_ADMIN), ENOENT (no loaded driver of
 * that name), EACCES (a built-in driver).
 */
int FreeLibrary(const char* name);

/*
 * Look at a file without loading it: whether it starts with "kdrv" and its
 * header makes sense. Needs SEC_FILE_READ only. `name` (may be NULL, at
 * least DRV_NAME_MAX bytes) gets the driver's name from the header.
 *
 *   1    a driver file
 *   0    not one
 *  -1    can't tell: errno from opening or reading the file
 */
int drv_check(const char* path, char* name);

/*
 * Library drivers
 *
 * A driver of type "library" binds to no hardware: it is a set of functions
 * other programs call by name, like a DLL (the functions run in the
 * kernel). Load it once with LoadLibrary (admin); after that any program
 * may call it, and <home>/sys/rundrv calls it from the command line:
 *
 *     int32_t sum;
 *     if (drv_call_ints("demolib", "add", &sum, 2, 2, 40) == 0)
 *         printf("%d\n", (int)sum);              // 42
 *
 *     drv_arg_t arg = drv_arg_string("hello");
 *     char text[64];
 *     drv_call("demolib", "upper", &arg, 1, NULL, text, sizeof(text));
 *
 * Each function has fixed parameters (drv_export_t.params, one letter each:
 * 'i' a 32-bit number, 's' a string) and returns a number; it may also
 * answer with a line of text. /drv/<name>/type says "library" for these,
 * and /drv/<name>/exports lists the function names.
 */

typedef SysDrvArg    drv_arg_t;
typedef SysDrvExport drv_export_t;

#define DRV_ARGS_MAX    SYS_DRV_ARGS_MAX    // Parameters per function
#define DRV_STRING_MAX  SYS_DRV_STRING_MAX  // Bytes per string argument, terminator included
#define DRV_OUTPUT_MAX  SYS_DRV_OUTPUT_MAX  // Bytes of text answer, terminator included
#define DRV_FUNC_MAX    SYS_DRV_FUNC_MAX    // drv_export_t.name, terminator included

static inline drv_arg_t drv_arg_int(int32_t value) {
    drv_arg_t a = { SYS_DRV_ARG_INT, (uint32_t)value };
    return a;
}

static inline drv_arg_t drv_arg_string(const char* text) {
    drv_arg_t a = { SYS_DRV_ARG_STRING, (uint32_t)(uintptr_t)text };
    return a;
}

/*
 * Call `function` of the library `driver` with `argc` arguments. `result`
 * (may be NULL) gets what the function returned, `out` (may be NULL, with
 * out_size 0) its text answer, always terminated ("" if it gave none).
 *
 * 0, or -1 with errno (nothing ran):
 *   ENOENT   no driver of that name is loaded, or it has no such function
 *   ENOEXEC  the driver is not a library (a device driver or a service)
 *   EINVAL   wrong number or types of arguments
 *   EPERM    the function needs rights this process doesn't have
 *   ENAMETOOLONG  a name or string argument too long
 */
int drv_call(const char* driver, const char* function, const drv_arg_t* args, unsigned argc,
             int32_t* result, char* out, size_t out_size);

/* drv_call with `count` int32_t arguments after it, no text answer */
int drv_call_ints(const char* driver, const char* function, int32_t* result, unsigned count, ...);

/*
 * drv_call with arguments given as text, the way a command line has them:
 * each is converted by the function's params ('i': decimal, 0x hex, 0
 * octal, or negative; 's': as it is). Looks the function up first
 * (drv_find_export). Besides drv_call's errors, EINVAL when a number isn't
 * one or the count is wrong.
 */
int drv_call_strings(const char* driver, const char* function, int argc, char** argv,
                     int32_t* result, char* out, size_t out_size);

/*
 * The library's index'th function (0, 1, ...): 1 got it, 0 past the last,
 * -1 with errno (ENOENT: no such driver, ENOEXEC: not a library).
 */
int drv_exports(const char* driver, unsigned index, drv_export_t* out);

/* The function named `function`: 1 found, 0 not there, -1 with errno */
int drv_find_export(const char* driver, const char* function, drv_export_t* out);

#endif

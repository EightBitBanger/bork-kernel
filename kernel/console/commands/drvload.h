#ifndef CONSOLE_CMD_DRVLOAD_H
#define CONSOLE_CMD_DRVLOAD_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/driver/driver.h>
#include <kernel/driver/kdrv_loader.h>
#include <kernel/util/string.h>
#include <kernel/console/commands/path_util.h>

// load <file>      load a driver file (one that starts with "kdrv") and start it
// load             list the drivers loaded from files
// unload <driver>  stop a loaded driver and free it
//
// The console is the kernel: no admin prompt, any file it can read. User
// programs go through SYS_DRV_LOAD (LoadLibrary in the libc), which needs
// admin.
static int cmd_load_list(void) {
    uint32_t shown = 0;
    char path[VFS_PATH_MAX];
    for (uint32_t i = 0; i < driver_count(); i++) {
        const struct Driver* drv = driver_get(i);
        if (drv == NULL || !kdrv_origin(drv, path, sizeof(path)))
            continue;
        print(drv->name);
        print("  ");
        print(drv->version != NULL ? drv->version : "-");
        print("  ");
        print(path);
        print("\n");
        shown++;
    }
    if (shown == 0)
        print("No drivers loaded from files\n");
    return 0;
}

static int cmd_load_run(int arg_count, char** args) {
    if (arg_count == 0)
        return cmd_load_list();

    char path[VFS_PATH_MAX];
    if (!command_resolve_path(args[0], path, sizeof(path)))
        return 1;

    KdrvInfo info;
    KdrvResult result = kdrv_load(path, &info);
    if (result != KDRV_OK) {
        print(args[0]);
        print(": ");
        print(kdrv_error_string(result));
        print("\n");
        return 2;
    }

    char number[12];
    print("Loaded ");
    print(info.name);
    if (info.version[0] != '\0') {
        print(" ");
        print(info.version);
    }
    print(", ");
    utos((info.image_size + 1023) / 1024, number);
    print(number);
    print(" KB, ");
    utos(info.devices, number);
    print(number);
    print(info.devices == 1 ? " device\n" : " devices\n");
    return 0;
}

static int cmd_unload_run(int arg_count, char** args) {
    if (arg_count == 0) {
        print("unload <driver>\n");
        return 1;
    }
    KdrvResult result = kdrv_unload(args[0]);
    if (result != KDRV_OK) {
        print(args[0]);
        print(": ");
        print(kdrv_error_string(result));
        print("\n");
        return 2;
    }
    return 0;
}

int console_cmd_load(int arg_count, char** args) {
    int result = cmd_load_run(arg_count, args);
    console_command_end();
    return result;
}

int console_cmd_unload(int arg_count, char** args) {
    int result = cmd_unload_run(arg_count, args);
    console_command_end();
    return result;
}

#endif

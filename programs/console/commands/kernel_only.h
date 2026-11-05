#ifndef CONSOLE_CMD_KERNEL_ONLY_H
#define CONSOLE_CMD_KERNEL_ONLY_H

#include "console_host.h"

// The kernel console commands that reach into kernel internals with no
// system call for a program yet:
//
//   fdisk, chkdsk, boot   the block layer and file system code directly
//   lsusb                 the USB device list
//   lsblk                 the block devices and their partition tables
//   kill                  DestroyProcess (no SYS_KILL)
//
// Each is listed here so "help" shows it and typing it says where it works
// instead of "Bad command or filename". Port one by replacing its body
// once the kernel offers what it needs (commands/<name>.h, as the others).

static int console_cmd_kernel_only(const char* name) {
    print(name);
    print(": only in the kernel console for now\n");
    console_command_end();
    return 1;
}

int console_cmd_fdisk(int arg_count, char** args)  { (void)arg_count; (void)args; return console_cmd_kernel_only("fdisk"); }
int console_cmd_chkdsk(int arg_count, char** args) { (void)arg_count; (void)args; return console_cmd_kernel_only("chkdsk"); }
int console_cmd_boot(int arg_count, char** args)   { (void)arg_count; (void)args; return console_cmd_kernel_only("boot"); }
int console_cmd_lsusb(int arg_count, char** args)  { (void)arg_count; (void)args; return console_cmd_kernel_only("lsusb"); }
int console_cmd_lsblk(int arg_count, char** args)  { (void)arg_count; (void)args; return console_cmd_kernel_only("lsblk"); }
int console_cmd_kill(int arg_count, char** args)   { (void)arg_count; (void)args; return console_cmd_kernel_only("kill"); }

#endif

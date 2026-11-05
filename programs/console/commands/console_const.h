#ifndef CONSOLE_CONST_H
#define CONSOLE_CONST_H

// Macros rather than the kernel's static pointers, so a file that uses
// only some of them builds without unused-variable warnings
#define msg_bad_command       "Bad command or filename\n"

#define msg_dir_invalid       "Invalid directory\n"
#define msg_dir_sym           "<DIR>"
#define msg_path_too_long     "Path too long\n"

#endif

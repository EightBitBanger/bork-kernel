#ifndef _SYSCALL_COMMAND_SYSTEM_H_
#define _SYSCALL_COMMAND_SYSTEM_H_

#define  SYSCALL_EXECUTE           1
// Navigation
#define  SYSCALL_CLS               2
#define  SYSCALL_LIST              4
#define  SYSCALL_CHDIR             8
#define  SYSCALL_COPY             12
// File system
#define  SYSCALL_MAKE             16
#define  SYSCALL_MAKEDIR          20
#define  SYSCALL_RENAME           21
#define  SYSCALL_REMOVE           24
// Disk
#define  SYSCALL_BOOT             28
#define  SYSCALL_FORMAT           32
#define  SYSCALL_CHKDSK           34
// Demo/application
#define  SYSCALL_TYPE             38
#define  SYSCALL_GRAPHICS         42

// Flags
#define  KSCF_COMMAND          1

#include <stddef.h>
#include <stdint.h>

int syscall(uint16_t id, char** args);

typedef int (*CommandFunction)(int arg_count, char** args);

CommandFunction syscall_get_function(uint16_t index);
const char* syscall_get_function_name(uint16_t index);
uint8_t syscall_get_flags(uint16_t index);
uint16_t syscall_get_count(void);

#endif

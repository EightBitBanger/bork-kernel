#ifndef _KERNEL_CONFIG_H_
#define _KERNEL_CONFIG_H_

//
// Kernel build configuration
//
// Switches that change kernel policy at build time. Each one can also be
// given on the compiler command line (-DNAME=0 or -DNAME=1), which wins over
// the default here.
//

// Programs typed at the console prompt run with every SYS_SEC_* bit,
// SYS_SEC_ADMIN included (full system control while the system is being
// built out). Set to 0 to give them PROCESS_SECURITY_DEFAULT, what a user
// program gets. To be replaced by sudo once there are accounts.
#ifndef CONFIG_CONSOLE_FULL_CLEARANCE
#define CONFIG_CONSOLE_FULL_CLEARANCE   1
#endif

#endif

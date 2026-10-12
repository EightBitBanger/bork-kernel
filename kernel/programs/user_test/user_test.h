#ifndef _KERNEL_USER_TEST_H_
#define _KERNEL_USER_TEST_H_

// Start the two embedded ring 3 test programs (user/user_test.asm).
// Output goes to COM1: run QEMU with -serial stdio to see it.
void user_test_launch(void);

#endif

/*
 * Process entry point. The loader jumps here in ring 3 with ESP pointing at
 * the argument block: argc, argv[0..argc-1], NULL, envp..., NULL (see the
 * "Processes" section of the kernel's syscall.h). Kept in .text.start so
 * the linker script places it at the very start of the image (a flat binary
 * can be entered at offset 0).
 */

    .section .text.start, "ax"
    .global _start
    .type _start, @function
_start:
    xorl    %ebp, %ebp          /* Ends frame-pointer backtraces */
    movl    %esp, %eax          /* The kernel's argument block */
    andl    $-16, %esp
    subl    $12, %esp
    pushl   %eax                /* i386 SysV: ESP % 16 == 0 at the CALL */
    call    __libc_start        /* __libc_start(block). Never returns */
1:  jmp     1b

    .section .note.GNU-stack, "", @progbits



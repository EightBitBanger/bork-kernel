/*
 * setjmp / longjmp for i386 cdecl. Written as top-level asm so the build
 * keeps compiling only .c files from src/.
 *
 *   jmp_buf[0..5] = EBX, ESI, EDI, EBP, ESP after return, return address
 *
 * EAX, ECX and EDX are caller-saved, so they need no slot.
 */

__asm__(
    "    .text\n"
    "    .global setjmp\n"
    "    .global _setjmp\n"
    "    .type setjmp, @function\n"
    "    .type _setjmp, @function\n"
    "setjmp:\n"
    "_setjmp:\n"
    "    movl  4(%esp), %eax\n"         /* env */
    "    movl  %ebx, 0(%eax)\n"
    "    movl  %esi, 4(%eax)\n"
    "    movl  %edi, 8(%eax)\n"
    "    movl  %ebp, 12(%eax)\n"
    "    leal  4(%esp), %ecx\n"         /* ESP as the caller sees it after return */
    "    movl  %ecx, 16(%eax)\n"
    "    movl  (%esp), %ecx\n"          /* Return address */
    "    movl  %ecx, 20(%eax)\n"
    "    xorl  %eax, %eax\n"
    "    ret\n"
    "    .size setjmp, . - setjmp\n"
    "\n"
    "    .global longjmp\n"
    "    .global _longjmp\n"
    "    .type longjmp, @function\n"
    "    .type _longjmp, @function\n"
    "longjmp:\n"
    "_longjmp:\n"
    "    movl  4(%esp), %edx\n"         /* env */
    "    movl  8(%esp), %eax\n"         /* value: 0 becomes 1 */
    "    testl %eax, %eax\n"
    "    jnz   1f\n"
    "    incl  %eax\n"
    "1:\n"
    "    movl  0(%edx), %ebx\n"
    "    movl  4(%edx), %esi\n"
    "    movl  8(%edx), %edi\n"
    "    movl  12(%edx), %ebp\n"
    "    movl  16(%edx), %esp\n"
    "    jmp   *20(%edx)\n"
    "    .size longjmp, . - longjmp\n"
);

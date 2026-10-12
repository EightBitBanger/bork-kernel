extern syscall_dispatch

global isr_syscall

; int 0x81: system calls from ring 3 (see syscall/syscall.h for the ABI).
;
; Installed as a DPL 3 TRAP gate, so interrupts stay enabled and a long
; syscall can be preempted like any other kernel code. Entered from ring 3,
; the CPU has already switched to this thread's kernel stack (TSS esp0) and
; pushed SS, ESP, EFLAGS, CS, EIP.
;
; Frame passed to C (struct SyscallFrame in syscall.c), low to high:
;   gs fs es ds | edi esi ebp esp ebx edx ecx eax | eip cs eflags user_esp user_ss
;
; The result goes back in the frame's EAX slot, which popad loads.
; The user's FPU/SSE registers are saved around the C code (GCC may use XMM).
isr_syscall:
    pushad
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10               ; Kernel data segments
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ebx, esp               ; Frame pointer (EBX is callee-saved in C)
    sub esp, 512
    and esp, 0xFFFFFFF0
    fxsave [esp]

    sub esp, 12                ; 12 + 4 (argument) = 16: ESP % 16 == 0 at the CALL
    push ebx                   ; Argument: struct SyscallFrame*
    call syscall_dispatch
    add esp, 16

    fxrstor [esp]
    mov esp, ebx

    pop gs
    pop fs
    pop es
    pop ds
    popad
    iret

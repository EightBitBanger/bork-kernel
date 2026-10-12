; Generic CPU exception stubs (vectors 0-31).
;
; Previously every vector pointed at isr_dummy, which sent a PIC EOI and
; executed iret. For exceptions that push an error code that iret pops the
; error code as EIP (double fault -> triple fault), and for #DE it returns to
; the same faulting instruction forever. These stubs report and halt instead.
;
; An exception raised in ring 3 only kills the process that raised it
; (process_user_fault); a kernel exception still goes to the panic screen.

extern isr_callback_exception_handler
extern process_user_fault

global isr_exception_table

%macro EXC_NOERR 1
isr_exc%1:
    push dword 0           ; dummy error code
    push dword %1          ; vector
    jmp exception_common
%endmacro

%macro EXC_ERR 1
isr_exc%1:
    push dword %1          ; vector (CPU already pushed the error code)
    jmp exception_common
%endmacro

section .text

EXC_NOERR 0
EXC_NOERR 1
EXC_NOERR 2
EXC_NOERR 3
EXC_NOERR 4
EXC_NOERR 5
EXC_NOERR 6
EXC_NOERR 7
EXC_ERR   8
EXC_NOERR 9
EXC_ERR   10
EXC_ERR   11
EXC_ERR   12
EXC_ERR   13
EXC_ERR   14
EXC_NOERR 15
EXC_NOERR 16
EXC_ERR   17
EXC_NOERR 18
EXC_NOERR 19
EXC_NOERR 20
EXC_ERR   21
EXC_NOERR 22
EXC_NOERR 23
EXC_NOERR 24
EXC_NOERR 25
EXC_NOERR 26
EXC_NOERR 27
EXC_NOERR 28
EXC_ERR   29
EXC_ERR   30
EXC_NOERR 31

; Stack on entry: [vector] [error code] [EIP] [CS] [EFLAGS] (+ [ESP] [SS] from ring 3)
exception_common:
    cli

    mov ax, 0x10           ; Kernel data segments (DS may be the user's 0x23)
    mov ds, ax
    mov es, ax

    ; Callee-saved registers, so they survive the first C call
    mov edi, [esp + 0]     ; vector
    mov ebx, [esp + 4]     ; error code
    mov esi, [esp + 8]     ; EIP
    mov ebp, [esp + 12]    ; CS

    and esp, 0xFFFFFFF0

    ; process_user_fault(vector, error_code, eip, cs, address): 12 + 5 * 4 = 32
    sub esp, 12
    push dword 0
    push ebp
    push esi
    push ebx
    push edi
    call process_user_fault
    add esp, 32

    ; Kernel exception. 4 (pad) + 3 arguments * 4 = 16
    ; cdecl: isr_callback_exception_handler(vector, error_code, eip)
    sub esp, 4
    push esi
    push ebx
    push edi
    call isr_callback_exception_handler

.hang:
    cli
    hlt
    jmp .hang

section .rodata
align 4
isr_exception_table:
%assign i 0
%rep 32
    dd isr_exc %+ i
%assign i i+1
%endrep

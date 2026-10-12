extern isr_callback_fault_handler
extern process_user_fault

global isr_general_fault

isr_general_fault:

    ; The CPU has already pushed EFLAGS, CS, EIP, and the Error Code
    ; (plus SS and ESP above them when the fault came from ring 3).
    ; For #GP the error code is a segment selector index (or 0), not page-fault bits.

    pusha                  ; error code at esp + 32, EIP at +36, CS at +40

    mov ax, 0x10           ; Kernel data segments (DS may be the user's 0x23)
    mov ds, ax
    mov es, ax

    mov ebx, [esp + 32]    ; Error code (selector)
    mov esi, [esp + 36]    ; Faulting EIP (CR2 is meaningless for #GP)
    mov edi, [esp + 40]    ; CS

    and esp, 0xFFFFFFF0

    ; process_user_fault(vector, error_code, eip, cs, address): 12 + 5 * 4 = 32.
    ; Ring 3 (cli, in/out, int 0x80, ...): kills the process, never returns.
    sub esp, 12
    push dword 0
    push edi
    push esi
    push ebx
    push dword 13
    call process_user_fault
    add esp, 32

    ; Kernel fault: the handler never returns. 4 + 3 * 4 = 16.
    sub esp, 4
    push dword 0x00        ; Third argument: fault type (PT_GENERAL_PROTECTION_FAULT)
    push esi               ; Second argument: faulting EIP
    push ebx               ; First argument: error_code

    call isr_callback_fault_handler

    cli
.hang:
    hlt
    jmp .hang

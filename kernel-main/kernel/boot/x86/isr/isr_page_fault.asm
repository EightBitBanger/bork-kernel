extern isr_callback_fault_handler
extern process_user_fault

global isr_page_fault

isr_page_fault:

    ; The CPU has already pushed EFLAGS, CS, EIP, and the Error Code
    ; (plus SS and ESP above them when the fault came from ring 3).
    ; Stack: [Error Code] [EIP] [CS] [EFLAGS] ... <- ESP points at the error code

    ; Push all general-purpose registers to preserve state for the panic screen
    pusha                  ; error code now at esp + 32, EIP at +36, CS at +40

    mov ax, 0x10           ; Kernel data segments (DS may be the user's 0x23)
    mov ds, ax
    mov es, ax
    cld                    ; Ring 3 can leave DF set; C needs it clear

    ; Callee-saved registers, so they survive the first C call
    mov ebx, [esp + 32]    ; Error code
    mov esi, [esp + 36]    ; Faulting EIP
    mov edi, [esp + 40]    ; CS
    mov ebp, cr2           ; Faulting linear address

    and esp, 0xFFFFFFF0

    ; process_user_fault(vector, error_code, eip, cs, address)
    ; 12 (pad) + 5 arguments * 4 = 32. A ring 3 fault kills the process and
    ; does not come back; a kernel fault returns here.
    sub esp, 12
    push ebp
    push edi
    push esi
    push ebx
    push dword 14
    call process_user_fault
    add esp, 32

    ; Kernel fault: the handler never returns. 4 (pad) + 3 arguments * 4 = 16.
    sub esp, 4
    push dword 0x01        ; Third argument: fault type (PT_PAGE_FAULT)
    push ebp               ; Second argument: faulting_address
    push ebx               ; First argument: error_code

    call isr_callback_fault_handler

    ; The handler never returns
    cli
.hang:
    hlt
    jmp .hang


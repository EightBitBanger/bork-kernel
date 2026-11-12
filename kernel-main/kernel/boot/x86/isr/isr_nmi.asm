extern wd_nmi_handler

global isr_nmi

; NMI (vector 2): hard-lockup watchdog tick (RTC via IOAPIC) and chipset
; error reports. See wd_nmi_handler in scheduler/watchdog.c.
;
; An NMI can land anywhere, including inside other ISRs and the scheduler,
; so everything is saved and restored, FPU/SSE state included. The CPU
; blocks further NMIs until the iret.
isr_nmi:
    pusha                  ; Frame for the C handler: pusha block + EIP/CS/EFLAGS
    push ds                ; User code may have left any selector here,
    push es                ; including null: reload the kernel's before C
    push fs
    push gs
    mov ax, 0x10           ; EAX was saved by pusha
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld                    ; Ring 3 can leave DF set; C needs it clear (iret restores it)

    lea ebp, [esp + 16]    ; Frame pointer: the pusha block, above the 4 selectors
    sub esp, 512
    and esp, 0xFFFFFFF0
    fxsave [esp]

    sub esp, 12            ; 12 + 4 (argument) = 16: ESP % 16 == 0 at the CALL
    push ebp               ; Argument: address of the pusha block
    call wd_nmi_handler
    add esp, 16

    fxrstor [esp]
    lea esp, [ebp - 16]    ; Back to the saved selectors
    pop gs
    pop fs
    pop es
    pop ds
    popa
    iret


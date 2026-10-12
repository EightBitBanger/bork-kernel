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

    mov ebp, esp           ; Frame pointer (EBP saved by pusha, callee-saved in C)
    sub esp, 512
    and esp, 0xFFFFFFF0
    fxsave [esp]

    sub esp, 12            ; 12 + 4 (argument) = 16: ESP % 16 == 0 at the CALL
    push ebp               ; Argument: address of the pusha block
    call wd_nmi_handler
    add esp, 16

    fxrstor [esp]
    mov esp, ebp
    popa
    iret

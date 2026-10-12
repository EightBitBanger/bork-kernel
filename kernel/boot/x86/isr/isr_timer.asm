extern isr_callback_timer_handler

global isr_timer

; NOTE: unused; vector 0x20 is scheduler_context. Kept aligned for safety.
isr_timer:
    pusha                  ; Save all general-purpose registers
    
    ; Carve out a 512-byte FXSAVE area and align it to 16 bytes. Aligning ESP
    ; itself also satisfies the i386 SysV ABI, which requires ESP % 16 == 0 at
    ; every CALL into C (GCC may use movaps on stack slots otherwise).
    mov ebp, esp           ; EBP was saved by pusha; callee-saved across the call
    sub esp, 512
    and esp, 0xFFFFFFF0
    
    fxsave [esp]           ; Save the entire FPU/MMX/SSE/XMM state
    
    call isr_callback_timer_handler
    
    fxrstor [esp]          ; Restore the entire FPU/MMX/SSE/XMM state
    
    mov esp, ebp           ; Drop the FXSAVE area and alignment padding
    popa                   ; Restore registers
    iret

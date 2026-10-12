global scheduler_context
extern thread_handler_c
extern isr_callback_timer_handler
extern current_thread

%define TB_FPU_STATE 16            ; offsetof(ThreadBlock, fpu_state), checked in scheduler.c

align 4

; Timer IRQ (vector 0x20): tick the clock, then let the scheduler pick a thread.
;
; The saved frame (segment regs + pushad + CPU iret frame) must stay exactly
; where it is, since its address is the thread's saved ESP. Alignment is done
; below it using EBX as the frame pointer, then ESP is replaced wholesale by
; the value thread_handler_c returns.
;
; FPU/SSE state is saved here, BEFORE any C code runs, into the outgoing
; thread's fpu_state, and the incoming thread's is restored right before the
; switch. (thread_handler_c used to do this at its very end, after code that
; may itself use XMM registers.)
scheduler_context:
    pushad
    push ds
    push es
    push fs
    push gs
    
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    mov eax, [current_thread]
    test eax, eax
    jz .no_save
    mov eax, [eax + TB_FPU_STATE]
    fxsave [eax]
.no_save:
    
    mov ebx, esp               ; Frame pointer (EBX saved by pushad, callee-saved in C)
    and esp, 0xFFFFFFF0        ; ESP % 16 == 0 at each CALL (i386 SysV ABI)
    
    call isr_callback_timer_handler
    
    sub esp, 12                ; 12 + 4 (argument) = 16
    push ebx                   ; Argument: address of the saved frame
    call thread_handler_c
    
    mov esp, eax               ; Switch to the chosen thread's saved frame
    
    mov eax, [current_thread]
    test eax, eax
    jz .no_restore
    mov eax, [eax + TB_FPU_STATE]
    fxrstor [eax]
.no_restore:
    
    pop gs
    pop fs
    pop es
    pop ds
    popad
    iret

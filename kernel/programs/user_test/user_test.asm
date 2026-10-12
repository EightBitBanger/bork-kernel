; user_test.asm -- two tiny flat ring 3 programs embedded in the kernel image
;
; There is no executable loader yet, so these are raw bytes the kernel copies
; to PROCESS_IMAGE_BASE (0x40000000) with CreateProcessFromImage(). They live
; in .rodata: the kernel never runs them in place. Jumps are relative; every
; absolute address goes through the U() macro, which turns "offset in this
; blob" into "address in the user address space".
;
;   hello   prints, checks that a kernel pointer is refused (-EFAULT),
;           grows its heap with sbrk, prints a line from heap memory and
;           shrinks it again, ticks five times a second apart, exits 0
;   crash   prints, then writes to kernel memory: the page fault must kill
;           it and leave the kernel running
;
; Assemble: nasm -f elf32 user_test.asm -o user_test.o

BITS 32

%define USER_BASE       0x40000000
%define SYSCALL         0x81

%define SYS_EXIT        0
%define SYS_PRINT       1
%define SYS_YIELD       2
%define SYS_SLEEP       3
%define SYS_GETPID      4
%define SYS_SBRK        5

%define SYS_EFAULT      -2
%define SYS_ERROR_MIN   -4095

; print(label, length)
%macro PRINT 2
    mov eax, SYS_PRINT
    mov ebx, U(%1)
    mov ecx, %2
    int SYSCALL
%endmacro

section .rodata
align 16

; ---------------------------------------------------------------------------
; hello
; ---------------------------------------------------------------------------

global user_hello_start
global user_hello_end

%define U(label) (USER_BASE + ((label) - user_hello_start))

user_hello_start:
    PRINT   hello_msg, hello_msg_len

    ; A pointer into the kernel must be rejected, not printed
    mov     eax, SYS_PRINT
    mov     ebx, 0x00100000         ; Kernel image
    mov     ecx, 16
    int     SYSCALL
    cmp     eax, SYS_EFAULT
    jne     .check_failed
    PRINT   hello_check_ok, hello_check_ok_len
    jmp     .heap
.check_failed:
    PRINT   hello_check_bad, hello_check_bad_len

    ; Heap: sbrk(8192) maps two zeroed pages and returns their start
.heap:
    mov     eax, SYS_SBRK
    mov     ebx, 8192
    int     SYSCALL
    cmp     eax, SYS_ERROR_MIN      ; Errors are [-4095, -1]: unsigned compare
    jae     .heap_failed
    mov     ebp, eax                ; Heap block

    mov     edi, ebp                ; Copy a message into it...
    mov     esi, U(hello_heap_msg)
    mov     ecx, hello_heap_msg_len
    rep     movsb
    mov     byte [ebp + 8191], 0xAA ; ...and touch the far end of page two

    mov     eax, SYS_PRINT          ; ...then print it from the heap
    mov     ebx, ebp
    mov     ecx, hello_heap_msg_len
    int     SYSCALL

    mov     eax, SYS_SBRK           ; Give both pages back
    mov     ebx, -8192
    int     SYSCALL
    jmp     .ticks
.heap_failed:
    PRINT   hello_heap_bad, hello_heap_bad_len

.ticks:
    mov     esi, 5                  ; Preserved across syscalls
.tick:
    PRINT   hello_tick, hello_tick_len
    mov     eax, SYS_SLEEP
    mov     ebx, 1000
    int     SYSCALL
    dec     esi
    jnz     .tick

    PRINT   hello_bye, hello_bye_len
    mov     eax, SYS_EXIT
    xor     ebx, ebx
    int     SYSCALL
    jmp     $                       ; Not reached

hello_msg:          db "hello: running in ring 3", 10
hello_msg_len       equ $ - hello_msg
hello_check_ok:     db "hello: kernel pointer refused (EFAULT), good", 10
hello_check_ok_len  equ $ - hello_check_ok
hello_check_bad:    db "hello: kernel pointer was NOT refused!", 10
hello_check_bad_len equ $ - hello_check_bad
hello_heap_msg:     db "hello: this line was printed from heap memory (sbrk)", 10
hello_heap_msg_len  equ $ - hello_heap_msg
hello_heap_bad:     db "hello: sbrk failed!", 10
hello_heap_bad_len  equ $ - hello_heap_bad
hello_tick:         db "hello: tick", 10
hello_tick_len      equ $ - hello_tick
hello_bye:          db "hello: done, exiting", 10
hello_bye_len       equ $ - hello_bye

user_hello_end:

%undef U

; ---------------------------------------------------------------------------
; crash
; ---------------------------------------------------------------------------

align 16

global user_crash_start
global user_crash_end

%define U(label) (USER_BASE + ((label) - user_crash_start))

user_crash_start:
    mov     eax, SYS_SLEEP          ; Let hello print first
    mov     ebx, 2500
    int     SYSCALL

    PRINT   crash_msg, crash_msg_len

    ; Kernel pages are supervisor-only: #PF from ring 3 -> process killed
    mov     dword [0x00100000], 0xDEADBEEF

    PRINT   crash_survived, crash_survived_len
    mov     eax, SYS_EXIT
    mov     ebx, 1
    int     SYSCALL
    jmp     $

crash_msg:          db "crash: writing to kernel memory...", 10
crash_msg_len       equ $ - crash_msg
crash_survived:     db "crash: the write went through! (isolation is broken)", 10
crash_survived_len  equ $ - crash_survived

user_crash_end:

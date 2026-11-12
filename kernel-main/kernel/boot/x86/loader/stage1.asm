; =============================================================================
; stage1.asm - Master Boot Record (LBA 0)
;
; The BIOS loads this sector to 0000:7C00 and jumps to it with the boot drive
; number in DL. All it does is pull stage 2 off the disk (LBA 1 onward) to
; 0000:8000 using the INT 13h extensions and jump there, passing DL along.
;
; Built with:  nasm -f bin -DSTAGE2_SECTORS=<n> stage1.asm -o stage1.bin
; The partition table (0x1BE-0x1FD) is left zeroed; build-disk.sh fills it.
; =============================================================================

[bits 16]
[org 0x7C00]

%ifndef STAGE2_SECTORS
  %error "STAGE2_SECTORS must be defined (-DSTAGE2_SECTORS=n)"
%endif
%if STAGE2_SECTORS < 1 || STAGE2_SECTORS > 62
  %error "stage 2 must be 1..62 sectors (it is loaded to 0x8000 and must stay below 0xFC00)"
%endif

STAGE2_ADDR     equ 0x8000
STAGE2_LBA      equ 1

start:
    ; Some BIOSes enter at 07C0:0000 instead of 0000:7C00. Normalize CS.
    jmp     0x0000:.normalized
.normalized:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti
    cld

    mov     [boot_drive], dl

    ; INT 13h extensions present? (needed for LBA reads)
    mov     ah, 0x41
    mov     bx, 0x55AA
    mov     dl, [boot_drive]
    int     0x13
    jc      .no_ext
    cmp     bx, 0xAA55
    jne     .no_ext
    test    cl, 1                   ; Bit 0: DAP-based access (AH=42h) supported
    jz      .no_ext

    ; Read stage 2, retrying a few times (real drives occasionally need it)
    mov     byte [retries], 3
.read:
    mov     si, dap
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    jnc     .loaded

    xor     ah, ah                  ; Reset the drive and try again
    mov     dl, [boot_drive]
    int     0x13
    dec     byte [retries]
    jnz     .read

    mov     si, msg_disk
    jmp     fail

.loaded:
    mov     dl, [boot_drive]
    jmp     0x0000:STAGE2_ADDR

.no_ext:
    mov     si, msg_ext

fail:
    lodsb
    test    al, al
    jz      .halt
    mov     ah, 0x0E
    mov     bx, 0x0007
    int     0x10
    jmp     fail
.halt:
    cli
    hlt
    jmp     .halt

align 4
dap:
    db      0x10                    ; Size of this packet
    db      0
    dw      STAGE2_SECTORS          ; Sector count
    dw      STAGE2_ADDR             ; Buffer offset
    dw      0x0000                  ; Buffer segment
    dq      STAGE2_LBA              ; Starting LBA

boot_drive  db 0
retries     db 0

msg_ext     db "stage1: BIOS lacks INT 13h extensions", 0
msg_disk    db "stage1: error reading stage 2", 0

    times 0x1B8 - ($ - $$) db 0
    dd      0                       ; Disk signature (build-disk.sh may set it)
    dw      0
    times 64 db 0                   ; Partition table (filled by build-disk.sh)
    dw      0xAA55

; SPDX-License-Identifier: MIT
; Copyright (c) mochimochi-man / Uh
;
; hddrom.asm - an IDE hard disk option ROM for a PC-9801 running a BIOS dumped from a real machine.
;
; A dumped BIOS has no hard disk code of its own: on the machine that lives in the interface board's ROM. The BIOS
; looks for option ROMs at D000h-DF00h in 4KB steps (word AA55h at offset 9) and far-calls them:
;   +0Ch  initialise (BX = the ROM's byte in the 0:04D0h table, DS = 0)
;   +12h, +0Fh, +15h  later passes, only for ROMs whose table byte has bit 6 set; +15h is the boot device pass,
;         called with AL = 1, 2, ... in turn, before the BIOS tries its own devices for the same number.
; This ROM claims the IDE drives the emulator attached (ports 640h-74Eh), takes INT 1Bh calls for DA/UA 8xh / 0xh,
; and boots from unit 0 on the boot pass from AL = 2 on (AL = 1 is left to the BIOS's own floppy boot).
; The vector is taken over in the later passes: at +0Ch the BIOS has not set INT 1Bh yet.
; The ROM lives in a 4KB block the emulator makes writable, so its variables sit at the end of the same block.
; The disk code follows rom_src/pc98_bios_native/pc98n.asm (the project's own BIOS).

        cpu     186
        org     0

IDE_DATA        equ 0x640
IDE_BANK        equ 0x432
IDE_SECCNT      equ 0x644
IDE_SECNUM      equ 0x646
IDE_CYLLO       equ 0x648
IDE_CYLHI       equ 0x64a
IDE_DRVHD       equ 0x64c
IDE_STATUS      equ 0x64e
IDE_DEVCTL      equ 0x74c

W_DISK_EQUIP    equ 0x055c          ; byte +1: hard disk units present (bit 0 = unit 0)
W_DISK_BOOT     equ 0x0584          ; DA/UA the IPL was read from
IPL_SEG         equ 0x1fc0

; caller frame after PUSHA, PUSH DS, PUSH ES (same order as pc98n.asm's INTFRAME)
F_ES    equ 0
F_DS    equ 2
F_BP    equ 8
F_BX    equ 12
F_DX    equ 14
F_CX    equ 16
F_AX    equ 18
F_FL    equ 24

; ---------------------------------------------------------------- header
        times 9 db 0xcb             ; RETF: nobody calls these
        dw      0xaa55
        db      0x00
        jmp     near init           ; +0Ch
        jmp     near pass_hook      ; +0Fh
        jmp     near pass_hook      ; +12h
        jmp     near boot           ; +15h

; The later passes: the BIOS has set its own vectors by now, so INT 1Bh is taken over here (not in +0Ch, where the
; vector is still empty).  Done once: a vector that already points here is left alone.
pass_hook:
        call    hook1b
        retf

hook1b:
        push    ax
        push    ds
        push    es
        push    cs
        pop     ds
        test    byte [V_UNITS], 3
        jz      .out
        xor     ax, ax
        mov     es, ax
        cmp     word [es:0x1b * 4], int1b
        jne     .take
        mov     ax, cs
        cmp     [es:0x1b * 4 + 2], ax
        je      .out
.take:  cli
        mov     ax, [es:0x1b * 4]
        mov     [V_OLD1B], ax
        mov     ax, [es:0x1b * 4 + 2]
        mov     [V_OLD1B + 2], ax
        mov     word [es:0x1b * 4], int1b
        mov     [es:0x1b * 4 + 2], cs
        sti
.out:   pop     es
        pop     ds
        pop     ax
        ret

; ---------------------------------------------------------------- variables (end of the block)
VARS            equ 0x0f00
V_OLD1B         equ VARS + 0x00     ; dword: the INT 1Bh vector before us
V_UNITS         equ VARS + 0x04     ; byte : bit n = IDE unit n present
V_DAUA          equ VARS + 0x05
V_CMD           equ VARS + 0x06
V_DIR           equ VARS + 0x07
V_LEN           equ VARS + 0x08
V_CX            equ VARS + 0x0a
V_DX            equ VARS + 0x0c
V_H             equ VARS + 0x0e
V_R             equ VARS + 0x0f
V_SEG           equ VARS + 0x10
V_OFF           equ VARS + 0x12
V_LBAL          equ VARS + 0x14
V_LBAH          equ VARS + 0x16
V_TMP           equ VARS + 0x18
V_SECCNT        equ VARS + 0x1a
V_LOGP          equ VARS + 0x1c     ; byte : next slot of the call log
V_LOG           equ VARS + 0x40     ; 16 x 8 bytes: AX, CX, DX, status of the last INT 1Bh calls (diagnostics)

; ---------------------------------------------------------------- +0Ch: initialise
init:
        push    ax
        push    cx
        push    dx
        push    si
        push    ds
        push    es
        mov     si, bx                  ; our byte in the 0:04D0h table (DS = 0 here)
        xor     ax, ax
        mov     es, ax                  ; ES = 0: the BIOS work area
        push    cs
        pop     ds
        mov     byte [V_UNITS], 0
        mov     byte [V_DAUA], 0x80
        call    ide_present
        jc      .u1
        or      byte [V_UNITS], 1
.u1:    mov     byte [V_DAUA], 0x81
        call    ide_present
        jc      .done
        or      byte [V_UNITS], 2
.done:  mov     al, [V_UNITS]
        or      al, al
        jz      .out                    ; no drive: stay out of the way
        or      [es:W_DISK_EQUIP + 1], al
        or      byte [es:si], 0x40      ; ask for the later passes (INT 1Bh hook, boot)
.out:   pop     es
        pop     ds
        pop     si
        pop     dx
        pop     cx
        pop     ax
        retf

; ---------------------------------------------------------------- +15h: boot from unit 0
boot:
        call    hook1b
        cmp     al, 2
        jb      .no                     ; AL = 1: the BIOS's floppy boot comes first
        push    cs
        pop     ds
        test    byte [V_UNITS], 1
        jz      .no
        mov     byte [V_DAUA], 0x80
        mov     byte [V_CMD], 0x06
        mov     word [V_LEN], 1024
        mov     word [V_CX], 0
        mov     word [V_DX], 0
        mov     byte [V_H], 0
        mov     byte [V_R], 0
        mov     word [V_SEG], IPL_SEG
        mov     word [V_OFF], 0
        mov     byte [V_DIR], 0
        call    ide_transfer
        or      ah, ah
        jnz     .no
        xor     ax, ax
        mov     es, ax
        mov     byte [es:W_DISK_BOOT], 0x80     ; the IPL reads this to find itself
        mov     ds, ax                  ; the IPL starts with AX = DS = ES = 0 (as from the project's own BIOS)
        jmp     IPL_SEG:0
.no:    retf

; ---------------------------------------------------------------- INT 1Bh
int1b:
        push    ax
        and     al, 0xf0
        cmp     al, 0x80
        je      .mine
        or      al, al
        je      .mine
        pop     ax
        jmp     far [cs:V_OLD1B]
.mine:  pop     ax
        pusha
        push    ds
        push    es
        mov     bp, sp
        push    cs
        pop     ds
        mov     al, [bp + F_AX]
        mov     [V_DAUA], al
        mov     al, [bp + F_AX + 1]
        mov     [V_CMD], al
        mov     ax, [bp + F_BX]
        mov     [V_LEN], ax
        mov     ax, [bp + F_CX]
        mov     [V_CX], ax
        mov     ax, [bp + F_DX]
        mov     [V_DX], ax
        mov     al, [bp + F_DX + 1]
        mov     [V_H], al
        mov     al, [bp + F_DX]
        mov     [V_R], al
        mov     ax, [bp + F_ES]
        mov     [V_SEG], ax
        mov     ax, [bp + F_BP]
        mov     [V_OFF], ax
        ; a unit this ROM does not drive
        mov     al, [V_DAUA]
        and     al, 1
        inc     al                      ; 1 or 2
        test    [V_UNITS], al
        jnz     .go
        mov     ah, 0x60                ; not ready
        jmp     short .status
.go:    call    ide_operate
        cmp     byte [V_CMD], 0x84      ; extended sense: geometry in BX / CX / DX
        jne     .status
        cmp     ah, 0x20
        jae     .status
        push    ax
        call    ide_identify_geometry
        jc      .nogeo
        mov     word [bp + F_BX], 512
        mov     ax, [V_LBAL]
        mov     [bp + F_CX], ax
        mov     ah, [V_LBAH]
        mov     al, [V_TMP]
        mov     [bp + F_DX], ax
.nogeo: pop     ax
.status:
        push    bx
        mov     bl, [V_LOGP]
        and     bx, 0x0f
        shl     bx, 3
        mov     cx, [bp + F_AX]
        mov     [V_LOG + bx], cx
        mov     cx, [bp + F_CX]
        mov     [V_LOG + bx + 2], cx
        mov     cx, [bp + F_DX]
        mov     [V_LOG + bx + 4], cx
        mov     [V_LOG + bx + 6], ah
        inc     byte [V_LOGP]
        pop     bx
        mov     [bp + F_AX + 1], ah
        and     word [bp + F_FL], 0xfffe
        cmp     ah, 0x20
        jb      .ok
        or      word [bp + F_FL], 0x0001
.ok:    pop     es
        pop     ds
        popa
        iret

; ---------------------------------------------------------------- the drive
; Returns AH = status.
ide_operate:
        mov     al, [V_CMD]
        and     al, 0x0f
        cmp     al, 0x03
        je      .ok                     ; INITIALIZE
        cmp     al, 0x04
        je      .sense
        cmp     al, 0x07
        je      .ok                     ; RETRACT
        cmp     al, 0x0e
        je      .ok
        cmp     al, 0x05
        je      .write
        cmp     al, 0x06
        je      .read
        cmp     al, 0x01
        je      .read                   ; VERIFY
        cmp     al, 0x02
        je      .read
        mov     ah, 0x40
        ret
.ok:    xor     ah, ah
        ret
.sense: call    ide_present
        jc      .absent
        mov     ah, 0x0f
        ret
.read:  mov     byte [V_DIR], 0
        jmp     ide_transfer
.write: mov     byte [V_DIR], 1
        jmp     ide_transfer
.absent:
        mov     ah, 0x60
        ret

; Select unit (V_DAUA bit 0) on the primary controller; CF set if it is not there.
ide_present:
        push    ax
        push    dx
        mov     dx, IDE_BANK
        xor     al, al
        out     dx, al
        mov     dx, IDE_DRVHD
        mov     al, [V_DAUA]
        and     al, 1
        shl     al, 4
        or      al, 0xe0
        out     dx, al
        call    ide_wait
        jc      .fail
        mov     dx, IDE_STATUS
        in      al, dx
        or      al, al
        jz      .fail
        cmp     al, 0xff
        je      .fail
        test    al, 0x40                ; DRDY
        jz      .fail
        pop     dx
        pop     ax
        clc
        ret
.fail:  pop     dx
        pop     ax
        stc
        ret

; IDENTIFY: cylinders -> V_LBAL, heads -> V_LBAH, sectors per track -> V_TMP.  CF set on failure.
ide_identify_geometry:
        push    ax
        push    bx
        push    cx
        push    dx
        call    ide_present
        jc      .fail
        mov     dx, IDE_DEVCTL
        mov     al, 0x0a                ; polled, no interrupt
        out     dx, al
        mov     dx, IDE_STATUS
        mov     al, 0xec                ; IDENTIFY DEVICE
        out     dx, al
        call    ide_drq
        jc      .fail
        mov     dx, IDE_DATA
        xor     bx, bx
        mov     cx, 256
.word:  in      ax, dx
        cmp     bx, 1
        jne     .h
        mov     [V_LBAL], ax
.h:     cmp     bx, 3
        jne     .s
        mov     [V_LBAH], ax
.s:     cmp     bx, 6
        jne     .n
        mov     [V_TMP], ax
.n:     inc     bx
        loop    .word
        call    ide_wait
        jc      .fail
        cmp     word [V_LBAL], 0
        je      .fail
        cmp     word [V_LBAH], 0
        je      .fail
        cmp     word [V_LBAH], 16
        ja      .fail
        cmp     word [V_TMP], 0
        je      .fail
        cmp     word [V_TMP], 255
        ja      .fail
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.fail:  pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

ide_wait:
        push    ax
        push    cx
        push    dx
        mov     dx, IDE_STATUS
        xor     cx, cx
.w:     in      al, dx
        test    al, 0x80
        jz      .ok
        loop    .w
        pop     dx
        pop     cx
        pop     ax
        stc
        ret
.ok:    pop     dx
        pop     cx
        pop     ax
        clc
        ret

ide_drq:
        push    ax
        push    cx
        push    dx
        mov     dx, IDE_STATUS
        xor     cx, cx
.w:     in      al, dx
        test    al, 0x01                ; ERR
        jnz     .fail
        test    al, 0x80
        jnz     .next
        test    al, 0x08                ; DRQ
        jnz     .ok
.next:  loop    .w
.fail:  pop     dx
        pop     cx
        pop     ax
        stc
        ret
.ok:    pop     dx
        pop     cx
        pop     ax
        clc
        ret

; Read or write V_LEN bytes (whole sectors) at the address of the request.  Returns AH = status.
ide_transfer:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es
        call    ide_present
        jc      .fail
        test    byte [V_DAUA], 0x80
        jz      .linear
        ; CHS (zero-based sector) with the drive's own translation
        call    ide_identify_geometry
        jc      .fail
        mov     ax, [V_CX]
        mov     bx, [V_LBAH]
        mul     bx
        or      dx, dx
        jnz     .fail
        mov     bl, [V_H]
        xor     bh, bh
        add     ax, bx
        mov     bx, [V_TMP]
        mul     bx
        mov     bl, [V_R]
        xor     bh, bh
        add     ax, bx
        adc     dx, 0
        jmp     short .lba
.linear:
        mov     ax, [V_CX]
        mov     dx, [V_DX]
.lba:   test    dh, 0xf0
        jnz     .fail
        mov     [V_LBAL], ax
        mov     [V_LBAH], dx
        mov     ax, [V_LEN]
        or      ax, ax
        jz      .fail
        test    ax, 0x01ff
        jnz     .fail
        mov     bx, [V_OFF]
        add     bx, ax
        jnc     .fits
        or      bx, bx
        jnz     .fail
.fits:  shr     ax, 9
        cmp     ax, 255
        ja      .fail
        mov     [V_SECCNT], ax

        mov     dx, IDE_DEVCTL
        mov     al, 0x0a
        out     dx, al
        mov     dx, IDE_SECCNT
        mov     al, [V_SECCNT]
        out     dx, al
        mov     dx, IDE_SECNUM
        mov     al, [V_LBAL]
        out     dx, al
        mov     dx, IDE_CYLLO
        mov     al, [V_LBAL + 1]
        out     dx, al
        mov     dx, IDE_CYLHI
        mov     al, [V_LBAH]
        out     dx, al
        mov     dx, IDE_DRVHD
        mov     al, [V_LBAH + 1]
        and     al, 0x0f
        or      al, 0xe0                ; LBA
        mov     ah, [V_DAUA]
        and     ah, 1
        shl     ah, 4
        or      al, ah
        out     dx, al
        mov     dx, IDE_STATUS
        mov     al, 0x20                ; READ SECTORS
        cmp     byte [V_DIR], 0
        je      .cmd
        mov     al, 0x30                ; WRITE SECTORS
.cmd:   out     dx, al

        mov     es, [V_SEG]
        mov     di, [V_OFF]
        mov     cx, [V_SECCNT]
.sector:
        push    cx
        call    ide_drq
        jc      .failpop
        mov     cx, 256
        mov     dx, IDE_DATA
        cmp     byte [V_DIR], 0
        jne     .wr
        rep     insw
        jmp     short .next
.wr:    push    ds
        mov     si, di
        push    es
        pop     ds
        rep     outsw
        pop     ds
        add     di, 512
.next:  pop     cx
        loop    .sector
        call    ide_wait
        jc      .fail
        mov     dx, IDE_STATUS
        in      al, dx
        test    al, 0x01
        jnz     .fail
        xor     ah, ah
        jmp     short .out
.failpop:
        pop     cx
.fail:  mov     ah, 0xd0
.out:   pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

        times VARS - ($ - $$) db 0xff
        times 0x1000 - ($ - $$) db 0

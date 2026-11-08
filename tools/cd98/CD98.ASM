; SPDX-License-Identifier: MIT
; Copyright (c) mochimochi-man / Uh
;
; CD98.SYS - ATAPI CD-ROM driver for the PC-98 IDE interface, for MSCDEX.
;
;   DEVICE=CD98.SYS /D:name        (name: up to 8 characters, the same one given to MSCDEX /D:)
;
; Looks for an ATAPI CD-ROM drive on the second IDE channel (bank 1, master then slave) and then as the
; slave of the first. PIO only, polled, interrupts from the drive masked. Data sectors (cooked, 2048 bytes)
; only; the audio commands are accepted and do nothing.
;
; nasm -f bin -o CD98.SYS CD98.ASM

        cpu     286
        org     0

IDE_DATA        equ     0640h
IDE_ERR         equ     0642h
IDE_FEAT        equ     0642h
IDE_CNT         equ     0644h
IDE_BCL         equ     0648h
IDE_BCH         equ     064Ah
IDE_DEV         equ     064Ch
IDE_CMD         equ     064Eh
IDE_CTRL        equ     074Ch
IDE_BANK        equ     0432h

; ---------------------------------------------------------------------------------------------- header
header:
        dd      -1
        dw      0C800h                  ; character device, IOCTL, open/close/removable
        dw      strategy
        dw      interrupt
devname db      'CD98$   '
        dw      0
        db      0                       ; drive letter, filled in by MSCDEX
        db      1                       ; units

reqptr  dd      0
bank    db      1
devsel  db      0A0h
oldbank db      0
changed db      1
nodisc  db      0
skey    db      0
left    dw      0                       ; words the caller's buffer still takes
nsec    dw      0
pkt     times 12 db 0
buf     times 816 db 0                  ; replies (TOC: 4 + 100 x 8)

; ---------------------------------------------------------------------------------------------- entry
strategy:
        mov     [cs:reqptr], bx
        mov     [cs:reqptr+2], es
        retf

interrupt:
        pushf
        pusha
        push    ds
        push    es
        push    cs
        pop     ds
        cld
        les     bx, [reqptr]
        mov     word [es:bx+3], 0
        mov     al, [es:bx+2]
        cmp     al, 0
        je      .init
        cmp     al, 3
        je      .ioctl_in
        cmp     al, 12
        je      .done
        cmp     al, 13
        je      .done
        cmp     al, 14
        je      .done
        cmp     al, 128
        je      .read
        cmp     al, 130
        je      .done
        cmp     al, 131
        je      .done
        cmp     al, 132
        je      .done
        cmp     al, 133
        je      .done
        cmp     al, 136
        je      .done
        mov     al, 3                   ; unknown command
        jmp     .error
.init:
        call    init
        jmp     .done
.ioctl_in:
        call    ioctl_in
        jc      .error
        jmp     .done
.read:
        call    read_long
        jc      .error
.done:
        les     bx, [reqptr]
        or      word [es:bx+3], 0100h
        jmp     .out
.error:
        les     bx, [reqptr]
        mov     ah, 81h                 ; error + done
        mov     [es:bx+3], ax
.out:
        pop     es
        pop     ds
        popa
        popf
        retf

; ---------------------------------------------------------------------------------------------- IDE
select:
        mov     dx, IDE_BANK
        in      al, dx
        and     al, 7Fh
        mov     [oldbank], al
        mov     al, [bank]
        out     dx, al
        mov     dx, IDE_DEV
        mov     al, [devsel]
        out     dx, al
        mov     dx, IDE_CTRL
        mov     al, 0Ah                 ; interrupts off
        out     dx, al
        ret

unselect:
        push    ax
        push    dx
        mov     dx, IDE_BANK
        mov     al, [oldbank]
        out     dx, al
        pop     dx
        pop     ax
        ret

; Wait for BSY clear. CF = timeout. AL = status.
wait_ready:
        push    cx
        push    dx
        push    bx
        mov     dx, IDE_CTRL
        mov     bx, 40
.outer: xor     cx, cx
.inner: in      al, dx
        test    al, 80h
        jz      .ok
        loop    .inner
        dec     bx
        jnz     .outer
        stc
        jmp     .end
.ok:    clc
.end:   pop     bx
        pop     dx
        pop     cx
        ret

; Send pkt and take the reply into ES:DI (at most [left] words; the rest is drained).
; CF = error, [skey] = sense key (0 when the drive did not answer).
packet:
        mov     byte [skey], 0
        call    select
        call    wait_ready
        jc      .fail
        mov     dx, IDE_FEAT
        xor     al, al
        out     dx, al
        mov     dx, IDE_BCL
        out     dx, al
        mov     dx, IDE_BCH
        mov     al, 80h                 ; up to 32 KB per DRQ
        out     dx, al
        mov     dx, IDE_CMD
        mov     al, 0A0h
        out     dx, al
        call    wait_ready
        jc      .fail
        test    al, 08h
        jz      .status
        mov     si, pkt
        mov     cx, 6
        mov     dx, IDE_DATA
.pk:    lodsw
        out     dx, ax
        loop    .pk
.data:  call    wait_ready
        jc      .fail
        mov     dx, IDE_CMD
        in      al, dx
        test    al, 08h
        jz      .status
        mov     dx, IDE_BCL
        in      al, dx
        mov     cl, al
        mov     dx, IDE_BCH
        in      al, dx
        mov     ch, al
        inc     cx
        shr     cx, 1                   ; words
        mov     dx, IDE_DATA
.w:     in      ax, dx
        cmp     word [left], 0
        je      .skip
        stosw
        dec     word [left]
.skip:  loop    .w
        mov     ax, di                  ; keep ES:DI away from the end of the segment
        shr     ax, 4
        mov     cx, es
        add     cx, ax
        mov     es, cx
        and     di, 0Fh
        jmp     .data
.status:
        test    al, 01h
        jnz     .err
        call    unselect
        clc
        ret
.err:   mov     dx, IDE_ERR
        in      al, dx
        shr     al, 4
        mov     [skey], al
.fail:  call    unselect
        stc
        ret

clear_pkt:
        push    ax
        push    cx
        push    di
        push    es
        push    cs
        pop     es
        mov     di, pkt
        xor     ax, ax
        mov     cx, 6
        rep     stosw
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

; Run pkt into cs:buf (CX = bytes wanted), sensing and retrying after a unit attention.
; CF = error, AL = DOS error code.
cmd_buf:
        push    cx
        mov     byte [nodisc], 0
        mov     bp, 3
.again: push    cs
        pop     es
        mov     di, buf
        pop     cx
        push    cx
        shr     cx, 1
        inc     cx
        mov     [left], cx
        call    packet
        jnc     .ok
        call    sense_error
        jnc     .again                  ; unit attention: once more
        pop     cx
        stc
        ret
.ok:    pop     cx
        clc
        ret

; After a failed packet: CF clear = retry (unit attention, tries left), else AL = DOS error.
sense_error:
        mov     al, [skey]
        cmp     al, 6
        jne     .other
        mov     byte [changed], 1
        dec     bp
        jz      .changed
        clc
        ret
.changed:
        mov     al, 0Fh                 ; invalid disk change
        stc
        ret
.other: cmp     al, 2
        jne     .gen
        mov     byte [nodisc], 1
        mov     al, 2                   ; not ready
        stc
        ret
.gen:   mov     al, 0Ch                 ; general failure
        cmp     byte [skey], 0
        je      .g2
        mov     al, 0Bh                 ; read fault
.g2:    stc
        ret

; ---------------------------------------------------------------------------------------------- READ LONG
read_long:
        les     bx, [reqptr]
        cmp     byte [es:bx+18h], 0     ; cooked only
        jne     .unk
        mov     ax, [es:bx+14h]
        mov     dx, [es:bx+16h]
        cmp     byte [es:bx+0Dh], 1
        jne     .hsg
        call    msf_to_lba
.hsg:   mov     cx, [es:bx+12h]
        les     di, [es:bx+0Eh]
.loop:  jcxz    .ok
        push    cx
        cmp     cx, 16
        jbe     .n
        mov     cx, 16
.n:     mov     [nsec], cx
        push    cx
        push    ax
        push    dx
        call    clear_pkt
        mov     byte [pkt], 28h
        mov     [pkt+2], dh
        mov     [pkt+3], dl
        mov     [pkt+4], ah
        mov     [pkt+5], al
        mov     [pkt+8], cl
        mov     bp, 3
.retry: push    es
        push    di
        mov     ax, [nsec]
        shl     ax, 10                  ; 1024 words a sector
        mov     [left], ax
        call    packet
        jnc     .got
        pop     di
        pop     es
        call    sense_error
        jnc     .retry
        pop     dx
        pop     dx
        pop     cx
        pop     cx
        stc
        ret
.got:   add     sp, 4                   ; ES:DI now past the data
        pop     dx
        pop     ax
        pop     cx
        add     ax, cx
        adc     dx, 0
        mov     si, cx
        pop     cx
        sub     cx, si
        jmp     .loop
.ok:    mov     byte [changed], 0
        clc
        ret
.unk:   mov     al, 3
        stc
        ret

; DX:AX = Red Book (frame, second, minute, 0) -> DX:AX = LBA
msf_to_lba:
        push    bx
        push    cx
        mov     bl, al                  ; frame
        mov     bh, ah                  ; second
        mov     al, dl                  ; minute
        xor     ah, ah
        mov     cx, 60
        mul     cx
        mov     cl, bh
        xor     ch, ch
        add     ax, cx                  ; seconds
        mov     cx, 75
        mul     cx                      ; DX:AX frames
        mov     cl, bl
        xor     ch, ch
        add     ax, cx
        adc     dx, 0
        sub     ax, 150
        sbb     dx, 0
        pop     cx
        pop     bx
        ret

; DX:AX = LBA -> DX:AX = Red Book
lba_to_msf:
        push    bx
        push    cx
        add     ax, 150
        adc     dx, 0
        mov     cx, 75
        div     cx                      ; AX = seconds, DX = frame
        mov     bl, dl
        xor     dx, dx
        mov     cx, 60
        div     cx                      ; AX = minutes, DX = seconds
        mov     ah, dl
        mov     dl, al
        mov     al, bl
        mov     dh, 0
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------------------------------------- IOCTL input
ioctl_in:
        les     bx, [reqptr]
        les     di, [es:bx+0Eh]
        mov     al, [es:di]
        cmp     al, 0
        je      .devhdr
        cmp     al, 1
        je      .head
        cmp     al, 4
        je      .audch
        cmp     al, 6
        je      .status
        cmp     al, 7
        je      .secsize
        cmp     al, 8
        je      .volsize
        cmp     al, 9
        je      .media
        cmp     al, 10
        je      .diskinfo
        cmp     al, 11
        je      .trackinfo
        cmp     al, 12
        je      .qchan
        cmp     al, 15
        je      .audstat
        mov     al, 3
        stc
        ret
.devhdr:
        mov     word [es:di+1], 0
        mov     [es:di+3], cs
        clc
        ret
.head:  xor     ax, ax
        mov     [es:di+2], ax
        mov     [es:di+4], ax
        clc
        ret
.audch: mov     word [es:di+1], 0FF00h
        mov     word [es:di+3], 0FF01h
        mov     word [es:di+5], 0002h
        mov     word [es:di+7], 0003h
        clc
        ret
.status:
        push    es
        push    di
        call    test_unit
        pop     di
        pop     es
        mov     ax, 0202h               ; door unlocked, HSG and Red Book
        cmp     byte [nodisc], 0
        je      .st1
        or      ax, 0800h               ; no disc
.st1:   mov     [es:di+1], ax
        mov     word [es:di+3], 0
        clc
        ret
.secsize:
        mov     word [es:di+2], 2048
        clc
        ret
.volsize:
        push    es
        push    di
        call    clear_pkt
        mov     byte [pkt], 25h
        mov     cx, 8
        call    cmd_buf
        pop     di
        pop     es
        jc      .err
        mov     ah, [buf+2]
        mov     al, [buf+3]
        mov     dh, [buf]
        mov     dl, [buf+1]
        add     ax, 1
        adc     dx, 0
        mov     [es:di+1], ax
        mov     [es:di+3], dx
        clc
        ret
.media: push    es
        push    di
        call    test_unit
        pop     di
        pop     es
        mov     al, 1                   ; not changed
        cmp     byte [nodisc], 0
        je      .m1
        xor     al, al                  ; no disc: don't know
        jmp     .m2
.m1:    cmp     byte [changed], 0
        je      .m2
        mov     al, 0FFh
        mov     byte [changed], 0
.m2:    mov     [es:di+1], al
        clc
        ret
.diskinfo:
        push    es
        push    di
        call    read_toc
        pop     di
        pop     es
        jc      .err
        mov     al, [buf+2]
        mov     [es:di+1], al
        mov     al, [buf+3]
        mov     [es:di+2], al
        mov     al, 0AAh                ; lead-out
        call    toc_find
        jc      .err0
        mov     [es:di+3], ax
        mov     [es:di+5], dx
        clc
        ret
.trackinfo:
        push    es
        push    di
        call    read_toc
        pop     di
        pop     es
        jc      .err
        mov     al, [es:di+1]
        call    toc_find
        jc      .err0
        mov     [es:di+2], ax
        mov     [es:di+4], dx
        mov     al, [si+1]
        mov     ah, al
        shl     al, 4
        shr     ah, 4
        or      al, ah
        mov     [es:di+6], al
        clc
        ret
.qchan: push    di
        inc     di
        mov     cx, 10
        xor     al, al
        rep     stosb
        pop     di
        mov     byte [es:di+1], 41h
        mov     byte [es:di+2], 1
        mov     byte [es:di+3], 1
        clc
        ret
.audstat:
        push    di
        inc     di
        mov     cx, 10
        xor     al, al
        rep     stosb
        pop     di
        clc
        ret
.err0:  mov     al, 0Ch
.err:   stc
        ret

; TEST UNIT READY (taking a unit attention as a change). Sets [nodisc].
test_unit:
        call    clear_pkt
        mov     cx, 2
        call    cmd_buf
        ret

; READ TOC (MSF) into buf
read_toc:
        call    clear_pkt
        mov     byte [pkt], 43h
        mov     byte [pkt+1], 02h       ; MSF
        mov     byte [pkt+7], 03h       ; 800 bytes
        mov     byte [pkt+8], 20h
        mov     cx, 800
        call    cmd_buf
        ret

; Find track AL in the TOC in buf. DX:AX = start (Red Book), SI = the entry. CF = not there.
toc_find:
        mov     si, buf+4
        mov     cl, [buf]
        mov     ch, [buf+1]
        xchg    cl, ch
        sub     cx, 2
        shr     cx, 3
        jcxz    .no
.f:     cmp     [si+2], al
        je      .yes
        add     si, 8
        loop    .f
.no:    stc
        ret
.yes:   mov     al, [si+7]              ; frame
        mov     ah, [si+6]              ; second
        mov     dl, [si+5]              ; minute
        xor     dh, dh
        clc
        ret

resident_end:

; ---------------------------------------------------------------------------------------------- INIT
init:
        les     bx, [reqptr]
        mov     word [es:bx+0Eh], 0     ; nothing stays unless a drive is found
        mov     [es:bx+10h], cs
        push    es
        push    bx
        lds     si, [es:bx+12h]
        call    parse_args
        push    cs
        pop     ds
        mov     dx, msg_hello
        mov     ah, 09h
        int     21h
        call    probe
        pop     bx
        pop     es
        jc      .none
        mov     dx, msg_found
        mov     ah, 09h
        int     21h
        mov     dx, model
        mov     ah, 09h
        int     21h
        mov     word [es:bx+0Eh], resident_end
        mov     [es:bx+10h], cs
        ret
.none:  mov     dx, msg_none
        mov     ah, 09h
        int     21h
        mov     word [es:bx+3], 810Ch   ; general failure: nothing installed
        ret

; DS:SI = the rest of the DEVICE= line. /D:name sets the device name.
parse_args:
.next:  lodsb
        cmp     al, 0Dh
        je      .end
        cmp     al, 0Ah
        je      .end
        cmp     al, 0
        je      .end
        cmp     al, '/'
        jne     .next
        lodsb
        and     al, 0DFh
        cmp     al, 'D'
        jne     .next
        lodsb
        cmp     al, ':'
        jne     .next
        mov     di, devname
        mov     cx, 8
.c:     lodsb
        cmp     al, ' '
        jbe     .pad
        cmp     al, '/'
        je      .pad
        cmp     al, 'a'
        jb      .s
        cmp     al, 'z'
        ja      .s
        and     al, 0DFh
.s:     mov     [cs:di], al
        inc     di
        loop    .c
        jmp     .end
.pad:   mov     byte [cs:di], ' '
        inc     di
        loop    .pad
.end:   ret

; Look for the drive: bank 1 master, bank 1 slave, bank 0 slave. CF = none.
probe:
        mov     si, cand
.p:     mov     al, [si]
        cmp     al, 0FFh
        je      .none
        mov     [bank], al
        mov     al, [si+1]
        mov     [devsel], al
        push    si
        call    identify
        pop     si
        jnc     .found
        add     si, 2
        jmp     .p
.found: clc
        ret
.none:  stc
        ret

cand    db      1, 0A0h, 1, 0B0h, 0, 0B0h, 0FFh

; IDENTIFY PACKET DEVICE: a CD-ROM there? The model name goes to model.
identify:
        call    select
        mov     dx, IDE_CTRL
        in      al, dx
        cmp     al, 0FFh                ; nothing on the bus
        je      .no
        call    wait_ready
        jc      .no
        mov     dx, IDE_CMD
        mov     al, 0A1h
        out     dx, al
        call    wait_ready
        jc      .no
        test    al, 01h
        jnz     .no
        test    al, 08h
        jz      .no
        push    cs
        pop     es
        mov     di, buf
        mov     cx, 256
        mov     dx, IDE_DATA
.r:     in      ax, dx
        stosw
        loop    .r
        call    unselect
        mov     ax, [buf]
        and     ax, 0DF00h
        cmp     ax, 8500h               ; ATAPI, CD-ROM
        jne     .no2
        mov     si, buf+54              ; words 27..46, high byte first
        mov     di, model
        mov     cx, 20
.m:     lodsw
        xchg    al, ah
        stosw
        loop    .m
        mov     si, model+39            ; trim
.t:     cmp     byte [si], ' '
        jne     .te
        cmp     si, model
        je      .te
        dec     si
        jmp     .t
.te:    mov     word [si+1], 0A0Dh
        mov     byte [si+3], '$'
        clc
        ret
.no:    call    unselect
.no2:   stc
        ret

msg_hello db    'CD98 ATAPI CD-ROM driver 1.0', 0Dh, 0Ah, '$'
msg_found db    '  Drive: $'
msg_none  db    '  No ATAPI CD-ROM drive found.', 0Dh, 0Ah, '$'
model     times 44 db '$'

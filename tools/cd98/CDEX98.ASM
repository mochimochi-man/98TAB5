; SPDX-License-Identifier: MIT
; Copyright (c) mochimochi-man / Uh
;
; CDEX98.COM - CD-ROM extensions: makes the disc in a CD-ROM device driver (CD98.SYS) a read-only DOS drive.
;
;   CDEX98 /D:name [/L:x]        name: the device driver's name (as its /D:), x: the drive letter (default Q)
;
; A network redirector (INT 2Fh AH=11h) over ISO 9660, plus the common part of the MSCDEX interface
; (INT 2Fh AX=1500h, 1501h, 1505h, 1508h, 150Bh, 150Ch, 150Dh, 1510h). File names are the ISO 9660 names
; cut to 8.3. Directories up to 64 KB.
;
; nasm -f bin -o CDEX98.COM CDEX98.ASM

        cpu     286
        org     100h

; SDA offsets (DOS 4 and later, FreeDOS)
SDA_DTA         equ     0Ch
SDA_FN1         equ     9Eh
SDA_SDB         equ     19Eh
SDA_DIRENT      equ     1B3h
SDA_SATTR       equ     24Dh
SDA_EXTACT      equ     2DDh
SDA_EXTMODE     equ     2E1h

; saved registers on our stack (pusha, then ES, DS)
R_DS    equ     0
R_ES    equ     2
R_DI    equ     4
R_SI    equ     6
R_BX    equ     12
R_DX    equ     14
R_CX    equ     16
R_AX    equ     18

start:  jmp     init

; ============================================================================================== resident
old2f   dd      0
strat   dd      0
intr    dd      0
devhdr  dd      0
sda     dd      0
cdsp    dd      0
drive   db      16                      ; 0-based
ready   db      0
rootlba dd      0
rootlen dd      0
volsize dd      0
vlabel  times 11 db ' '
savess  dw      0
savesp  dw      0
retcf   db      0
cachel  dd      -1
; directory iterator
it_lba  dd      0
it_len  dd      0
it_off  dd      0
; the entry it found
e_lba   dd      0
e_size  dd      0
e_attr  db      0
e_time  dw      0
e_date  dw      0
e_name  times 11 db ' '
pattern times 11 db '?'
comp    times 11 db ' '
sattr   db      0
req     times 32 db 0
ctl     times 8 db 0
        align   2
cache   times 2048 db 0
        times   640 db 0
stacktop:

; ---------------------------------------------------------------------------------------------- INT 2Fh
int2f:  cmp     ah, 11h
        je      redir
        cmp     ah, 15h
        je      mscdex
chain:  jmp     far [cs:old2f]

redir:  cmp     al, 0
        jne     .own
        mov     al, 0FFh
        iret
.own:   call    is_mine
        jc      chain
        mov     [cs:savess], ss
        mov     [cs:savesp], sp
        push    cs
        pop     ss
        mov     sp, stacktop
        pusha
        push    es
        push    ds
        mov     bp, sp
        push    cs
        pop     ds
        cld
        mov     byte [retcf], 0
        call    dispatch
        pop     ds
        pop     es
        popa
        mov     ss, [cs:savess]
        mov     sp, [cs:savesp]
        cmp     byte [cs:retcf], 0
        je      .ok
        stc
        retf    2
.ok:    clc
        retf    2

; Ours? CF clear = yes. Keeps every register.
is_mine:
        push    ax
        push    bx
        push    ds
        push    si
        cmp     al, 06h
        je      .sft
        cmp     al, 07h
        je      .sft
        cmp     al, 08h
        je      .sft
        cmp     al, 09h
        je      .sft
        cmp     al, 0Ah
        je      .sft
        cmp     al, 0Bh
        je      .sft
        cmp     al, 21h
        je      .sft
        cmp     al, 1Ch
        je      .sdb
        cmp     al, 0Ch
        je      .cds
        push    cs
        pop     ds
        mov     si, pathfn
.pf:    mov     ah, [si]
        inc     si
        or      ah, ah
        jz      .no
        cmp     ah, al
        jne     .pf
        lds     si, [cs:sda]            ; path functions: FN1 = "x:\..."
        mov     al, [si+SDA_FN1]
        and     al, 0DFh
        sub     al, 'A'
        cmp     al, [cs:drive]
        jne     .no
        cmp     byte [si+SDA_FN1+1], ':'
        jne     .no
        jmp     .yes
.sft:   mov     ax, [es:di+5]
        and     ax, 803Fh
        mov     bl, [cs:drive]
        xor     bh, bh
        or      bx, 8000h
        cmp     ax, bx
        jne     .no
        jmp     .yes
.sdb:   mov     al, [es:di]
        test    al, 80h
        jz      .no
        and     al, 1Fh
        cmp     al, [cs:drive]
        jne     .no
        jmp     .yes
.cds:   mov     ax, es
        cmp     ax, [cs:cdsp+2]
        jne     .no
        cmp     di, [cs:cdsp]
        jne     .no
.yes:   clc
        jmp     .end
.no:    stc
.end:   pop     si
        pop     ds
        pop     bx
        pop     ax
        ret

pathfn  db      01h, 03h, 05h, 0Eh, 0Fh, 11h, 13h, 16h, 17h, 18h, 1Bh, 2Eh, 0

dispatch:
        mov     al, [bp+R_AX]
        cmp     al, 05h
        je      f_chdir
        cmp     al, 06h
        je      f_close
        cmp     al, 08h
        je      f_read
        cmp     al, 0Ch
        je      f_space
        cmp     al, 0Fh
        je      f_getattr
        cmp     al, 16h
        je      f_open
        cmp     al, 2Eh
        je      f_extopen
        cmp     al, 1Bh
        je      f_findfirst
        cmp     al, 1Ch
        je      f_findnext
        cmp     al, 21h
        je      f_seekend
        cmp     al, 07h
        je      ok
        cmp     al, 0Ah
        je      ok
        cmp     al, 0Bh
        je      ok
        mov     ax, 5                   ; anything else would write: access denied
err:    mov     [bp+R_AX], ax
        mov     byte [retcf], 1
        ret
ok:     ret

; ---------------------------------------------------------------------------------------------- functions
f_chdir:
        call    resolve_fn1
        jc      .nf
        test    byte [e_attr], 10h
        jz      .nf
        les     di, [cdsp]              ; the new current directory
        push    ds
        lds     si, [sda]
        add     si, SDA_FN1
        mov     cx, 67
        rep     movsb
        pop     ds
        ret
.nf:    mov     ax, 3
        jmp     err

f_close:
        mov     es, [bp+R_ES]
        mov     di, [bp+R_DI]
        cmp     word [es:di], 0
        je      ok
        dec     word [es:di]
        ret

f_space:
        mov     word [bp+R_AX], 1
        mov     ax, [volsize]
        cmp     word [volsize+2], 0
        je      .s
        mov     ax, 0FFFFh
.s:     mov     [bp+R_BX], ax
        mov     word [bp+R_CX], 2048
        mov     word [bp+R_DX], 0
        ret

f_getattr:
        call    resolve_fn1
        jc      nf2
        xor     ah, ah
        mov     al, [e_attr]
        mov     [bp+R_AX], ax
        mov     ax, [e_size+2]
        mov     [bp+R_BX], ax
        mov     ax, [e_size]
        mov     [bp+R_DI], ax
        mov     ax, [e_time]
        mov     [bp+R_CX], ax
        mov     ax, [e_date]
        mov     [bp+R_DX], ax
        ret
nf2:    mov     ax, 2
        jmp     err

f_extopen:
        mov     ax, [bp+R_AX]
        push    ds
        lds     si, [sda]
        mov     ax, [si+SDA_EXTACT]
        mov     bx, [si+SDA_EXTMODE]
        pop     ds
        and     al, 0Fh
        cmp     al, 1                   ; open an existing file only
        jne     .deny
        test    bl, 7                   ; read access only
        jnz     .deny
        call    f_open
        cmp     byte [retcf], 0
        jne     .r
        mov     word [bp+R_CX], 1       ; opened
.r:     ret
.deny:  mov     ax, 5
        jmp     err

f_open:
        mov     es, [bp+R_ES]
        mov     di, [bp+R_DI]
        test    byte [es:di+2], 7       ; asked to write
        jnz     .deny
        call    resolve_fn1
        jc      nf2
        test    byte [e_attr], 10h
        jnz     .deny
        mov     es, [bp+R_ES]
        mov     di, [bp+R_DI]
        mov     al, [e_attr]
        mov     [es:di+4], al
        mov     al, [drive]
        xor     ah, ah
        or      ax, 8040h               ; remote, not written
        mov     [es:di+5], ax
        xor     ax, ax
        mov     [es:di+7], ax
        mov     [es:di+9], ax
        mov     ax, [e_lba]
        mov     [es:di+0Bh], ax
        mov     ax, [e_lba+2]
        mov     [es:di+19h], ax
        mov     ax, [e_time]
        mov     [es:di+0Dh], ax
        mov     ax, [e_date]
        mov     [es:di+0Fh], ax
        mov     ax, [e_size]
        mov     [es:di+11h], ax
        mov     ax, [e_size+2]
        mov     [es:di+13h], ax
        xor     ax, ax
        mov     [es:di+15h], ax
        mov     [es:di+17h], ax
        mov     [es:di+1Bh], ax
        mov     [es:di+1Dh], ax
        mov     [es:di+1Fh], al
        push    di
        add     di, 20h
        mov     si, e_name
        mov     cx, 11
        rep     movsb
        pop     di
        ret
.deny:  mov     ax, 5
        jmp     err

f_seekend:
        mov     es, [bp+R_ES]
        mov     di, [bp+R_DI]
        mov     ax, [es:di+11h]
        mov     dx, [es:di+13h]
        add     ax, [bp+R_DX]
        adc     dx, [bp+R_CX]
        mov     [es:di+15h], ax
        mov     [es:di+17h], dx
        mov     [bp+R_AX], ax
        mov     [bp+R_DX], dx
        ret

; READ: CX bytes from the file at its position into the DTA. CX = bytes read.
f_read:
        mov     es, [bp+R_ES]
        mov     di, [bp+R_DI]
        mov     ax, [es:di+11h]         ; left in the file
        mov     dx, [es:di+13h]
        sub     ax, [es:di+15h]
        sbb     dx, [es:di+17h]
        jnc     .pos
        xor     ax, ax
        xor     dx, dx
.pos:   mov     cx, [bp+R_CX]
        or      dx, dx
        jnz     .n
        cmp     ax, cx
        jae     .n
        mov     cx, ax
.n:     mov     [bp+R_CX], cx           ; the count to report
        mov     [rd_left], cx
        mov     ax, [es:di+0Bh]
        mov     [rd_base], ax
        mov     ax, [es:di+19h]
        mov     [rd_base+2], ax
        mov     ax, [es:di+15h]
        mov     [rd_pos], ax
        mov     ax, [es:di+17h]
        mov     [rd_pos+2], ax
        push    ds
        lds     si, [sda]
        les     di, [si+SDA_DTA]
        pop     ds
.loop:  cmp     word [rd_left], 0
        je      .done
        mov     ax, di                  ; keep ES:DI normal
        shr     ax, 4
        mov     dx, es
        add     dx, ax
        mov     es, dx
        and     di, 0Fh
        mov     ax, [rd_pos]            ; sector = base + pos / 2048, offset = pos % 2048
        mov     dx, [rd_pos+2]
        mov     bx, ax
        and     bx, 07FFh
        mov     cx, 11
.sh:    shr     dx, 1
        rcr     ax, 1
        loop    .sh
        add     ax, [rd_base]
        adc     dx, [rd_base+2]
        or      bx, bx
        jnz     .part
        cmp     word [rd_left], 2048
        jb      .part
        mov     cx, [rd_left]           ; whole sectors straight into the DTA
        shr     cx, 11
        call    drv_read
        jc      .ioerr
        mov     ax, [rd_left]
        and     ax, 0F800h
        jmp     .adv
.part:  push    es
        push    di
        call    rd_cache
        pop     di
        pop     es
        jc      .ioerr
        mov     si, cache
        add     si, bx
        mov     cx, 2048
        sub     cx, bx
        cmp     cx, [rd_left]
        jbe     .c
        mov     cx, [rd_left]
.c:     mov     ax, cx
        rep     movsb
        jmp     .adv2
.adv:   add     di, ax
.adv2:  sub     [rd_left], ax
        add     [rd_pos], ax
        adc     word [rd_pos+2], 0
        jmp     .loop
.done:  mov     es, [bp+R_ES]
        mov     di, [bp+R_DI]
        mov     ax, [rd_pos]
        mov     [es:di+15h], ax
        mov     ax, [rd_pos+2]
        mov     [es:di+17h], ax
        ret
.ioerr: mov     ax, 1Eh                 ; read fault
        jmp     err

rd_left dw      0
rd_base dd      0
rd_pos  dd      0

f_findfirst:
        call    check_media
        jc      .nr
        push    ds
        lds     si, [sda]
        mov     al, [si+SDA_SATTR]
        pop     ds
        mov     [sattr], al
        cmp     al, 08h                 ; the volume label alone
        je      .label
        call    split_fn1               ; directory -> it_*, last name -> pattern
        jc      .np
        mov     word [it_off], 0
        mov     word [it_off+2], 0
        jmp     find_go
.label: mov     si, vlabel
        mov     di, e_name
        push    ds
        pop     es
        mov     cx, 11
        rep     movsb
        mov     byte [e_attr], 08h
        xor     ax, ax
        mov     [e_time], ax
        mov     [e_date], ax
        mov     [e_size], ax
        mov     [e_size+2], ax
        mov     word [it_off], 0FFFFh   ; nothing after it
        mov     word [it_lba], 0
        mov     word [it_lba+2], 0
        call    put_found
        ret
.nr:    mov     ax, 15h
        jmp     err
.np:    mov     ax, 3
        jmp     err

f_findnext:
        mov     es, [bp+R_ES]
        mov     di, [bp+R_DI]
        lea     si, [di+1]
        push    di
        mov     di, pattern
        push    es
        push    ds
        push    es
        pop     ds
        pop     es
        mov     cx, 11
        rep     movsb
        push    es
        pop     ds
        pop     es
        pop     di
        mov     al, [es:di+12]
        mov     [sattr], al
        mov     ax, [es:di+13]
        mov     [it_off], ax
        mov     word [it_off+2], 0
        cmp     ax, 0FFFFh
        je      nomore
        mov     ax, [es:di+15]
        mov     [it_lba], ax
        mov     ax, [es:di+17]
        mov     [it_lba+2], ax
        mov     ax, [es:di+19]
        mov     [it_len], ax
        mov     word [it_len+2], 0
find_go:
.next:  call    dir_next
        jc      nomore
        mov     si, e_name
        call    match
        jne     .next
        mov     al, [e_attr]            ; directories only when asked for
        and     al, 16h
        not     byte [sattr]
        and     al, [sattr]
        not     byte [sattr]
        jnz     .next
        call    put_found
        ret
nomore: mov     ax, 12h
        jmp     err

; Write the search state (SDB at SDA+19E and, for FINDNEXT, at ES:DI) and the entry (SDA+1B3).
put_found:
        les     di, [sda]
        add     di, SDA_SDB
        call    put_sdb
        cmp     byte [bp+R_AX], 1Ch
        jne     .d
        mov     es, [bp+R_ES]
        mov     di, [bp+R_DI]
        call    put_sdb
.d:     les     di, [sda]
        add     di, SDA_DIRENT
        mov     si, e_name
        mov     cx, 11
        rep     movsb
        mov     al, [e_attr]
        stosb
        xor     ax, ax
        mov     cx, 5
        rep     stosw
        mov     ax, [e_time]
        stosw
        mov     ax, [e_date]
        stosw
        xor     ax, ax
        stosw
        mov     ax, [e_size]
        stosw
        mov     ax, [e_size+2]
        stosw
        ret

put_sdb:
        mov     al, [drive]
        or      al, 80h
        stosb
        mov     si, pattern
        mov     cx, 11
        rep     movsb
        mov     al, [sattr]
        stosb
        mov     ax, [it_off]
        stosw
        mov     ax, [it_lba]
        stosw
        mov     ax, [it_lba+2]
        stosw
        mov     ax, [it_len]
        stosw
        ret

; e_name (SI) against pattern: ZF = match
match:  push    di
        mov     di, pattern
        mov     cx, 11
.m:     mov     al, [di]
        cmp     al, '?'
        je      .n
        cmp     al, [si]
        jne     .x
.n:     inc     si
        inc     di
        loop    .m
        xor     al, al                  ; ZF
.x:     pop     di
        ret

; ---------------------------------------------------------------------------------------------- paths
; Walk FN1 to an entry (e_*); the root gives attr 10h. CF = not found / not ready.
resolve_fn1:
        call    check_media
        jc      .no
        call    fn1_local
        call    root_it
        mov     byte [e_attr], 10h
        mov     ax, [rootlba]
        mov     [e_lba], ax
        mov     ax, [rootlba+2]
        mov     [e_lba+2], ax
        mov     ax, [rootlen]
        mov     [e_size], ax
        mov     ax, [rootlen+2]
        mov     [e_size+2], ax
        xor     ax, ax
        mov     [e_time], ax
        mov     [e_date], ax
.c:     cmp     byte [si], 0
        je      .yes
        test    byte [e_attr], 10h      ; only a directory has more below it
        jz      .no
        mov     ax, [e_lba]
        mov     [it_lba], ax
        mov     ax, [e_lba+2]
        mov     [it_lba+2], ax
        mov     ax, [e_size]
        mov     [it_len], ax
        mov     ax, [e_size+2]
        mov     [it_len+2], ax
        xor     ax, ax
        mov     [it_off], ax
        mov     [it_off+2], ax
        call    take_comp
        jc      .no
        push    si
.f:     call    dir_next
        jc      .nf
        mov     si, e_name
        mov     di, comp
        mov     cx, 11
        push    es
        push    ds
        pop     es
        repe    cmpsb
        pop     es
        jne     .f
        pop     si
        jmp     .c
.nf:    pop     si
.no:    stc
        ret
.yes:   clc
        ret

; Copy FN1 past "x:\" into fn (local), SI -> it.
fn1_local:
        push    ds
        lds     si, [sda]
        add     si, SDA_FN1+3
        push    cs
        pop     es
        mov     di, fn
        mov     cx, 125
.l:     lodsb
        cmp     al, 'a'
        jb      .s
        cmp     al, 'z'
        ja      .s
        and     al, 0DFh
.s:     stosb
        or      al, al
        loopnz  .l
        mov     byte [es:di], 0
        pop     ds
        mov     si, fn
        ret

fn      times 128 db 0

root_it:
        mov     ax, [rootlba]
        mov     [it_lba], ax
        mov     ax, [rootlba+2]
        mov     [it_lba+2], ax
        mov     ax, [rootlen]
        mov     [it_len], ax
        mov     ax, [rootlen+2]
        mov     [it_len+2], ax
        xor     ax, ax
        mov     [it_off], ax
        mov     [it_off+2], ax
        ret

; One path component at SI into comp (FCB form, '*' filled with '?'). SI past it (and its '\').
; CF = empty component.
take_comp:
        push    di
        push    es
        push    ds
        pop     es
        mov     di, comp
        mov     al, ' '
        mov     cx, 11
        rep     stosb
        mov     di, comp
        mov     cx, 8
        cmp     byte [si], 0
        je      .bad
        cmp     byte [si], '\'
        je      .bad
.n:     lodsb
        cmp     al, 0
        je      .end0
        cmp     al, '\'
        je      .end
        cmp     al, '.'
        je      .ext
        cmp     al, '*'
        je      .star8
        jcxz    .n
        stosb
        dec     cx
        jmp     .n
.star8: mov     al, '?'
        rep     stosb
        jmp     .n
.ext:   mov     di, comp+8
        mov     cx, 3
.e:     lodsb
        cmp     al, 0
        je      .end0
        cmp     al, '\'
        je      .end
        cmp     al, '*'
        je      .stare
        jcxz    .e
        stosb
        dec     cx
        jmp     .e
.stare: mov     al, '?'
        rep     stosb
        jmp     .e
.end0:  dec     si                      ; stay on the terminating zero
.end:   pop     es
        pop     di
        clc
        ret
.bad:   pop     es
        pop     di
        stc
        ret

; FN1 -> it_* = the directory it names, pattern = its last component. CF = no such directory.
split_fn1:
        call    fn1_local
        mov     di, si                  ; find the last '\'
        xor     bx, bx
.s:     mov     al, [di]
        or      al, al
        jz      .e
        cmp     al, '\'
        jne     .s1
        mov     bx, di
.s1:    inc     di
        jmp     .s
.e:     or      bx, bx
        jz      .root
        mov     byte [bx], 0            ; fn = the directory, BX+1 = the name
        inc     bx
        push    bx
        call    walk_fn
        pop     si
        jc      .no
        jmp     .pat
.root:  call    root_it
.pat:   cmp     byte [si], 0
        jne     .p1
        mov     di, pattern             ; "x:\" alone: everything
        mov     al, '?'
        mov     cx, 11
        push    ds
        pop     es
        rep     stosb
        clc
        ret
.p1:    call    take_comp
        mov     si, comp
        mov     di, pattern
        push    ds
        pop     es
        mov     cx, 11
        rep     movsb
        clc
        ret
.no:    stc
        ret

; fn (SI = fn) as a directory -> it_*
walk_fn:
        call    root_it
.c:     cmp     byte [si], 0
        je      .ok
        call    take_comp
        jc      .no
        push    si
.f:     call    dir_next
        jc      .nf
        mov     si, e_name
        mov     di, comp
        mov     cx, 11
        push    ds
        pop     es
        repe    cmpsb
        jne     .f
        pop     si
        test    byte [e_attr], 10h
        jz      .no
        mov     ax, [e_lba]
        mov     [it_lba], ax
        mov     ax, [e_lba+2]
        mov     [it_lba+2], ax
        mov     ax, [e_size]
        mov     [it_len], ax
        mov     ax, [e_size+2]
        mov     [it_len+2], ax
        xor     ax, ax
        mov     [it_off], ax
        mov     [it_off+2], ax
        jmp     .c
.nf:    pop     si
.no:    stc
        ret
.ok:    clc
        ret

; ---------------------------------------------------------------------------------------------- ISO 9660
; Next entry of the directory it_* into e_* (skipping "." and ".."). CF = end.
dir_next:
.again: mov     ax, [it_off]
        mov     dx, [it_off+2]
        cmp     dx, [it_len+2]
        ja      .end
        jb      .in
        cmp     ax, [it_len]
        jae     .end
.in:    mov     bx, ax
        and     bx, 07FFh               ; offset in the sector
        mov     cx, 11
.sh:    shr     dx, 1
        rcr     ax, 1
        loop    .sh
        push    ax
        push    dx
        add     ax, [it_lba]
        adc     dx, [it_lba+2]
        call    rd_cache
        pop     dx
        pop     ax
        jc      .end
        mov     si, cache
        add     si, bx
        mov     cl, [si]
        xor     ch, ch
        or      cx, cx
        jnz     .rec
        add     ax, 1                   ; the rest of the sector is padding
        adc     dx, 0
        mov     cx, 11
.up:    shl     ax, 1
        rcl     dx, 1
        loop    .up
        mov     [it_off], ax
        mov     [it_off+2], dx
        jmp     .again
.rec:   add     [it_off], cx
        adc     word [it_off+2], 0
        cmp     byte [si+32], 1         ; "." and ".."
        jne     .name
        cmp     byte [si+33], 1
        jbe     .again
.name:  mov     ax, [si+2]
        mov     [e_lba], ax
        mov     ax, [si+4]
        mov     [e_lba+2], ax
        mov     ax, [si+10]
        mov     [e_size], ax
        mov     ax, [si+12]
        mov     [e_size+2], ax
        mov     al, 01h                 ; read-only
        test    byte [si+25], 02h
        jz      .f
        mov     al, 10h
.f:     test    byte [si+25], 01h
        jz      .h
        or      al, 02h
.h:     mov     [e_attr], al
        call    iso_date
        push    si                      ; the name, cut to 8.3
        mov     di, e_name
        push    ds
        pop     es
        mov     al, ' '
        mov     cx, 11
        rep     stosb
        mov     cl, [si+32]
        xor     ch, ch
        add     si, 33
        mov     di, e_name
        mov     dx, 8
.c:     jcxz    .cd
        lodsb
        dec     cx
        cmp     al, ';'
        je      .cd
        cmp     al, '.'
        je      .dot
        or      dx, dx
        jz      .c
        cmp     al, 'a'
        jb      .st
        cmp     al, 'z'
        ja      .st
        and     al, 0DFh
.st:    stosb
        dec     dx
        jmp     .c
.dot:   mov     di, e_name+8
        mov     dx, 3
        jmp     .c
.cd:    pop     si
        clc
        ret
.end:   stc
        ret

; Recording date at SI+18 -> e_date, e_time
iso_date:
        mov     al, [si+18]             ; years since 1900
        xor     ah, ah
        sub     ax, 80
        jnc     .y
        xor     ax, ax
.y:     shl     ax, 9
        mov     dl, [si+19]
        xor     dh, dh
        shl     dx, 5
        or      ax, dx
        mov     dl, [si+20]
        or      ax, dx
        mov     [e_date], ax
        mov     al, [si+21]
        xor     ah, ah
        shl     ax, 11
        mov     dl, [si+22]
        xor     dh, dh
        shl     dx, 5
        or      ax, dx
        mov     dl, [si+23]
        shr     dl, 1
        xor     dh, dh
        or      ax, dx
        mov     [e_time], ax
        ret

; Sector DX:AX into the cache. CF = read error.
rd_cache:
        cmp     ax, [cachel]
        jne     .rd
        cmp     dx, [cachel+2]
        jne     .rd
        clc
        ret
.rd:    push    es
        push    di
        push    cx
        push    ds
        pop     es
        mov     di, cache
        mov     cx, 1
        mov     word [cachel], -1
        mov     word [cachel+2], -1
        call    drv_read
        jc      .e
        mov     [cachel], ax
        mov     [cachel+2], dx
.e:     pop     cx
        pop     di
        pop     es
        ret

; The disc: load the volume when it changed. CF = not ready.
check_media:
        push    es
        push    di
        mov     byte [ctl], 9           ; media changed?
        mov     byte [ctl+1], 0
        mov     ax, 3
        mov     cx, 2
        call    drv_ioctl
        jc      .bad
        cmp     byte [ctl+1], 1
        jne     .load
        cmp     byte [ready], 0
        jne     .ok
.load:  mov     byte [ready], 0
        mov     word [cachel], -1
        mov     word [cachel+2], -1
        mov     ax, 16                  ; primary volume descriptor
        xor     dx, dx
.pvd:   call    rd_cache
        jc      .bad
        cmp     word [cache+1], 'CD'
        jne     .bad
        cmp     byte [cache], 1
        je      .got
        cmp     byte [cache], 0FFh
        je      .bad
        add     ax, 1
        adc     dx, 0
        cmp     ax, 32
        jb      .pvd
        jmp     .bad
.got:   mov     ax, [cache+156+2]
        mov     [rootlba], ax
        mov     ax, [cache+156+4]
        mov     [rootlba+2], ax
        mov     ax, [cache+156+10]
        mov     [rootlen], ax
        mov     ax, [cache+156+12]
        mov     [rootlen+2], ax
        mov     ax, [cache+80]
        mov     [volsize], ax
        mov     ax, [cache+82]
        mov     [volsize+2], ax
        mov     si, cache+40
        mov     di, vlabel
        push    ds
        pop     es
        mov     cx, 11
        rep     movsb
        mov     byte [ready], 1
.ok:    pop     di
        pop     es
        clc
        ret
.bad:   mov     byte [ready], 0
        pop     di
        pop     es
        stc
        ret

; ---------------------------------------------------------------------------------------------- the driver
; READ LONG: CX sectors from DX:AX to ES:DI. CF = error. Keeps AX, DX.
drv_read:
        push    bx
        mov     byte [req], 1Bh
        mov     byte [req+1], 0
        mov     byte [req+2], 128
        mov     word [req+3], 0
        mov     byte [req+0Dh], 0
        mov     [req+0Eh], di
        mov     [req+10h], es
        mov     [req+12h], cx
        mov     [req+14h], ax
        mov     [req+16h], dx
        mov     word [req+18h], 0
        mov     byte [req+1Ah], 0
        call    drv_call
        pop     bx
        ret

; IOCTL (AX = 3 input / 12 output) with ctl, CX bytes. CF = error.
drv_ioctl:
        mov     byte [req], 1Ah
        mov     byte [req+1], 0
        mov     [req+2], al
        mov     word [req+3], 0
        mov     byte [req+0Dh], 0
        mov     word [req+0Eh], ctl
        mov     [req+10h], cs
        mov     [req+12h], cx
        mov     word [req+14h], 0
        mov     word [req+16h], 0
        call    drv_call
        ret

; Call the driver with req. CF = the driver set the error bit.
drv_call:
        push    ax
        push    bx
        push    es
        push    ds
        pop     es
        mov     bx, req
        call    far [strat]
        push    cs
        pop     ds
        push    cs
        pop     es
        mov     bx, req
        call    far [intr]
        push    cs
        pop     ds
        pop     es
        pop     bx
        pop     ax
        test    byte [req+4], 80h
        jnz     .e
        clc
        ret
.e:     stc
        ret

; ---------------------------------------------------------------------------------------------- MSCDEX
mscdex: cmp     al, 00h
        je      .inst
        cmp     al, 0Ch
        je      .ver
        cmp     al, 0Bh
        je      .check
        cmp     al, 0Dh
        je      .letters
        cmp     al, 01h
        je      .list
        cmp     al, 08h
        je      .read
        cmp     al, 05h
        je      .vtoc
        cmp     al, 10h
        je      .send
        jmp     chain
.inst:  mov     bx, 1
        mov     cl, [cs:drive]
        xor     ch, ch
        iret
.ver:   mov     bx, 0217h
        iret
.check: mov     bx, 0ADADh
        xor     ax, ax
        cmp     cl, [cs:drive]
        jne     .c0
        or      ch, ch
        jnz     .c0
        mov     ax, 5AD8h
.c0:    iret
.letters:
        push    ax
        mov     al, [cs:drive]
        mov     [es:bx], al
        pop     ax
        iret
.list:  push    ax
        mov     byte [es:bx], 0
        mov     ax, [cs:devhdr]
        mov     [es:bx+1], ax
        mov     ax, [cs:devhdr+2]
        mov     [es:bx+3], ax
        pop     ax
        iret
.read:  cmp     cl, [cs:drive]
        jne     .baddrv
        call    api_enter
        mov     ax, [bp+R_DI]           ; SI:DI = first sector, DX = count, ES:BX = buffer
        mov     dx, [bp+R_SI]
        mov     cx, [bp+R_DX]
        mov     es, [bp+R_ES]
        mov     di, [bp+R_BX]
        call    drv_read
        jmp     api_leave
.vtoc:  cmp     cl, [cs:drive]
        jne     .baddrv
        call    api_enter
        mov     ax, [bp+R_DX]           ; DX = descriptor number
        add     ax, 16
        xor     dx, dx
        mov     cx, 1
        mov     es, [bp+R_ES]
        mov     di, [bp+R_BX]
        call    drv_read
        jc      api_leave
        mov     al, [es:di]
        xor     ah, ah
        cmp     al, 0FFh
        jne     .v1
        mov     ax, 0FFh
.v1:    mov     [bp+R_AX], ax
        jmp     api_leave
.send:  cmp     cl, [cs:drive]
        jne     .baddrv
        push    ds
        push    es
        push    bx
        push    cs
        pop     ds
        call    far [strat]
        pop     bx
        pop     es
        push    es
        push    bx
        call    far [cs:intr]
        pop     bx
        pop     es
        pop     ds
        iret
.baddrv:
        mov     ax, 0Fh                 ; invalid drive
        stc
        retf    2

; Our stack and frame for an API call; api_leave sets AX = error (15h) on CF.
api_enter:
        pop     word [cs:apiret]
        mov     [cs:savess], ss
        mov     [cs:savesp], sp
        push    cs
        pop     ss
        mov     sp, stacktop
        pusha
        push    es
        push    ds
        mov     bp, sp
        push    cs
        pop     ds
        cld
        jmp     [cs:apiret]
api_leave:
        jnc     .ok
        mov     word [bp+R_AX], 15h
        mov     byte [retcf], 1
        jmp     .x
.ok:    mov     byte [retcf], 0
.x:     pop     ds
        pop     es
        popa
        mov     ss, [cs:savess]
        mov     sp, [cs:savesp]
        cmp     byte [cs:retcf], 0
        je      .c
        stc
        retf    2
.c:     clc
        retf    2

apiret  dw      0

resident_end:

; ============================================================================================== install
init:   push    cs
        pop     ds
        mov     dx, msg_hello
        call    print
        mov     si, 81h                 ; command line
        call    parse
        ; the device driver, by name, along the device chain from NUL
        mov     ah, 52h
        int     21h
        mov     [lol], bx
        mov     [lol+2], es
        lea     di, [bx+22h]
.dev:   push    di
        lea     di, [di+0Ah]
        mov     si, dname
        mov     cx, 8
        repe    cmpsb
        pop     di
        je      .found
        les     di, [es:di]
        cmp     di, 0FFFFh
        jne     .dev
        mov     dx, msg_nodev
        jmp     fail
.found: mov     [devhdr], di
        mov     [devhdr+2], es
        mov     ax, [es:di+6]
        mov     [strat], ax
        mov     [strat+2], es
        mov     ax, [es:di+8]
        mov     [intr], ax
        mov     [intr+2], es
        ; the drive's CDS
        les     bx, [lol]
        mov     al, [drive]
        cmp     al, [es:bx+21h]         ; LASTDRIVE
        jb      .cdsok
        mov     dx, msg_last
        jmp     fail
.cdsok: les     bx, [es:bx+16h]
        mov     ah, 58h
        mul     ah
        add     bx, ax
        mov     [cdsp], bx
        mov     [cdsp+2], es
        test    word [es:bx+43h], 0C000h
        jz      .free
        mov     dx, msg_used
        jmp     fail
.free:  mov     al, [drive]             ; "x:\"
        add     al, 'A'
        mov     [es:bx], al
        mov     word [es:bx+1], '\' * 256 + ':'
        mov     byte [es:bx+3], 0
        mov     word [es:bx+43h], 0C000h        ; network + physical
        mov     word [es:bx+45h], 0
        mov     word [es:bx+47h], 0
        mov     word [es:bx+49h], 0FFFFh
        mov     word [es:bx+4Bh], 0FFFFh
        mov     word [es:bx+4Fh], 2
        les     di, [devhdr]            ; tell the driver its letter
        mov     al, [drive]
        inc     al
        mov     [es:di+14h], al
        ; the SDA
        push    ds
        mov     ax, 5D06h
        int     21h
        mov     ax, ds
        pop     ds
        mov     [sda], si
        mov     [sda+2], ax
        ; INT 2Fh
        mov     ax, 352Fh
        int     21h
        mov     [old2f], bx
        mov     [old2f+2], es
        mov     dx, int2f
        mov     ax, 252Fh
        int     21h
        mov     al, [drive]
        add     al, 'A'
        mov     [msg_drv+8], al
        mov     dx, msg_drv
        call    print
        mov     es, [2Ch]               ; the environment is not needed
        mov     ah, 49h
        int     21h
        mov     dx, (resident_end - start + 100h + 15) / 16
        mov     ax, 3100h
        int     21h

fail:   call    print
        mov     ax, 4C01h
        int     21h

print:  mov     ah, 09h
        int     21h
        ret

; /D:name /L:x
parse:
.n:     lodsb
        cmp     al, 0Dh
        je      .end
        cmp     al, '/'
        jne     .n
        lodsb
        and     al, 0DFh
        cmp     al, 'D'
        je      .d
        cmp     al, 'L'
        jne     .n
        lodsb
        cmp     al, ':'
        jne     .n
        lodsb
        and     al, 0DFh
        sub     al, 'A'
        cmp     al, 25
        ja      .n
        mov     [drive], al
        jmp     .n
.d:     lodsb
        cmp     al, ':'
        jne     .n
        mov     di, dname
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
.s:     mov     [di], al
        inc     di
        loop    .c
        jmp     .n
.pad:   dec     si
.p:     mov     byte [di], ' '
        inc     di
        loop    .p
        jmp     .n
.end:   ret

lol     dd      0
dname   db      'CD98$   '
msg_hello db    'CDEX98 CD-ROM extensions 1.0', 0Dh, 0Ah, '$'
msg_drv   db    '  Drive  : is the CD-ROM.', 0Dh, 0Ah, '$'
msg_nodev db    '  No such CD-ROM device driver (/D:).', 0Dh, 0Ah, '$'
msg_last  db    '  The drive letter is past LASTDRIVE.', 0Dh, 0Ah, '$'
msg_used  db    '  That drive letter is in use.', 0Dh, 0Ah, '$'

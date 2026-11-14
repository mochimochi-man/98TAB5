; ===========================================================================
;  PC98N - a standalone PC-9801/PC-486 compatible ROM BIOS
;
;  This ROM is the BIOS. It brings the machine up by programming the actual
;  chips - 8259A, 8237A, 8253, 8251, uPD7220 x2, uPD765A, SASI, IDE and
;  WD33C93 SCSI - and services
;  INT 09h/18h/1Bh/1Ch out of its own code. It needs no emulator hook,
;  host callback, other emulator's source, ESP-IDF component or vendor firmware.
;
;  Nothing here comes from NEC's ROM. The chips are programmed from their
;  public register documentation; the PC-98 specifics - port map, work area
;  layout, the INT 1Bh calling convention, keyboard scan codes - are the
;  published interface, checked against independent implementations and a
;  running machine. No vendor ROM image was disassembled.
;
;  MEMORY MAP
;    E8000h-F7FFFh   unprogrammed except for the vendor text at F3274h
;    F8000h          entry point, all code and tables
;    F3274h          "Compatible BIOS by Codex" at the EPSON vendor-text slot
;    FFFC0h          the same identification for 32 KiB ROM loaders
;    FFFF0h          reset vector
;    FFFFFh          checksum byte, written by build.sh
;
;  Build: ./build.sh   (nasm, plus a pass to compute the checksum)
; ===========================================================================

        CPU 186                 ; V30 and 80286 upwards; nothing 386-only
        BITS 16

; ---------------------------------------------------------------- utilities
; A breath between back to back writes to the same chip.
%macro JIODLY 0
        jmp     short %%next
%%next:
%endmacro

; Interrupt handlers save everything and return values by writing the saved
; copy, so a handler can answer in any register - and in the carry flag -
; without juggling the stack.
F_ES            equ 0
F_DS            equ 2
F_DI            equ 4
F_SI            equ 6
F_BP            equ 8
F_SP            equ 10
F_BX            equ 12
F_DX            equ 14
F_CX            equ 16
F_AX            equ 18
F_IP            equ 20
F_CS            equ 22
F_FL            equ 24

%macro INTFRAME 0
        cld
        pusha
        push    ds
        push    es
        mov     bp, sp
        push    ax
        xor     ax, ax
        mov     ds, ax
        pop     ax
%endmacro

%macro INTLEAVE 0
        pop     es
        pop     ds
        popa
        iret
%endmacro

; ---------------------------------------------------------------- I/O ports
PIC0            equ 0x00        ; master 8259A  ICW1 / OCW2 / OCW3
PIC0_IMR        equ 0x02        ;               ICW2-4 / IMR
PIC1            equ 0x08        ; slave 8259A
PIC1_IMR        equ 0x0a
DMA_ADR1        equ 0x05        ; 8237A channel 1 (640KB floppy)
DMA_CNT1        equ 0x07
DMA_ADR2        equ 0x09        ; 8237A channel 2 (1MB floppy)
DMA_CNT2        equ 0x0b
DMA_SMASK       equ 0x15        ; single mask bit
DMA_MODE        equ 0x17
DMA_CLRFF       equ 0x19        ; clear the byte pointer flip-flop
DMA_MCLR        equ 0x1b
DMA_ALLMASK     equ 0x1f
DMA_BANK1       equ 0x21        ; A16-A23, channel 1
DMA_BANK2       equ 0x23        ; A16-A23, channel 2
DMA_ADR3        equ 0x0d        ; 8237A channel 3 (640KB floppy)
DMA_CNT3        equ 0x0f
DMA_BANK3       equ 0x25        ; A16-A23, channel 3
SYSPORT_A       equ 0x31        ; 8255 port A, dip switches
RTC_CTRL        equ 0x20        ; uPD4990A command/data/clock/strobe
RTC_DATA        equ 0x33        ; uPD4990A serial data in, bit 0
SYSPORT_CTRL    equ 0x37        ; 8255 control; bit 3 gates the buzzer
KBD_DATA        equ 0x41        ; 8251 keyboard interface
KBD_CMD         equ 0x43
TGDC_PARAM      equ 0x60        ; uPD7220 #1 (text) parameter / status
TGDC_CMD        equ 0x62        ;                  command / data
GDC_MODEFF1     equ 0x68        ; mode flip-flop 1
GDC_MODEFF2     equ 0x6a        ; mode flip-flop 2
GDC_MODEFF3     equ 0x6e        ; 15kHz / 24kHz
PIT_CH0         equ 0x71        ; 8253 counter 0, the 10ms system tick
PIT_CH1         equ 0x73        ;      counter 1, the buzzer
PIT_CTRL        equ 0x77
FDC_MSR         equ 0x90        ; uPD765A on the 1MB interface
FDC_MODEREG     equ 0xbe        ; media type and which register set answers
FDC2_MSR        equ 0xc8        ; uPD765A on the 640KB interface
GGDC_PARAM      equ 0xa0        ; uPD7220 #2 (graphics)
GGDC_CMD        equ 0xa2
FDC_PATIENCE    equ 0x40            ; x 64K polls: a few seconds, as a real BIOS waits
BANKSEL         equ 0x43d       ; 10h ITF bank, 12h BIOS bank
IDE_DATA        equ 0x640
IDE_BANK        equ 0x432       ; controller bank: 0 = primary pair
IDE_ERR         equ 0x642
IDE_SECCNT      equ 0x644
IDE_SECNUM      equ 0x646
IDE_CYLLO       equ 0x648
IDE_CYLHI       equ 0x64a
IDE_DRVHD       equ 0x64c
IDE_STATUS      equ 0x64e       ; read status, write command
IDE_DEVCTL      equ 0x74c
SASI_DATA       equ 0x80        ; PC-98 SASI data register
SASI_CTRL       equ 0x82        ; output control / input phase status
SCSI_IO_FIRST   equ 0x0cc0      ; PC-9801-55 family selectable bases
SCSI_IO_LAST    equ 0x0cf0
SCSI_WD_ADDR    equ 0           ; base + 0: WD33C93 address / auxiliary status
SCSI_WD_REG     equ 2           ; base + 2: selected WD33C93 register
SCSI_BOARD      equ 4           ; base + 4: board DMA control / configuration

; ------------------------------------------------------------- text screen
TVRAM_SEG       equ 0xa000      ; character codes, two bytes a cell
TATTR_SEG       equ 0xa200      ; attributes, same stride
TEXT_COLS       equ 80
TEXT_ROWS       equ 25
TEXT_CELLS      equ (TEXT_COLS * TEXT_ROWS)
ATTR_NORMAL     equ 0xe1        ; white, visible

; ------------------------------------------- BIOS work area (0400h - 05FFh)
W_EXPMMSZ       equ 0x0401
W_SYS_TYPE      equ 0x0480
W_DISK_EQUIPS   equ 0x0482
W_F2HD_MODE     equ 0x0493
W_BIOS_FLAG0    equ 0x0500
W_BIOS_FLAG1    equ 0x0501
W_KB_BUF        equ 0x0502      ; 16 words
W_KB_BUF_END    equ 0x0522
W_KB_SHIFT_TBL  equ 0x0522      ; offset of the live key code table
W_KB_BUF_HEAD   equ 0x0524
W_KB_BUF_TAIL   equ 0x0526
W_KB_COUNT      equ 0x0528
W_KB_KY_STS     equ 0x052a      ; 16 byte bitmap of the keys held down
W_SHIFT_STS     equ 0x053a
W_CRT_RASTER    equ 0x053b
W_CRT_STS_FLAG  equ 0x053c
W_CRT_CNT       equ 0x053d
W_CRT_W_VRAMADR equ 0x0548
W_CRT_W_RASTER  equ 0x054a
W_PRXCRT        equ 0x054c
W_PRXDUPD       equ 0x054d
W_PRXGLS        equ 0x054e      ; the last line / character pattern drawn
W_DISK_EQUIP    equ 0x055c
W_DISK_INTL     equ 0x055e
W_DISK_INTH     equ 0x055f
W_DISK_RESULT   equ 0x0564      ; the last seven result bytes off the FDC
W_DISK_BOOT     equ 0x0584
W_CA_TIM_CNT    equ 0x058a      ; interval timer countdown, in 10ms units
W_CRT_BIOS      equ 0x0597
W_KB_CODE_OFF   equ 0x05c6      ; far pointer to the key code tables
W_KB_CODE_SEG   equ 0x05c8
W_F2DD_MODE     equ 0x05ca
W_F2DD_POINTER  equ 0x05cc
W_F2HD_POINTER  equ 0x05f8
W_MSW           equ 0x3fe2      ; in the text VRAM segment, every 4th byte

; ------------------------------------ this BIOS's own variables, 0420-044Fh
;  Deliberately not in the documented work area: 0400-041Fh, 0455h upwards
;  (SASI parameters), 0460h (SCSI) and 04B0h (expansion ROM table) are all
;  read by software, so this BIOS keeps out of them.
BV_CURSOR       equ 0x0420      ; word  print cursor, in cells
BV_ATTR         equ 0x0422      ; byte  attribute the BIOS prints with
BV_FDBASE       equ 0x0423      ; byte  90h or C8h: the live register set
BV_FDMODE       equ 0x0424      ; byte  last value written to port BEh
BV_FDSPT        equ 0x0425      ; byte  sectors per track of the live format
BV_FDN          equ 0x0426      ; byte  sector size code of the live format
BV_DIR          equ 0x0427      ; byte  0 = read, 1 = write
BV_FDTRACK      equ 0x0428      ; 4     cylinder each unit is sitting on
BV_TICKL        equ 0x042c      ; word  free running 10ms counter
BV_TICKH        equ 0x042e
BV_TIMERMODE    equ 0x0430      ; byte  1 = interval timer armed
BV_SCAN         equ 0x0431      ; byte  scan code being translated
; The request block. INT 1Bh copies its registers in here and the drivers
; work from it, so the bootstrap can drive exactly the same code.
BV_REQ_DAUA     equ 0x0432      ; byte  device/unit address
BV_REQ_CMD      equ 0x0433      ; byte  command byte (AH)
BV_REQ_LEN      equ 0x0434      ; word  bytes to move
BV_REQ_C        equ 0x0436      ; byte  cylinder
BV_REQ_H        equ 0x0437      ; byte  head
BV_REQ_R        equ 0x0438      ; byte  sector
BV_REQ_N        equ 0x0439      ; byte  sector size code
BV_REQ_SEG      equ 0x043a      ; word  buffer segment
BV_REQ_OFF      equ 0x043c      ; word  buffer offset
BV_XFERL        equ 0x043e      ; word  linear address of the next byte
BV_XFERH        equ 0x0440
BV_SECSIZE      equ 0x0442      ; word  bytes in the sector being moved
BV_LBAL         equ 0x0444      ; word  block number for IDE
BV_LBAH         equ 0x0446
BV_SECCNT       equ 0x0448      ; word  sectors left in an IDE transfer
BV_TMP          equ 0x044a      ; word  scratch
BV_REQ_WORD_CX  equ 0x044c      ; word  original CX (HDD address)
BV_REQ_WORD_DX  equ 0x044e      ; word  original DX (HDD linear address)
BV_SCSI_BASE    equ 0x0450      ; word  detected PC-9801-55 I/O base
BV_SCSI_ID      equ 0x0452      ; byte  initiator ID read from the board
BV_SCSI_READY   equ 0x0453      ; byte  0 unknown, 1 present, FF absent
BV_SASI_READY   equ 0x0454      ; byte  last media-switch result
BV_FDGPL        equ 0x0455      ; byte  GPL for the live request
BV_SCSI_ROM     equ 0x0456      ; word  SCSI option-ROM segment, zero if none
BV_FDBUSY       equ 0x0458      ; byte  1 while INT 1Bh is driving the uPD765A
BV_FDEOT        equ 0x0459      ; byte  last sector of the run fdc_rw moves

W_SCSI_INFO     equ 0x0460      ; four bytes per target, eight targets
SCSI_CDB_BUF    equ 0x0900      ; temporary CDB/capacity buffer in low RAM

; --------------------------------------------------------------- constants
STACK_TOP       equ 0x7c00      ; the BIOS runs on SS=0
IPL_SEG1024     equ 0x1fc0      ; 1024 byte boot sectors land here
IPL_SEG512      equ 0x1fe0      ; 512 byte ones here
ROM_SEG         equ 0xf800

; ===========================================================================
        section .low start=0 vstart=0
; The real PC-486NAV BIOS starts its vendor text at physical F3274h. Keep our
; independently written identification at the same address; all other bytes in
; this otherwise unused window remain erased.
        times 0xb274 db 0xff
vendor_id:
        db 'Compatible BIOS by Codex'
        times 0x10000 - ($ - $$) db 0xff

; ===========================================================================
        section .rom start=0x10000 vstart=0
; Everything from here on lives at F800:0000.

; ---------------------------------------------------------------------------
; The reset vector lands here.
; ---------------------------------------------------------------------------
; The normal BIOS entry offset differs between PC-98 generations and ITFs.
; Keep all three offsets as real instruction boundaries so an ITF may enter at
; F800:0000, F800:0004 or F800:0008 without landing in the middle of setup.
entry:
        jmp     short entry_main
        nop
        nop
entry_0004:
        jmp     short entry_main
        nop
        nop
entry_0008:
        jmp     short entry_main

entry_main:
        cli
        cld
        ; A program that left protected mode by resetting the CPU cleared SHUT0
        ; (system port C bit 7) first and put its SS:SP at 0000:0404h, with the
        ; place to go on next on that stack: set SHUT0/SHUT1 back and go there.
        in      al, 0x35
        test    al, 0x80
        jnz     .cold
        mov     al, 0x0f                ; SHUT0 = 1
        out     0x37, al
        mov     al, 0x0b                ; SHUT1 = 1
        out     0x37, al
        xor     ax, ax
        mov     ds, ax
        mov     ss, [0x0406]
        mov     sp, [0x0404]
        retf
.cold:
        xor     ax, ax
        mov     ss, ax
        mov     sp, STACK_TOP
        mov     ds, ax
        mov     es, ax

        mov     dx, BANKSEL
        mov     al, 0x12                ; out of the ITF bank into the BIOS
        out     dx, al                  ; bank; both hold this same ROM

        call    init_pic
        call    init_dma
        call    init_pit
        call    init_kbd
        call    init_gdc
        call    init_work
        call    mem_size
        call    text_clear
        call    scsi_option_init
        call    sasi_equip_init
        call    ide_equip_init

        sti
        call    beep_pipo
        call    boot                    ; only returns when nothing booted

        mov     si, msg_nosystem
        call    puts
.stop:  sti
        hlt
        jmp     short .stop

; ---------------------------------------------------------------------------
; The 8259A pair: master vectors from 08h, slave from 10h, cascaded on IR7.
; ---------------------------------------------------------------------------
init_pic:
        mov     al, 0x11                ; ICW1: edge triggered, ICW4 follows
        out     PIC0, al
        JIODLY
        mov     al, 0x08                ; ICW2: master base vector
        out     PIC0_IMR, al
        JIODLY
        mov     al, 0x80                ; ICW3: a slave hangs off IR7
        out     PIC0_IMR, al
        JIODLY
        mov     al, 0x1d                ; ICW4: 8086, buffered master, SFNM
        out     PIC0_IMR, al
        JIODLY

        mov     al, 0x11
        out     PIC1, al
        JIODLY
        mov     al, 0x10                ; ICW2: slave base vector
        out     PIC1_IMR, al
        JIODLY
        mov     al, 0x07                ; ICW3: this slave is number 7
        out     PIC1_IMR, al
        JIODLY
        mov     al, 0x09                ; ICW4: 8086, buffered slave
        out     PIC1_IMR, al
        JIODLY

        mov     al, 0xf3                ; the slave: the two floppy interfaces
        out     PIC1_IMR, al            ; (IR2 640KB, IR3 1MB), as NEC leaves it
        JIODLY
        mov     al, 0x7c                ; timer, keyboard and the cascade
        out     PIC0_IMR, al
        ret

; ---------------------------------------------------------------------------
; The 8237A, cleared, with every channel masked until a transfer wants one.
; ---------------------------------------------------------------------------
init_dma:
        xor     al, al
        out     DMA_MCLR, al
        JIODLY
        out     DMA_CLRFF, al
        JIODLY
        mov     al, 0x0f
        out     DMA_ALLMASK, al
        ret

; ---------------------------------------------------------------------------
; Counter 0 at 10ms, the unit the timer BIOS counts in. The divisor depends
; on which clock chain the machine has, which dip switch port 31h reports.
; ---------------------------------------------------------------------------
init_pit:
        mov     al, 0x36                ; counter 0, LSB+MSB, mode 3
        out     PIT_CTRL, al
        JIODLY
        call    pit_interval
        ret

pit_interval:
        push    ax
        xor     al, al
        out     PIT_CH0, al
        JIODLY
        call    pit_is8mhz
        mov     al, 0x60                ; 24576 / 2.4576MHz = 10ms
        jnc     .put
        mov     al, 0x4e                ; 19968 / 1.9968MHz = 10ms
.put:   out     PIT_CH0, al
        pop     ax
        ret

; CF=1 when this machine runs off the 8MHz clock chain.
pit_is8mhz:
        push    ax
        in      al, SYSPORT_A
        test    al, 0x80
        pop     ax
        jnz     .no
        stc
        ret
.no:    clc
        ret

; ---------------------------------------------------------------------------
; The 8251 keyboard interface, and the buffer INT 09h fills.
; ---------------------------------------------------------------------------
init_kbd:
        mov     al, 0x3a                ; reset, high
        out     KBD_CMD, al
        JIODLY
        mov     al, 0x32                ; reset, low
        out     KBD_CMD, al
        JIODLY
        mov     al, 0x16                ; clear the error latch, receive on
        out     KBD_CMD, al
        JIODLY
        ; fall through

kbd_clearbuf:
        push    ax
        push    cx
        push    di
        push    es
        xor     ax, ax
        mov     es, ax
        mov     di, W_KB_BUF
        mov     cx, 0x10
        rep     stosw                   ; the 16 entries
        mov     di, W_KB_COUNT
        mov     cx, 0x13                ; count, key bitmap, shift state
        rep     stosb
        mov     word [W_KB_BUF_HEAD], W_KB_BUF
        mov     word [W_KB_BUF_TAIL], W_KB_BUF
        mov     word [W_KB_CODE_OFF], keytables ; the tables stay in ROM, as
        mov     word [W_KB_CODE_SEG], ROM_SEG   ; NEC's do: low RAM is DOS's
        mov     word [W_KB_SHIFT_TBL], keytables
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

; The ROM stores a scan-code-major matrix rather than a byte-for-byte copy of
; a vendor table. Transpose it into the documented eight planar tables in RAM.

; ---------------------------------------------------------------------------
; Both uPD7220s: 640x400 at 24.83kHz, 80x25 text of 16 raster lines a row.
; ---------------------------------------------------------------------------
init_gdc:
        push    ax
        push    cx
        push    si
        push    ds
        mov     ax, ROM_SEG
        mov     ds, ax                  ; the parameter blocks are in ROM

        xor     al, al                  ; 24kHz
        out     GDC_MODEFF3, al
        JIODLY

        push    dx
        mov     si, crtc_400_25         ; CRTC: 16-line rows, 25 of them
        mov     dx, 0x70                ; PL BL CL SSL SUR SDR at 70h-7Ah
        mov     cx, 6
.crtc:  lodsb
        out     dx, al
        JIODLY
        add     dx, 2
        loop    .crtc
        pop     dx

        mov     si, gdc_mode1           ; mode flip-flop 1, a bit at a time
        mov     cx, 8
.m1:    lodsb
        out     GDC_MODEFF1, al
        JIODLY
        loop    .m1

        mov     si, gdc_mode2
        mov     cx, 4
.m2:    lodsb
        out     GDC_MODEFF2, al
        JIODLY
        loop    .m2

        ; ---- text GDC (master)
        xor     al, al                  ; RESET, then the sync parameters
        out     TGDC_CMD, al
        JIODLY
        mov     si, gdc_tsync
        mov     cx, 8
        call    gdc_tparams
        mov     al, 0x6f                ; MASTER
        out     TGDC_CMD, al
        JIODLY
        mov     al, 0x47                ; PITCH
        out     TGDC_CMD, al
        JIODLY
        mov     al, TEXT_COLS
        out     TGDC_PARAM, al
        JIODLY
        mov     al, 0x4b                ; CSRFORM
        out     TGDC_CMD, al
        JIODLY
        mov     si, gdc_csrform
        mov     cx, 3
        call    gdc_tparams
        mov     al, 0x70                ; PRAM: display area 0 is the screen
        out     TGDC_CMD, al
        JIODLY
        mov     si, gdc_tpram
        mov     cx, 8
        call    gdc_tparams
        mov     al, 0x49                ; cursor to the top left
        out     TGDC_CMD, al
        JIODLY
        xor     al, al
        out     TGDC_PARAM, al
        JIODLY
        out     TGDC_PARAM, al
        JIODLY
        out     TGDC_PARAM, al
        JIODLY
        mov     al, 0x6b                ; START
        out     TGDC_CMD, al
        JIODLY
        mov     al, 0x0d                ; BCTRL ON (explicit display enable)
        out     TGDC_CMD, al
        JIODLY

        ; ---- graphics GDC (slave): set up, but left switched off
        xor     al, al
        out     GGDC_CMD, al
        JIODLY
        mov     si, gdc_gsync
        mov     cx, 8
        call    gdc_gparams
        mov     al, 0x6e                ; SLAVE
        out     GGDC_CMD, al
        JIODLY
        mov     al, 0x47                ; PITCH: 640 dots
        out     GGDC_CMD, al
        JIODLY
        mov     al, 0x28
        out     GGDC_PARAM, al
        JIODLY
        mov     al, 0x70
        out     GGDC_CMD, al
        JIODLY
        mov     si, gdc_gpram
        mov     cx, 8
        call    gdc_gparams
        mov     al, 0x0c                ; STOP
        out     GGDC_CMD, al

        pop     ds
        pop     si
        pop     cx
        pop     ax
        ret

gdc_tparams:                            ; DS:SI -> CX parameter bytes
        lodsb
        out     TGDC_PARAM, al
        JIODLY
        loop    gdc_tparams
        ret

gdc_gparams:
        lodsb
        out     GGDC_PARAM, al
        JIODLY
        loop    gdc_gparams
        ret

; ---------------------------------------------------------------------------
; Interrupt vectors, memory switches, and the work area DOS expects.
; ---------------------------------------------------------------------------
init_work:
        push    ax
        push    bx
        push    cx
        push    si
        push    di
        push    es

        ; ---- everything unclaimed points at an IRET
        xor     ax, ax
        mov     es, ax
        xor     di, di
        mov     cx, 0x20
.vec:   mov     ax, dummy_iret
        stosw
        mov     ax, ROM_SEG
        stosw
        loop    .vec

        ; ---- then the ones we do answer
        push    ds
        mov     ax, ROM_SEG
        mov     ds, ax
        mov     si, vectors
        mov     cx, (vectors_end - vectors) / 3
.vec2:  lodsb                           ; vector number
        mov     bl, al
        xor     bh, bh
        shl     bx, 1
        shl     bx, 1
        lodsw                           ; offset within this ROM
        mov     [es:bx], ax
        mov     word [es:bx+2], ROM_SEG
        loop    .vec2
        pop     ds

        ; ---- what kind of machine this is
        mov     byte [W_SYS_TYPE], 0x01         ; 80286 or better (V30: below)
        mov     byte [W_BIOS_FLAG0], 0x03       ; 1MB floppy interface fitted
        mov     al, 0x20
        call    pit_is8mhz
        jnc     .f1
        or      al, 0x80
.f1:    mov     [W_BIOS_FLAG1], al
        ; V30/8086 or 80286 and up: FLAGS bits 12-15 cannot be cleared on
        ; the former, and read back as 0 in real mode on the latter.
        pushf
        xor     ax, ax
        push    ax
        popf
        pushf
        pop     ax
        popf
        and     ax, 0xf000
        cmp     ax, 0xf000
        jne     .cpu286
        mov     byte [W_SYS_TYPE], 0x00         ; V30
        or      byte [W_BIOS_FLAG1], 0x40       ; and says so here too
.cpu286:
        mov     byte [W_PRXCRT], 0x4f
        mov     byte [W_PRXDUPD], 0x58
        mov     byte [W_CRT_RASTER], 0x0f       ; 16 raster lines a row
        mov     byte [W_CRT_STS_FLAG], 0x80
        mov     byte [W_CRT_CNT], 0x20
        mov     byte [W_EXPMMSZ], 0             ; mem_size fills it in
        mov     word [W_CRT_W_VRAMADR], 0
        mov     word [W_CRT_W_RASTER], 400 << 4

        ; ---- floppy
        mov     byte [W_F2HD_MODE], 0xff
        mov     byte [W_F2DD_MODE], 0xff
        mov     word [W_DISK_EQUIP], 0x0003     ; two 1MB drives
        mov     word [W_F2HD_POINTER], fdfmt_2hd
        mov     word [W_F2HD_POINTER+2], ROM_SEG
        mov     word [W_F2DD_POINTER], fdfmt_2dd
        mov     word [W_F2DD_POINTER+2], ROM_SEG
        mov     byte [W_DISK_INTL], 0
        mov     byte [W_DISK_INTH], 0
        mov     byte [W_DISK_EQUIPS], 0
        mov     byte [BV_FDBUSY], 0

        ; ---- our own state
        mov     word [BV_CURSOR], 0
        mov     byte [BV_ATTR], ATTR_NORMAL
        mov     byte [BV_FDBASE], FDC_MSR
        mov     byte [BV_FDMODE], 0x03
        mov     byte [BV_FDSPT], 8
        mov     byte [BV_FDN], 3
        mov     word [BV_TICKL], 0
        mov     word [BV_TICKH], 0
        mov     byte [BV_TIMERMODE], 0
        mov     word [BV_FDTRACK], 0xffff       ; positions unknown
        mov     word [BV_FDTRACK+2], 0xffff
        mov     word [BV_SCSI_BASE], 0
        mov     byte [BV_SCSI_ID], 7
        mov     byte [BV_SCSI_READY], 0
        mov     byte [BV_SASI_READY], 0
        mov     word [BV_SCSI_ROM], 0

        ; ---- memory switches, written through the text VRAM window
        mov     al, 0x0d                        ; mode FF1 bit 6: MSW writable
        out     GDC_MODEFF1, al
        JIODLY
        mov     ax, TVRAM_SEG
        mov     es, ax
        mov     di, W_MSW
        push    ds
        mov     ax, ROM_SEG
        mov     ds, ax
        mov     si, msw_default
        mov     cx, 8
.msw:   lodsb
        mov     [es:di], al
        add     di, 4
        loop    .msw
        pop     ds
        mov     al, 0x0c
        out     GDC_MODEFF1, al

        pop     es
        pop     di
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ret

; ===========================================================================
;  Text output, for the BIOS's own messages. Software uses INT 18h or writes
;  the VRAM itself.
; ===========================================================================

text_clear:
        push    ax
        mov     al, 0x20
        mov     ah, [BV_ATTR]
        call    text_fill
        mov     word [BV_CURSOR], 0
        call    text_setcursor
        pop     ax
        ret

; AL = character, AH = attribute.
text_fill:
        push    ax
        push    cx
        push    di
        push    es
        push    ax
        mov     cx, TVRAM_SEG
        mov     es, cx
        xor     di, di
        xor     ah, ah
        mov     cx, TEXT_CELLS
        rep     stosw
        pop     ax
        mov     cx, TATTR_SEG
        mov     es, cx
        xor     di, di
        mov     al, ah
        xor     ah, ah
        mov     cx, TEXT_CELLS
        rep     stosw
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

; SI -> a NUL terminated string in the ROM segment. DS must be 0.
puts:
        push    ax
        push    si
        push    ds
.next:  push    ds
        mov     ax, ROM_SEG
        mov     ds, ax
        mov     al, [si]
        pop     ds
        inc     si
        or      al, al
        jz      .done
        call    putc
        jmp     short .next
.done:  pop     ds
        pop     si
        pop     ax
        ret

; AL = character. DS must be 0.
putc:
        push    ax
        push    bx
        push    cx
        push    dx
        push    es
        cmp     al, 0x0d
        je      .cr
        cmp     al, 0x0a
        je      .lf
        mov     bx, [BV_CURSOR]
        shl     bx, 1
        mov     ah, 0
        mov     cx, TVRAM_SEG
        mov     es, cx
        mov     [es:bx], ax
        mov     cx, TATTR_SEG
        mov     es, cx
        mov     al, [BV_ATTR]
        mov     ah, 0
        mov     [es:bx], ax
        inc     word [BV_CURSOR]
        jmp     short .wrap
.cr:    mov     ax, [BV_CURSOR]
        xor     dx, dx
        mov     cx, TEXT_COLS
        div     cx
        sub     [BV_CURSOR], dx
        jmp     short .out
.lf:    add     word [BV_CURSOR], TEXT_COLS
.wrap:  cmp     word [BV_CURSOR], TEXT_CELLS
        jb      .out
        call    text_scroll
        mov     word [BV_CURSOR], TEXT_CELLS - TEXT_COLS
.out:   call    text_setcursor
        pop     es
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; Rolls both planes up one row.
text_scroll:
        push    ax
        push    cx
        push    si
        push    di
        push    ds
        push    es
        mov     ax, TVRAM_SEG
        mov     ds, ax
        mov     es, ax
        mov     si, TEXT_COLS * 2
        xor     di, di
        mov     cx, TEXT_CELLS - TEXT_COLS
        rep     movsw
        mov     ax, 0x0020
        mov     cx, TEXT_COLS
        rep     stosw
        mov     ax, TATTR_SEG
        mov     ds, ax
        mov     es, ax
        mov     si, TEXT_COLS * 2
        xor     di, di
        mov     cx, TEXT_CELLS - TEXT_COLS
        rep     movsw
        mov     ax, ATTR_NORMAL
        mov     cx, TEXT_COLS
        rep     stosw
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

text_setcursor:
        push    ax
        mov     ax, [BV_CURSOR]
        call    gdc_csrw
        pop     ax
        ret

; AX = cursor address, in words.
gdc_csrw:
        push    ax
        push    bx
        mov     bx, ax
        mov     al, 0x49
        out     TGDC_CMD, al
        JIODLY
        mov     al, bl
        out     TGDC_PARAM, al
        JIODLY
        mov     al, bh
        out     TGDC_PARAM, al
        JIODLY
        xor     al, al
        out     TGDC_PARAM, al
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; The power-on chime: counter 1 makes the tone, port 37h gates it.
; ---------------------------------------------------------------------------
beep_pipo:
        push    bx
        push    cx
        mov     bx, 0x0500
        call    beep_tone
        mov     cx, 0x1800
        call    beep_wait
        call    beep_off
        mov     cx, 0x0600
        call    beep_wait
        mov     bx, 0x0700
        call    beep_tone
        mov     cx, 0x1800
        call    beep_wait
        call    beep_off
        pop     cx
        pop     bx
        ret

; BX = counter 1 divisor.
beep_tone:
        push    ax
        mov     al, 0x76                ; counter 1, LSB+MSB, mode 3
        out     PIT_CTRL, al
        JIODLY
        mov     al, bl
        out     PIT_CH1, al
        JIODLY
        mov     al, bh
        out     PIT_CH1, al
        JIODLY
        mov     al, 0x06                ; port C bit 3 low: buzzer on
        out     SYSPORT_CTRL, al
        pop     ax
        ret

beep_off:
        push    ax
        mov     al, 0x07
        out     SYSPORT_CTRL, al
        pop     ax
        ret

beep_wait:
        push    cx
.l:     push    cx
        mov     cx, 0x30
.i:     loop    .i
        pop     cx
        loop    .l
        pop     cx
        ret

; ===========================================================================
;  Interrupt handlers
; ===========================================================================
dummy_iret:
        iret

irq_master_eoi:
        push    ax
        mov     al, 0x20
        out     PIC0, al
        pop     ax
        iret

irq_slave_eoi:
        push    ax
        mov     al, 0x20
        out     PIC1, al
        JIODLY
        out     PIC0, al
        pop     ax
        iret

; INT 11h - IRQ 9, the SASI board's end-of-command interrupt (enabled by bit 0 of the board's output
; control register). The DOS disk driver talks to the board itself and sleeps on bit 0 of 0:055Fh, which this
; handler sets, as the NEC BIOS does.
irq_sasi:
        push    ax
        push    ds
        xor     ax, ax
        mov     ds, ax
        or      byte [W_DISK_INTH], 0x01
        mov     al, 0x20
        out     PIC1, al
        JIODLY
        out     PIC0, al
        pop     ds
        pop     ax
        iret

; ---------------------------------------------------------------------------
; INT 13h - IRQ11, the 1MB floppy interface. As NEC's BIOS does: SENSE
; INTERRUPT STATUS until the controller has nothing more to say, each unit's
; result bytes into 0:0564 + unit * 8 and its bit into 0:055E. That is how a
; disk being put in or taken out reaches DOS, whose INT 13h hook reads them.
; Left alone while INT 1Bh is driving the controller: those results are its.
; ---------------------------------------------------------------------------
int13:
        push    ax
        push    cx
        push    di
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     al, 0x20
        out     PIC1, al
        JIODLY
        mov     al, 0x0b                ; slave ISR: anything else in service?
        out     PIC1, al
        JIODLY
        in      al, PIC1
        or      al, al
        jnz     .busy
        mov     al, 0x20
        out     PIC0, al
.busy:  cmp     byte [BV_FDBUSY], 0
        jne     .done
.next:  in      al, FDC_MSR
        test    al, 0x10                ; CB: a result is waiting already
        jnz     .ready
        and     al, 0xc0
        cmp     al, 0x80                ; idle and wanting a command
        jne     .done
        mov     al, 0x08                ; SENSE INTERRUPT STATUS
        out     FDC_MSR + 2, al
        JIODLY
        in      al, FDC_MSR
.ready: and     al, 0xd0
        cmp     al, 0xd0                ; RQM, DIO and CB: a byte to read
        jne     .done
        in      al, FDC_MSR + 2         ; ST0
        cmp     al, 0x80                ; invalid command: no more interrupts
        je      .done
        mov     cl, al
        and     cl, 3
        mov     di, cx
        and     di, 3
        shl     di, 3
        add     di, W_DISK_RESULT
.store: mov     [di], al
        inc     di
        in      al, FDC_MSR
        and     al, 0xd0
        cmp     al, 0xd0
        jne     .unit
        in      al, FDC_MSR + 2
        jmp     short .store
.unit:  mov     al, 1
        shl     al, cl
        or      [W_DISK_INTL], al
        jmp     short .next
.done:  pop     ds
        pop     di
        pop     cx
        pop     ax
        iret

; ---------------------------------------------------------------------------
; INT 08h - IRQ0, the 10ms tick. Counts, and when the count armed by
; INT 1Ch AH=02h runs out, runs the user's routine through INT 07h.
; ---------------------------------------------------------------------------
int08:
        INTFRAME
        add     word [BV_TICKL], 1
        adc     word [BV_TICKH], 0
        cmp     byte [BV_TIMERMODE], 0
        je      .eoi
        mov     ax, [W_CA_TIM_CNT]
        or      ax, ax
        jz      .eoi
        dec     ax
        mov     [W_CA_TIM_CNT], ax
        or      ax, ax
        jnz     .eoi
        mov     byte [BV_TIMERMODE], 0
        mov     al, 0x20                ; the tick itself is finished with
        out     PIC0, al
        int     0x07                    ; and now the user's routine
        INTLEAVE
.eoi:   mov     al, 0x20
        out     PIC0, al
        INTLEAVE

; ---------------------------------------------------------------------------
; INT 09h - IRQ1, keyboard. Tracks the modifiers, turns everything else into
; a (scan code, character) word in the ring buffer.
; ---------------------------------------------------------------------------
int09:
        INTFRAME
        in      al, KBD_DATA
        mov     [BV_SCAN], al
        mov     al, 0x20
        out     PIC0, al

        mov     al, [BV_SCAN]
        mov     bl, al
        and     bl, 0x7f
        mov     bh, bl
        shr     bh, 1
        shr     bh, 1
        shr     bh, 1                   ; BH = byte index into the bitmap
        mov     cl, bl
        and     cl, 7
        mov     ch, 1
        shl     ch, cl                  ; CH = bit within it

        test    al, 0x80
        jnz     .release

        ; ---- pressed
        push    bx
        mov     bl, bh
        xor     bh, bh
        add     bx, W_KB_KY_STS
        or      [bx], ch
        pop     bx

        mov     al, bl                  ; the make code
        cmp     al, 0x70
        je      .shift
        cmp     al, 0x7d
        je      .shift
        cmp     al, 0x71
        jb      .normal
        cmp     al, 0x75
        jae     .normal
        sub     al, 0x70                ; 71h caps, 72h kana, 73h grph, 74h ctrl
        mov     cl, al
        mov     al, 1
        shl     al, cl
        or      [W_SHIFT_STS], al
        call    kbd_shifttable
        jmp     short .done
.shift: or      byte [W_SHIFT_STS], 0x01
        call    kbd_shifttable
        jmp     short .done

.normal:
        call    kbd_translate           ; AX = key word, or FFFFh
        cmp     ax, 0xffff
        je      .done
        call    kbd_enqueue
        jmp     short .done

        ; ---- released
.release:
        push    bx
        mov     bl, bh
        xor     bh, bh
        add     bx, W_KB_KY_STS
        mov     al, ch
        not     al
        and     [bx], al
        pop     bx
        mov     al, [BV_SCAN]
        cmp     al, 0xf0
        je      .unshift
        cmp     al, 0xfd
        je      .unshift
        cmp     al, 0xf1
        jb      .done
        cmp     al, 0xf5
        jae     .done
        sub     al, 0xf0
        mov     cl, al
        mov     al, 1
        shl     al, cl
        not     al
        and     [W_SHIFT_STS], al
        call    kbd_shifttable
        jmp     short .done
.unshift:
        and     byte [W_SHIFT_STS], 0xfe
        call    kbd_shifttable
.done:
        INTLEAVE

; Points W_KB_SHIFT_TBL at the table matching the modifiers now held.
kbd_shifttable:
        push    ax
        push    bx
        push    dx
        mov     al, [W_SHIFT_STS]
        test    al, 0x10                ; ctrl wins outright
        jz      .nc
        mov     al, 7
        jmp     short .set
.nc:    test    al, 0x08                ; then graph
        jz      .ng
        mov     al, 6
        jmp     short .set
.ng:    and     al, 7                   ; shift, caps and kana combine
        cmp     al, 6
        jb      .set
        sub     al, 2
.set:   xor     ah, ah
        mov     bx, 0x60
        mul     bx
        add     ax, [W_KB_CODE_OFF]
        mov     [W_KB_SHIFT_TBL], ax
        pop     dx
        pop     bx
        pop     ax
        ret

; BV_SCAN -> AX = key word (scan code in AH, character in AL), FFFFh if the
; key produces nothing.
kbd_translate:
        push    bx
        push    si
        push    es
        mov     es, [W_KB_CODE_SEG]     ; table segment:offset as 05C8h/0522h say
        mov     bl, [BV_SCAN]
        and     bl, 0x7f
        xor     bh, bh
        mov     si, [W_KB_SHIFT_TBL]

        cmp     bl, 0x51
        ja      .high
        cmp     bl, 0x51
        je      .codeonly
        cmp     bl, 0x35
        je      .codeonly
        cmp     bl, 0x3e
        je      .codeonly
        add     si, bx
        mov     al, [es:si]
        cmp     al, 0xff
        je      .none
        mov     ah, bl                  ; scan code above, character below
        jmp     short .out

.codeonly:
        add     si, bx
        mov     al, [es:si]
        cmp     al, 0xff
        je      .none
        mov     ah, al
        xor     al, al
        jmp     short .out

.high:  cmp     bl, 0x5e                ; HOME/CLR
        jne     .fn
        mov     ax, 0xae00
        jmp     short .out
.fn:    cmp     bl, 0x62                ; f1 to f10 and the vf keys
        jb      .none
        cmp     bl, 0x70
        jae     .none
        sub     bl, 0x0c
        add     si, bx
        mov     al, [es:si]
        cmp     al, 0xff
        je      .none
        mov     ah, al
        xor     al, al
        jmp     short .out
.none:  mov     ax, 0xffff
.out:   pop     es
        pop     si
        pop     bx
        ret

; AX = key word.
kbd_enqueue:
        push    bx
        cmp     byte [W_KB_COUNT], 0x10
        jae     .full
        inc     byte [W_KB_COUNT]
        mov     bx, [W_KB_BUF_TAIL]
        mov     [bx], ax
        add     bx, 2
        cmp     bx, W_KB_BUF_END
        jb      .store
        mov     bx, W_KB_BUF
.store: mov     [W_KB_BUF_TAIL], bx
.full:  pop     bx
        ret

; AX <- the oldest key word.
kbd_dequeue:
        push    bx
        mov     bx, [W_KB_BUF_HEAD]
        mov     ax, [bx]
        add     bx, 2
        cmp     bx, W_KB_BUF_END
        jb      .store
        mov     bx, W_KB_BUF
.store: mov     [W_KB_BUF_HEAD], bx
        dec     byte [W_KB_COUNT]
        pop     bx
        ret

; ===========================================================================
; INT 18h - screen and keyboard BIOS
; ===========================================================================
int18:
        INTFRAME
        mov     al, [bp+F_AX+1]         ; AH as the caller passed it
        cmp     al, 0x50
        jae     .unsupported
        mov     bl, al
        xor     bh, bh
        shl     bx, 1
        mov     ax, [cs:int18_table+bx]
        or      ax, ax
        jz      .unsupported
        push    ax
        ret                             ; into the handler; it INTLEAVEs
.unsupported:
        INTLEAVE

i18_00:                                 ; read a key, waiting if need be
        cmp     byte [W_KB_COUNT], 0
        jne     .have
        sti
        hlt
        jmp     short i18_00
.have:  call    kbd_dequeue
        mov     [bp+F_AX], ax
        INTLEAVE

i18_01:                                 ; is a key waiting?
        cmp     byte [W_KB_COUNT], 0
        je      .empty
        mov     bx, [W_KB_BUF_HEAD]
        mov     ax, [bx]
        mov     [bp+F_AX], ax
        mov     byte [bp+F_BX+1], 1
        INTLEAVE
.empty: mov     byte [bp+F_BX+1], 0
        INTLEAVE

i18_02:                                 ; modifier state
        mov     al, [W_SHIFT_STS]
        mov     [bp+F_AX], al
        INTLEAVE

i18_03:                                 ; re-initialise the keyboard
        call    init_kbd
        INTLEAVE

i18_04:                                 ; is this group of keys down?
        mov     al, [bp+F_AX]
        and     al, 0x0f
        xor     ah, ah
        mov     bx, ax
        mov     al, [bx+W_KB_KY_STS]
        mov     [bp+F_AX+1], al
        INTLEAVE

i18_05:                                 ; read a key without waiting
        cmp     byte [W_KB_COUNT], 0
        je      .empty
        call    kbd_dequeue
        mov     [bp+F_AX], ax
        mov     byte [bp+F_BX+1], 1
        INTLEAVE
.empty: mov     byte [bp+F_BX+1], 0
        INTLEAVE

i18_0a:                                 ; CRT mode: AL bit 0 20 lines, bit 1
        mov     bl, [bp+F_AX]           ; 40 columns, bit 2 attribute bit 4 is
        and     bl, 0x0f                ; simple graphics (not a vertical
        or      bl, 0x80                ; line), bit 3 KCG code access
        mov     [W_CRT_STS_FLAG], bl    ; (bit 7: this is a 400-line CRT)
        mov     al, 0x00                ; mode F/F 1: vertical line ...
        test    bl, 0x04
        jz      .ff1
        inc     al                      ; ... or simple graphics
.ff1:   out     GDC_MODEFF1, al
        JIODLY
        mov     al, 0x04                ; 80 / 40 columns
        test    bl, 0x02
        jz      .ff2
        inc     al
.ff2:   out     GDC_MODEFF1, al
        JIODLY
        mov     al, 0x0a                ; KCG dot / code access
        test    bl, 0x08
        jz      .ff3
        inc     al
.ff3:   out     GDC_MODEFF1, al
        JIODLY
        mov     si, crt_400_25          ; raster, then CRTC PL BL CL
        test    bl, 0x01
        jz      .crt
        mov     si, crt_400_20
.crt:   mov     al, [cs:si]
        mov     [W_CRT_RASTER], al
        mov     dx, 0x70
        mov     cx, 3
.crtc:  inc     si
        mov     al, [cs:si]
        out     dx, al
        JIODLY
        add     dx, 2
        loop    .crtc
        xor     al, al                  ; SSL
        out     dx, al
        call    crt_csrform             ; AL = 0: the cursor shape, unshown
        INTLEAVE

crt_400_25:     db 0x0f, 0x00, 0x0f, 0x10
crt_400_20:     db 0x13, 0x1e, 0x11, 0x10

i18_0b:                                 ; CRT mode sense
        mov     al, [W_CRT_STS_FLAG]
        mov     [bp+F_AX], al
        INTLEAVE

i18_0c:                                 ; text on
        mov     al, 0x0d                ; BCTRL ON
        out     TGDC_CMD, al
        INTLEAVE

i18_0d:                                 ; text off
        mov     al, 0x0c
        out     TGDC_CMD, al
        INTLEAVE

i18_0e:                                 ; the upper display area
        mov     ax, [bp+F_DX]
        shr     ax, 1
        mov     [W_CRT_W_VRAMADR], ax
        mov     bx, 200 << 4
        test    byte [W_CRT_STS_FLAG], 0x80
        jz      .put
        mov     bx, 400 << 4
.put:   mov     [W_CRT_W_RASTER], bx
        call    gdc_pram1
        INTLEAVE

i18_0f:                                 ; several areas, from a table
        mov     ax, [bp+F_BX]           ; BX:CX -> (byte address, lines) pairs
        mov     es, ax
        mov     si, [bp+F_CX]
        mov     cl, [bp+F_DX+1]         ; DH = first area
        mov     ch, [bp+F_DX]           ; DL = how many
        call    gdc_pramtable
        INTLEAVE

i18_10:                                 ; cursor shape, AL = blink bits
        mov     al, [bp+F_AX]
        call    crt_csrform
        INTLEAVE

; Cursor shape for the current text mode, the cursor not shown. AL = the
; blink control (AH=10h's AL). DS = 0.
crt_csrform:
        and     byte [W_CRT_STS_FLAG], 0xbf
        shl     al, 1
        shl     al, 1
        shl     al, 1
        shl     al, 1
        shl     al, 1
        mov     [W_CRT_CNT], al
        jmp     cursor_off

i18_11:                                 ; cursor on
        call    gdc_csrform_on
        INTLEAVE

i18_12:                                 ; cursor off
        call    cursor_off
        INTLEAVE

; CSRFORM with the cursor not shown. DS = 0.
cursor_off:
        push    ax
        and     byte [W_CRT_STS_FLAG], 0xbf
        mov     al, [W_CRT_RASTER]
        call    csr_write
        pop     ax
        ret

; CSRFORM: AL = P1 (lines per row - 1, bit 7 shown); P2 = W_CRT_CNT, P3
; the bottom line for 25 or 20 rows.
csr_write:
        push    ax
        mov     ah, al
        mov     al, 0x4b
        out     TGDC_CMD, al
        JIODLY
        mov     al, ah
        out     TGDC_PARAM, al
        JIODLY
        mov     al, [W_CRT_CNT]
        out     TGDC_PARAM, al
        JIODLY
        mov     al, 0x7b
        test    byte [W_CRT_STS_FLAG], 0x01
        jz      .p3
        mov     al, 0x9b
.p3:    out     TGDC_PARAM, al
        pop     ax
        ret

i18_13:                                 ; cursor position, DX counts bytes
        mov     ax, [bp+F_DX]
        shr     ax, 1
        mov     [BV_CURSOR], ax
        call    gdc_csrw
        INTLEAVE

i18_14:                                 ; read a font pattern into BX:CX
        mov     es, [bp+F_BX]           ; (the font ROM itself, through the
        mov     di, [bp+F_CX]           ; CG window at A1h-A9h)
        mov     dx, [bp+F_DX]           ; DX = the code
        cld
        or      dh, dh
        jz      .ank8
        cmp     dh, 0x80
        je      .ank16
        mov     al, dl                  ; kanji: A1h = second JIS byte,
        out     0xa1, al
        mov     al, dh                  ; A3h = first JIS byte - 20h
        sub     al, 0x20
        out     0xa3, al
        cmp     dh, 0x29                ; 29h-2Bh: the half-width ones
        jb      .k16
        cmp     dh, 0x2b
        ja      .k16
        mov     ax, 0x0102              ; 8x16
        stosw
        mov     bl, 0x20                ; left half only
        call    cg_lines
        INTLEAVE
.k16:   mov     ax, 0x0202              ; 16x16: left and right, row by row
        stosw
        xor     cx, cx
.k16r:  mov     al, cl
        or      al, 0x20                ; left
        out     0xa5, al
        in      al, 0xa9
        stosb
        mov     al, cl                  ; right
        out     0xa5, al
        in      al, 0xa9
        stosb
        inc     cx
        cmp     cx, 16
        jb      .k16r
        INTLEAVE
.ank16: call    cg_ank
        mov     ax, 0x0102              ; 8x16
        stosw
        xor     bl, bl
        call    cg_lines
        INTLEAVE
.ank8:  call    cg_ank                  ; 8x8: the window only gives the 8x16
        mov     ax, 0x0101              ; face, so every other line of it
        stosw
        xor     cx, cx
.a8:    mov     al, cl
        out     0xa5, al
        in      al, 0xa9
        stosb
        add     cx, 2
        cmp     cx, 16
        jb      .a8
        INTLEAVE

; CG window on the ANK character DL.
cg_ank:
        xor     al, al
        out     0xa1, al
        mov     al, dl
        out     0xa3, al
        ret

; 16 lines of the character in the CG window -> ES:DI, BL = the half (20h
; left, 00h right).
cg_lines:
        xor     cx, cx
.l:     mov     al, cl
        or      al, bl
        out     0xa5, al
        in      al, 0xa9
        stosb
        inc     cx
        cmp     cx, 16
        jb      .l
        ret

i18_16:                                 ; fill the text screen
        mov     al, [bp+F_DX]           ; DL = character
        mov     ah, [bp+F_DX+1]         ; DH = attribute
        call    text_fill
        INTLEAVE

i18_17:                                 ; buzzer on
        mov     al, 0x06
        out     SYSPORT_CTRL, al
        INTLEAVE

i18_18:                                 ; buzzer off
        mov     al, 0x07
        out     SYSPORT_CTRL, al
        INTLEAVE

i18_40:                                 ; graphics on
        mov     al, 0x0d
        out     GGDC_CMD, al
        or      byte [W_PRXCRT], 0x80
        INTLEAVE

i18_41:                                 ; graphics off
        mov     al, 0x0c
        out     GGDC_CMD, al
        and     byte [W_PRXCRT], 0x7f
        INTLEAVE

; ---- the graphics BIOS ------------------------------------------------------
; AL -> the graphics GDC as a command / a parameter, once its FIFO has room.
ggdc_cmd:
        push    ax
        call    ggdc_wait
        pop     ax
        out     GGDC_CMD, al
        ret
ggdc_par:
        push    ax
        call    ggdc_wait
        pop     ax
        out     GGDC_PARAM, al
        ret
ggdc_wait:
        push    cx
        mov     cx, 0x8000
.w:     in      al, GGDC_PARAM          ; status, bit 1: FIFO full
        test    al, 0x02
        jz      .ok
        loop    .w
.ok:    pop     cx
        ret

; AL <- AL with its bits the other way round: the GDC takes patterns LSB
; first, the UCW holds them MSB first.
rev8:
        push    cx
        push    dx
        mov     dl, al
        xor     al, al
        mov     cx, 8
.r:     shr     dl, 1
        rcl     al, 1
        loop    .r
        pop     dx
        pop     cx
        ret

i18_42:                                 ; display area and colour or mono
        mov     ch, [bp+F_CX+1]         ; CH
        mov     bl, ch
        shr     bl, 6
        xor     bh, bh
        mov     bl, [cs:i18_42_modes+bx]  ; 0 lower, 1 upper, 2 all, 3 none
        mov     al, 0x70                ; SCROLL: where display area 1 starts
        call    ggdc_cmd
        xor     ax, ax
        cmp     bl, 2
        je      .sad
        test    bl, 1
        jz      .sad
        mov     ax, 200 * 40
.sad:   call    ggdc_par
        mov     al, ah
        call    ggdc_par
        xor     al, al
        call    ggdc_par
        test    byte [W_PRXDUPD], 0x04
        jz      .s3
        mov     al, 0x40
.s3:    call    ggdc_par
        mov     al, 0x08                ; 400 lines as they are
        mov     ah, 0x40
        cmp     bl, 2
        je      .form
        test    byte [W_PRXCRT], 0x40
        jz      .form
        mov     al, 0x09                ; 200 lines, each shown twice
        mov     ah, 0x41
.form:  out     GDC_MODEFF1, al
        mov     al, 0x4b                ; CSRFORM
        call    ggdc_cmd
        mov     al, ah
        call    ggdc_par
        xor     al, al
        call    ggdc_par
        call    ggdc_par
        cmp     bl, 3
        je      .colour
        mov     al, ch                  ; bit 4: the page shown
        shr     al, 4
        and     al, 1
        out     0xa4, al
.colour:
        mov     al, 0x02                ; bit 5: colour (0) or mono (1)
        test    ch, 0x20
        jz      .c
        mov     al, 0x03
.c:     out     GDC_MODEFF1, al
        INTLEAVE

i18_42_modes:
        db      3, 1, 0, 2

i18_43:                                 ; the 8-colour palette, DS:BX = UCW
        mov     es, [bp+F_DS]
        mov     bx, [bp+F_BX]
        ; GBCPC (+4..+7) holds colours 6/7, 4/5, 2/3, 0/1 as nibble pairs;
        ; the ports pair them 0/4 (AEh), 1/5 (AAh), 2/6 (ACh), 3/7 (A8h).
        mov     al, [es:bx+7]           ; colours 0, 1
        mov     ah, [es:bx+5]           ; colours 4, 5
        mov     dl, al
        and     dl, 0xf0
        mov     dh, ah
        shr     dh, 4
        or      dl, dh
        mov     cl, al                  ; keep colours 1 / 5
        mov     ch, ah
        mov     al, dl
        out     0xae, al                ; 0 / 4
        mov     al, cl
        shl     al, 4
        and     ch, 0x0f
        or      al, ch
        out     0xaa, al                ; 1 / 5
        mov     al, [es:bx+6]           ; colours 2, 3
        mov     ah, [es:bx+4]           ; colours 6, 7
        mov     dl, al
        and     dl, 0xf0
        mov     dh, ah
        shr     dh, 4
        or      dl, dh
        mov     cl, al
        mov     ch, ah
        mov     al, dl
        out     0xac, al                ; 2 / 6
        mov     al, cl
        shl     al, 4
        and     ch, 0x0f
        or      al, ch
        out     0xa8, al                ; 3 / 7
        INTLEAVE

i18_44:                                 ; border colour: nothing to set here
        INTLEAVE

i18_4a:                                 ; drawing mode
        test    byte [W_PRXCRT], 0x01
        jnz     .done
        mov     al, 0x0e                ; SYNC, keeping the display as it is
        test    byte [W_PRXCRT], 0x80
        jz      .s
        mov     al, 0x0f
.s:     call    ggdc_cmd
        mov     al, [bp+F_CX+1]         ; CH replaces the first parameter
        call    ggdc_par
        push    ds
        mov     ax, ROM_SEG
        mov     ds, ax
        mov     si, gdc_gsync + 1
        mov     cx, 7
.p:     lodsb
        call    ggdc_par
        loop    .p
        pop     ds
        and     byte [W_PRXDUPD], 0xf7
        test    byte [bp+F_CX+1], 0x10
        jnz     .done
        or      byte [W_PRXDUPD], 0x08
.done:  INTLEAVE

; The figure drawn by 47h/48h/49h is described to the GDC in 11 bytes,
; kept below the register frame while it is put together.
V_OPE           equ -12
V_DC            equ -11
V_D             equ -9
V_D2            equ -7
V_D1            equ -5
V_DM            equ -3

vect_clear:                             ; called with the frame's BP
        xor     ax, ax
        mov     [bp-12], ax
        mov     [bp-10], ax
        mov     [bp-8], ax
        mov     [bp-6], ax
        mov     [bp-4], ax
        mov     [bp-2], ax
        ret

i18_47:                                 ; line or box (47h), circle (48h)
i18_48:
        sub     sp, 12
        call    vect_clear
        mov     es, [bp+F_DS]
        mov     si, [bp+F_BX]
        mov     al, [es:si+0x28]        ; GBDTYP
        cmp     al, 1
        je      .line
        cmp     al, 2
        jbe     .box

        mov     al, [es:si+3]           ; circle or arc
        and     al, 7
        add     al, 0x20
        mov     [bp+V_OPE], al
        mov     ax, [es:si+0x0c]        ; GBLNG1
        mov     [bp+V_DC], ax
        mov     ax, [es:si+0x1c]        ; GBCIR - 1
        dec     ax
        mov     [bp+V_D], ax
        shr     ax, 1
        mov     [bp+V_D2], ax
        mov     word [bp+V_D1], 0x3fff
        cmp     byte [es:si+0x28], 4
        jne     .draw
        mov     ax, [es:si+0x1a]        ; GBMDOT
        mov     [bp+V_DM], ax
        jmp     .draw

.box:   mov     al, [es:si+3]
        and     al, 7
        add     al, 0x40
        mov     [bp+V_OPE], al
        mov     cx, [es:si+0x16]        ; |x2 - x1|
        sub     cx, [es:si+0x08]
        jns     .bx
        neg     cx
.bx:    mov     dx, [es:si+0x18]        ; |y2 - y1|
        sub     dx, [es:si+0x0a]
        jns     .by
        neg     dx
.by:    mov     bl, [es:si+3]
        and     bl, 3
        mov     ax, dx                  ; 0: D = dy, D2 = dx
        mov     di, cx
        cmp     bl, 0
        je      .bset
        mov     ax, cx                  ; 2: D = dx, D2 = dy
        mov     di, dx
        cmp     bl, 2
        je      .bset
        mov     di, cx                  ; 1 and 3: D2 = (dx + dy) / 2
        add     di, dx
        shr     di, 1
        mov     ax, cx                  ; 1: D = (dx - dy) / 2
        sub     ax, dx
        cmp     bl, 1
        je      .half
        mov     ax, dx                  ; 3: D = (dy - dx) / 2
        sub     ax, cx
.half:  shr     ax, 1
        and     ax, 0x3fff
.bset:  mov     word [bp+V_DC], 3
        mov     [bp+V_D], ax
        mov     [bp+V_DM], ax
        mov     [bp+V_D2], di
        mov     word [bp+V_D1], 0xffff
        jmp     .draw

.line:  mov     dx, [es:si+0x18]        ; dy = |y2 - y1|
        sub     dx, [es:si+0x0a]
        jns     .ly
        neg     dx
.ly:    xor     bl, bl                  ; the octant, worked out as the GDC
        mov     cx, [es:si+0x16]        ; wants it
        sub     cx, [es:si+0x08]        ; dx = x2 - x1
        jnz     .lnz
        mov     bl, 7
        mov     ax, [es:si+0x0a]
        cmp     ax, [es:si+0x18]
        jle     .lset
        mov     bl, 3
        jmp     short .lset
.lnz:   jl      .lneg
        mov     ax, [es:si+0x0a]
        cmp     ax, [es:si+0x18]
        jl      .loct
        add     bl, 2
        jmp     short .loct
.lneg:  neg     cx
        mov     bl, 4
        mov     ax, [es:si+0x0a]
        cmp     ax, [es:si+0x18]
        jg      .loct
        add     bl, 2
.loct:  test    bl, 2
        jz      .l2
        cmp     cx, dx
        jg      .lset
        inc     bl
        jmp     short .lset
.l2:    cmp     cx, dx
        jl      .lset
        inc     bl
.lset:  mov     al, bl
        inc     al
        test    al, 2
        jnz     .lns
        xchg    cx, dx
.lns:   add     bl, 0x08
        mov     [bp+V_OPE], bl
        mov     [bp+V_DC], cx
        shl     dx, 1
        mov     [bp+V_D1], dx
        sub     dx, cx
        mov     [bp+V_D], dx
        sub     dx, cx
        mov     [bp+V_D2], dx

.draw:  mov     al, 0x78                ; TEXTW: the line pattern
        call    ggdc_cmd
        mov     al, [es:si+0x21]
        call    rev8
        call    ggdc_par
        mov     al, [es:si+0x20]
        call    rev8
        call    ggdc_par
        mov     ax, [es:si+0x20]
        mov     [W_PRXGLS], ax
        mov     bh, 0x6c                ; VECTE
        call    ucw_draw
        add     sp, 12
        INTLEAVE

i18_49:                                 ; graphic character
        sub     sp, 12
        call    vect_clear
        mov     es, [bp+F_DS]
        mov     si, [bp+F_BX]
        mov     al, 0x78                ; TEXTW: the 8x8 pattern
        call    ggdc_cmd
        xor     bx, bx
.t:     mov     al, [es:si+0x20+bx]
        mov     [W_PRXGLS+bx], al
        call    rev8
        call    ggdc_par
        inc     bx
        cmp     bx, 8
        jb      .t
        mov     al, [es:si+3]
        and     al, 7
        add     al, 0x10
        mov     [bp+V_OPE], al
        mov     ax, [es:si+0x0c]        ; GBLNG1
        or      ax, ax
        jz      .def
        mov     [bp+V_D], ax
        mov     ax, [es:si+0x1e]        ; GBLNG2 - 1
        dec     ax
        and     ax, 0x3fff
        mov     [bp+V_DC], ax
        jmp     short .go
.def:   mov     word [bp+V_DC], 7
        mov     word [bp+V_D], 7
.go:    mov     bh, 0x68                ; TEXTE
        call    ucw_draw
        add     sp, 12
        INTLEAVE

; The figure in [bp-12..] drawn at the UCW's GBSX1/GBSY1 (ES:SI) on the
; planes CH asks for, BH = the GDC command that draws it.
ucw_draw:
        mov     ax, [es:si+0x0a]        ; GBSY1
        mov     cl, [bp+F_CX+1]
        and     cl, 0xc0
        cmp     cl, 0x40                ; the lower 200 lines
        jne     .y
        add     ax, 200
.y:     mov     dx, 40
        mul     dx
        mov     dx, [es:si+0x08]        ; GBSX1
        mov     cl, dl
        and     cl, 0x0f                ; the dot within the word
        shr     dx, 4
        add     ax, dx                  ; AX = word address
        mov     ch, [bp+F_CX+1]
        and     ch, 0x30
        cmp     ch, 0x30
        jne     .one
        mov     dl, [es:si]             ; all three planes, set or cleared
        add     ax, 0x4000              ; by GBON_PTN
        mov     bl, 2
        test    dl, 1
        jz      .p1
        mov     bl, 3
.p1:    call    gdc_figure
        add     ax, 0x4000
        mov     bl, 2
        test    dl, 2
        jz      .p2
        mov     bl, 3
.p2:    call    gdc_figure
        add     ax, 0x4000
        mov     bl, 2
        test    dl, 4
        jz      .p3
        mov     bl, 3
.p3:    call    gdc_figure
        jmp     short .done
.one:   mov     bl, [es:si+2]           ; one plane, GBDOTU says how
        and     bl, 3
        shr     ch, 4
        inc     ch
        shl     ch, 6                   ; plane 1-3 -> 4000h, 8000h, C000h
        add     ah, ch
        call    gdc_figure
.done:  and     byte [W_PRXDUPD], 0xfc
        or      [W_PRXDUPD], bl
        ret

; AX = word address, CL = dot, BL = drawing mode, BH = draw command.
gdc_figure:
        push    ax
        push    cx
        push    ax
        mov     al, 0x49                ; CSRW
        call    ggdc_cmd
        pop     ax
        call    ggdc_par
        mov     al, ah
        call    ggdc_par
        mov     al, cl
        shl     al, 4
        call    ggdc_par
        mov     al, bl                  ; WDAT: the mode, no data
        or      al, 0x20
        call    ggdc_cmd
        mov     al, 0x4c                ; VECTW
        call    ggdc_cmd
        push    si
        lea     si, [bp-12]
        mov     cx, 11
.v:     mov     al, [ss:si]
        call    ggdc_par
        inc     si
        loop    .v
        pop     si
        mov     al, bh
        call    ggdc_cmd
        pop     cx
        pop     ax
        ret

gdc_csrform_on:
        push    ax
        or      byte [W_CRT_STS_FLAG], 0x40
        mov     al, [W_CRT_RASTER]
        or      al, 0x80                ; cursor displayed
        call    csr_write
        pop     ax
        ret

; Writes display area 0 from W_CRT_W_VRAMADR and W_CRT_W_RASTER; the rest of
; the PRAM goes to zero.
gdc_pram1:
        push    ax
        push    bx
        push    cx
        mov     al, 0x70
        out     TGDC_CMD, al
        JIODLY
        mov     ax, [W_CRT_W_VRAMADR]
        mov     bx, [W_CRT_W_RASTER]
        out     TGDC_PARAM, al          ; SAD low
        JIODLY
        mov     al, ah
        and     al, 0x1f                ; SAD high, then LEN low in bits 4-7:
        mov     ah, bl                  ; BX holds LEN << 4, so BL's top
        and     ah, 0xf0                ; nibble is LEN's bottom one
        or      al, ah
        out     TGDC_PARAM, al
        JIODLY
        mov     al, bh                  ; LEN high: LEN >> 4
        and     al, 0x3f
        out     TGDC_PARAM, al
        JIODLY
        xor     al, al
        mov     cx, 5
.z:     out     TGDC_PARAM, al
        JIODLY
        loop    .z
        pop     cx
        pop     bx
        pop     ax
        ret

; ES:SI -> (byte address, lines) pairs, CL = first area, CH = how many.
gdc_pramtable:
        push    ax
        push    bx
        push    dx
        mov     al, 0x70
        out     TGDC_CMD, al
        JIODLY
        xor     dl, dl                  ; parameter bytes written so far
.area:  cmp     ch, 0
        je      .pad
        cmp     dl, 16
        jae     .pad
        mov     ax, [es:si]
        shr     ax, 1                   ; SAD, in words
        mov     bx, ax
        out     TGDC_PARAM, al
        JIODLY
        push    bx
        mov     ax, [es:si+2]           ; lines -> raster units
        mov     bx, 16
        mul     bx
        mov     bx, ax                  ; BX = LEN
        pop     ax                      ; AX = SAD
        mov     al, ah
        and     al, 0x1f
        mov     ah, bl
        shl     ah, 1
        shl     ah, 1
        shl     ah, 1
        shl     ah, 1
        or      al, ah
        out     TGDC_PARAM, al
        JIODLY
        mov     ax, bx                  ; LEN high: LEN >> 4
        shr     ax, 4
        and     al, 0x3f
        out     TGDC_PARAM, al
        JIODLY
        xor     al, al
        out     TGDC_PARAM, al
        JIODLY
        add     si, 4
        add     dl, 4
        dec     ch
        jmp     short .area
.pad:   cmp     dl, 16
        jae     .done
        xor     al, al
        out     TGDC_PARAM, al
        JIODLY
        inc     dl
        jmp     short .pad
.done:  pop     dx
        pop     bx
        pop     ax
        ret

; ===========================================================================
; INT 1Ch - timer BIOS
; ===========================================================================
int1c:
        INTFRAME
        mov     al, [bp+F_AX+1]
        cmp     al, 0x00
        je      .get
        cmp     al, 0x01
        je      .put
        cmp     al, 0x02
        je      .interval
        cmp     al, 0x03
        je      .continue
        INTLEAVE

.get:                                   ; ES:BX <- six BCD bytes
        mov     es, [bp+F_ES]
        mov     bx, [bp+F_BX]
        call    rtc_read
        INTLEAVE

.put:                                   ; ES:BX -> six BCD bytes
        mov     es, [bp+F_ES]
        mov     bx, [bp+F_BX]
        call    rtc_write
        INTLEAVE

.interval:                              ; arm the one shot interval timer
        mov     ax, [bp+F_BX]
        mov     [0x001c], ax            ; INT 07h is the user's routine
        mov     ax, [bp+F_ES]
        mov     [0x001e], ax
        mov     ax, [bp+F_CX]
        mov     [W_CA_TIM_CNT], ax
        mov     byte [BV_TIMERMODE], 1
        mov     al, 0x36
        out     PIT_CTRL, al
        JIODLY
.continue:
        call    pit_interval
        in      al, PIC0_IMR
        and     al, 0xfe                ; let IRQ0 through
        out     PIC0_IMR, al
        INTLEAVE

; ---------------------------------------------------------------------------
; How much memory there is, as the power-on test finds it: main memory in
; 0501h bits 0-2 (128KB blocks above the first), extended memory in 0401h
; (128KB blocks above 1MB, at most 14MB). Each block is written and read
; back; extended memory through INT 1Fh's block move.
; ---------------------------------------------------------------------------
MS_GDT          equ 0x0700              ; scratch at POST: descriptors
MS_OUT          equ 0x0740              ; the word written
MS_IN           equ 0x0742              ; the word read back
mem_size:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        xor     ax, ax
        mov     ds, ax

        xor     cl, cl                  ; main memory, 128KB-640KB
        mov     bx, 0x2000
.main:  mov     es, bx
        mov     ax, [es:0]
        mov     word [es:0], 0x5aa5
        cmp     word [es:0], 0x5aa5
        mov     [es:0], ax
        jne     .mdone
        inc     cl
        add     bx, 0x2000
        cmp     bx, 0xa000
        jb      .main
.mdone: and     byte [W_BIOS_FLAG1], 0xf8
        or      [W_BIOS_FLAG1], cl

        xor     dl, dl                  ; DL = 128KB blocks found
        cmp     byte [W_SYS_TYPE], 0    ; a V30 reaches 1MB and no further
        je      .edone
        mov     di, MS_GDT              ; the descriptor table, zeroed
        xor     ax, ax
        push    ds
        pop     es
        mov     cx, 0x20
        rep     stosw
        xor     dl, dl                  ; DL = 128KB blocks found
        mov     bx, 0x10                ; BX:00 = A23-A16 of the block (1MB up)
.ext:   mov     ax, bx
        mov     [MS_OUT], ax
        mov     word [MS_IN], 0
        add     word [MS_OUT], 0x6d39   ; a value tied to this block
        ; write: MS_OUT -> the block
        mov     word [MS_GDT+0x10], 0xffff
        mov     word [MS_GDT+0x12], MS_OUT
        mov     byte [MS_GDT+0x14], 0
        mov     byte [MS_GDT+0x15], 0x93
        mov     word [MS_GDT+0x18], 0xffff
        mov     word [MS_GDT+0x1a], 0
        mov     [MS_GDT+0x1c], bl
        mov     byte [MS_GDT+0x1d], 0x93
        call    .move
        jc      .edone
        ; read back: the block -> MS_IN
        mov     word [MS_GDT+0x12], 0
        mov     [MS_GDT+0x14], bl
        mov     word [MS_GDT+0x1a], MS_IN
        mov     byte [MS_GDT+0x1c], 0
        call    .move
        jc      .edone
        mov     ax, [MS_OUT]
        cmp     ax, [MS_IN]
        jne     .edone
        inc     dl
        add     bx, 2                   ; next 128KB
        cmp     dl, 14 * 8
        jb      .ext
.edone: mov     [W_EXPMMSZ], dl
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret
.move:  push    bx
        push    dx
        xor     ax, ax
        mov     es, ax
        mov     bx, MS_GDT
        xor     si, si
        xor     di, di
        mov     cx, 2
        mov     ah, 0x90
        int     0x1f
        pop     dx
        pop     bx
        ret

; ===========================================================================
; INT 1Ah - printer (AH = 1xh, 3xh) and cassette (AH = 0xh). There is no
; cassette; the printer is driven through the 8255 at 40h-46h, BUSY# being
; bit 2 of port 42h.
;
; The entry keeps NEC's layout, because MS-DOS (IO.SYS) relies on it: it
; hooks this vector, does STI / PUSH DS / PUSH DX itself and jumps straight
; to the printer path at entry+19h, which must end in POP DX / POP DS / IRET.
; ===========================================================================
int1a:
        sti                             ; +00
        push    ds                      ; +01
        push    dx                      ; +02
        test    ah, 0x10                ; +03  1xh/3xh: the printer
        jnz     .prt19                  ; +06
        call    i1a_body                ; +08  cassette
        jmp     short .ret              ; +0B
        times   0x19 - ($ - int1a) nop
.prt19: call    i1a_body                ; +19  IO.SYS comes in here
.ret:   pop     dx
        pop     ds
        iret
%if .prt19 - int1a != 0x19
%error "INT 1Ah printer entry must sit at +19h"
%endif

; The work itself: AH = function, every register as at the INT.
i1a_body:
        INTFRAME
        mov     ah, [bp+F_AX+1]
        mov     al, ah
        and     al, 0xf0
        cmp     al, 0x10
        je      .prt
        cmp     al, 0x30
        je      .prt
        mov     al, 0x00                ; cassette: nothing there
        cmp     ah, 0x04
        jne     .ah
        mov     al, 0x02
        jmp     short .ah
.prt:   mov     al, ah
        and     al, 0x0f
        cmp     al, 0x01
        je      .out1
        cmp     al, 0x02
        je      .sense
        cmp     al, 0x00
        jne     .zero
        cmp     ah, 0x30
        je      .block
        mov     al, 0x0d                ; initialise: printer f/f, reset,
        out     SYSPORT_CTRL, al        ; strobe inactive
        JIODLY
        mov     al, 0x82
        out     0x46, al
        JIODLY
        mov     al, 0x0f
        out     0x46, al
        JIODLY
        mov     al, 0x0c
        out     SYSPORT_CTRL, al
        JIODLY
.sense: in      al, 0x42                ; AH = 1 when not busy
        shr     al, 1
        shr     al, 1
        and     al, 1
        jmp     short .ah
.out1:  mov     al, [bp+F_AX]
        call    prt_char
        jmp     short .ah
.block: mov     cx, [bp+F_CX]           ; CX characters from ES:BX
        mov     al, 0x02
        jcxz    .ah
        mov     es, [bp+F_ES]
        mov     bx, [bp+F_BX]
.b:     mov     al, [es:bx]
        call    prt_char
        test    al, 0x02
        jnz     .bfail
        inc     bx
        dec     word [bp+F_CX]
        loop    .b
        mov     [bp+F_BX], bx
        xor     al, al
        jmp     short .ah
.bfail: mov     [bp+F_BX], bx
        mov     al, 0x02
        jmp     short .ah
.zero:  xor     al, al
.ah:    mov     [bp+F_AX+1], al
        pop     es
        pop     ds
        popa
        ret

; AL = the character. AL <- 1 when it went out, 2 when the printer is busy.
prt_char:
        push    dx
        mov     dl, al
        in      al, 0x42
        test    al, 0x04
        jz      .busy
        mov     al, dl
        out     0x40, al
        mov     al, 0x01
        pop     dx
        ret
.busy:  mov     al, 0x02
        pop     dx
        ret

; ===========================================================================
; INT 1Fh - extended BIOS. AH=90h moves a block to or from extended memory;
; EMS emulators (MIMIC and the like) page through it. Anything else comes
; back with carry set rather than pretending to have done something.
; ===========================================================================
int1f:
        INTFRAME
        cmp     byte [bp+F_AX+1], 0x90
        jne     .fail
        cmp     byte [W_SYS_TYPE], 0    ; LOADALL needs a 286
        je      .fail
        call    xmove
        jc      .fail
        and     word [bp+F_FL], 0xfffe
        INTLEAVE
.fail:  or      word [bp+F_FL], 0x0001
        INTLEAVE

; INT 1Fh AH=90h. ES:BX -> the descriptor table: the source at +10h and the
; destination at +18h, each a limit word and a 24-bit base. SI and DI are
; the offsets into them, CX the byte count (0 = 64KB). CF set when either
; side ends before the count does; what fitted is moved all the same.
;
; Without protected mode the move is made with the 286's LOADALL, which
; loads the segment base caches directly: DS and ES are pointed at the two
; bases - above 1MB if need be - while the CPU stays in real mode, and an
; ordinary REP MOVSB does the rest. LOADALL takes its image from 0800h, so
; what lives there is kept on the stack and put back afterwards.
LA_IMAGE        equ 0x0800
LA_LEN          equ 0x66
xmove:
        mov     es, [bp+F_ES]
        mov     bx, [bp+F_BX]
        mov     si, [bp+F_SI]
        mov     di, [bp+F_DI]
        mov     cx, [bp+F_CX]
        dec     cx                      ; bytes - 1: 0 becomes FFFFh, 64KB
        xor     dx, dx                  ; DL = 1 if a side runs out
        mov     ax, [es:bx+0x10]        ; source limit
        cmp     si, ax
        ja      .bad
        sub     ax, si                  ; source bytes left - 1
        cmp     cx, ax
        jbe     .s1
        mov     cx, ax
        mov     dl, 1
.s1:    mov     ax, [es:bx+0x18]        ; destination limit
        cmp     di, ax
        ja      .bad
        sub     ax, di
        cmp     cx, ax
        jbe     .s2
        mov     cx, ax
        mov     dl, 1
.s2:    push    si                      ; [bp-4]
        push    di                      ; [bp-6]
        push    cx                      ; [bp-8]  bytes - 1
        push    dx                      ; [bp-10]
        push    es                      ; [bp-12]
        push    bx                      ; [bp-14]
        sub     sp, LA_LEN              ; [bp-14-LA_LEN] what 0800h held
        cli
        cld
        mov     ax, ss                  ; keep 0000:0800h on the stack
        mov     es, ax
        mov     di, sp
        mov     si, LA_IMAGE
        mov     cx, LA_LEN / 2
        rep     movsw

        xor     ax, ax                  ; the LOADALL image
        mov     es, ax
        mov     di, LA_IMAGE
        mov     cx, LA_LEN / 2
        rep     stosw
        mov     ax, 0xfff0              ; MSW: a 286 in real mode, PE clear
        mov     [LA_IMAGE+0x04], ax     ; MSW (an emulator that reads it
        mov     [LA_IMAGE+0x06], ax     ; two bytes early finds it too)
        mov     word [LA_IMAGE+0x18], 0x0002    ; FLAGS, interrupts off
        mov     word [LA_IMAGE+0x1a], .moving   ; IP
        mov     ax, ss
        mov     [LA_IMAGE+0x20], ax     ; SS
        mov     word [LA_IMAGE+0x22], ROM_SEG   ; CS
        mov     ax, [bp-6]
        mov     [LA_IMAGE+0x26], ax     ; DI
        mov     ax, [bp-4]
        mov     [LA_IMAGE+0x28], ax     ; SI
        mov     [LA_IMAGE+0x2a], bp     ; BP
        mov     [LA_IMAGE+0x2c], sp     ; SP
        mov     ax, [bp-8]
        mov     [LA_IMAGE+0x32], ax     ; CX
        mov     es, [bp-12]
        mov     bx, [bp-14]
        mov     ax, [es:bx+0x1a]        ; ES cache: the destination
        mov     [LA_IMAGE+0x36], ax
        mov     al, [es:bx+0x1c]
        mov     [LA_IMAGE+0x38], al
        mov     byte [LA_IMAGE+0x39], 0x93
        mov     word [LA_IMAGE+0x3a], 0xffff
        mov     word [LA_IMAGE+0x3c], (ROM_SEG << 4) & 0xffff   ; CS cache
        mov     byte [LA_IMAGE+0x3e], ROM_SEG >> 12
        mov     byte [LA_IMAGE+0x3f], 0x9b
        mov     word [LA_IMAGE+0x40], 0xffff
        mov     ax, ss                  ; SS cache: as it is
        mov     dx, ax
        shl     ax, 4
        shr     dx, 12
        mov     [LA_IMAGE+0x42], ax
        mov     [LA_IMAGE+0x44], dl
        mov     byte [LA_IMAGE+0x45], 0x93
        mov     word [LA_IMAGE+0x46], 0xffff
        mov     ax, [es:bx+0x12]        ; DS cache: the source
        mov     [LA_IMAGE+0x48], ax
        mov     al, [es:bx+0x14]
        mov     [LA_IMAGE+0x4a], al
        mov     byte [LA_IMAGE+0x4b], 0x93
        mov     word [LA_IMAGE+0x4c], 0xffff
        mov     word [LA_IMAGE+0x5e], 0x03ff    ; IDTR: the real-mode table

        out     0xf2, al                ; A20 on: past 1MB
        db      0x0f, 0x05              ; LOADALL
.moving:
        cld
        rep     movsb                   ; CX = bytes - 1
        movsb

        xor     ax, ax                  ; ordinary segments again
        mov     es, ax
        mov     ax, ss
        mov     ds, ax
        lea     si, [bp-14-LA_LEN]
        mov     di, LA_IMAGE
        mov     cx, LA_LEN / 2
        rep     movsw
        xor     ax, ax
        mov     ds, ax
        mov     al, 0x03                ; A20 off again
        out     0xf6, al
        add     sp, LA_LEN
        pop     bx
        pop     es
        pop     dx
        pop     cx
        pop     di
        pop     si
        shr     dl, 1                   ; CF = a side ran out
        ret
.bad:   stc
        ret

; ===========================================================================
;  Calendar clock. The uPD4990A shifts second first, least-significant bit
;  first; the BIOS buffer is year through second, so the byte order reverses.
; ===========================================================================
rtc_read:
        push    ax
        push    cx
        push    dx
        push    si
        push    di
        mov     al, 0x03                ; TIME READ
        out     RTC_CTRL, al
        mov     al, 0x0b
        out     RTC_CTRL, al
        mov     al, 0x03
        out     RTC_CTRL, al
        mov     al, 0x01                ; REGISTER SHIFT
        out     RTC_CTRL, al
        mov     al, 0x09
        out     RTC_CTRL, al
        mov     al, 0x01
        out     RTC_CTRL, al
        mov     di, bx
        add     di, 5
        mov     si, 6
.byte: xor     dl, dl
        mov     cx, 8
.bit:  in      al, RTC_DATA
        shr     al, 1                   ; serial data bit -> carry
        rcr     dl, 1                   ; first bit eventually becomes bit 0
        mov     al, 0x11
        out     RTC_CTRL, al
        JIODLY
        mov     al, 0x01
        out     RTC_CTRL, al
        loop    .bit
        mov     [es:di], dl
        dec     di
        dec     si
        jnz     .byte
        xor     al, al                  ; REGISTER HOLD
        out     RTC_CTRL, al
        mov     al, 0x08
        out     RTC_CTRL, al
        xor     al, al
        out     RTC_CTRL, al
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     ax
        ret

rtc_write:
        push    ax
        push    cx
        push    dx
        push    si
        mov     al, 0x01                ; REGISTER SHIFT
        out     RTC_CTRL, al
        mov     al, 0x09
        out     RTC_CTRL, al
        mov     al, 0x01
        out     RTC_CTRL, al
        mov     si, bx
        add     si, 5
        mov     dh, 6
.byte: mov     dl, [es:si]
        call    rtc_shift_byte
        dec     si
        dec     dh
        jnz     .byte
        mov     dl, 1                   ; control byte behind the six BCD bytes
        call    rtc_shift_byte
        xor     dl, dl
        call    rtc_shift_byte
        mov     al, 0x02                ; TIME SET
        out     RTC_CTRL, al
        mov     al, 0x0a
        out     RTC_CTRL, al
        mov     al, 0x02
        out     RTC_CTRL, al
        pop     si
        pop     dx
        pop     cx
        pop     ax
        ret

; Shift DL into the clock, least-significant bit first.
rtc_shift_byte:
        push    ax
        push    cx
        mov     cx, 8
.bit:  mov     al, 0x01
        test    dl, 1
        jz      .data
        or      al, 0x20
.data: out     RTC_CTRL, al
        or      al, 0x10
        out     RTC_CTRL, al
        and     al, 0xef
        out     RTC_CTRL, al
        shr     dl, 1
        loop    .bit
        pop     cx
        pop     ax
        ret

; ===========================================================================
; INT 1Bh - disk BIOS
;
;   AL = DA/UA        AH = command      BX = byte count
;   FDD: CL = cylinder, CH = sector size, DH = head, DL = sector
;   HDD CHS: CX = cylinder, DH = head, DL = zero-based sector
;   HDD linear: DX:CX = LBA
;   ES:BP = buffer
;   returns AH = status, carry set when AH is 20h or more
; ===========================================================================
int1b:
        INTFRAME

        ; A PC-9801-55 option ROM owns ordinary SCSI disk calls when present.
        ; AH=B0h is kept here because EPSON Startup uses a direct-CDB extension
        ; that the common option ROM interface does not implement.
        mov     al, [bp+F_AX]
        and     al, 0xf0
        cmp     al, 0x20
        je      .check_scsi_rom
        cmp     al, 0xa0
        jne     .internal
.check_scsi_rom:
        cmp     word [BV_SCSI_ROM], 0
        je      .internal
        cmp     byte [bp+F_AX+1], 0xb0
        je      .internal
        jmp     scsi_option_chain

.internal:
        ; copy the call into the request block the drivers work from
        mov     al, [bp+F_AX]
        mov     [BV_REQ_DAUA], al
        mov     al, [bp+F_AX+1]
        mov     [BV_REQ_CMD], al
        mov     ax, [bp+F_BX]
        mov     [BV_REQ_LEN], ax
        mov     ax, [bp+F_CX]
        mov     [BV_REQ_WORD_CX], ax
        mov     al, [bp+F_CX]
        mov     [BV_REQ_C], al
        mov     al, [bp+F_CX+1]
        mov     [BV_REQ_N], al
        mov     ax, [bp+F_DX]
        mov     [BV_REQ_WORD_DX], ax
        mov     al, [bp+F_DX+1]
        mov     [BV_REQ_H], al
        mov     al, [bp+F_DX]
        mov     [BV_REQ_R], al
        mov     ax, [bp+F_ES]
        mov     [BV_REQ_SEG], ax
        mov     ax, [bp+F_BP]
        mov     [BV_REQ_OFF], ax

        ; Direct SCSI (AH=B0h) supplies its CDB at ES:SI.  CX is otherwise
        ; unused by that interface, so its request-block slot carries SI.
        cmp     byte [BV_REQ_CMD], 0xb0
        jne     .request_ready
        mov     ax, [bp+F_SI]
        mov     [BV_REQ_WORD_CX], ax
.request_ready:
        call    disk_operate            ; AH = status

        ; FDD READ ID hands back the ID it read, as the NEC BIOS does:
        ; CL = C, DH = H, DL = R, CH = N.
        mov     al, [BV_REQ_CMD]
        and     al, 0x0f
        cmp     al, 0x0a
        jne     .not_readid
        cmp     ah, 0x20
        jae     .not_readid
        mov     al, [BV_REQ_DAUA]
        and     al, 0x70
        cmp     al, 0x10                ; 1xh, 3xh, 5xh, 7xh, 9xh, Bxh, Dxh, Fxh
        je      .readid_regs            ; 9xh/1xh: 1MB/640KB FDD
        cmp     al, 0x30
        je      .readid_regs
        cmp     al, 0x50
        je      .readid_regs
        cmp     al, 0x70
        jne     .not_readid
.readid_regs:
        mov     al, [W_DISK_RESULT+3]   ; C
        mov     [bp+F_CX], al
        mov     al, [W_DISK_RESULT+6]   ; N
        mov     [bp+F_CX+1], al
        mov     al, [W_DISK_RESULT+4]   ; H
        mov     [bp+F_DX+1], al
        mov     al, [W_DISK_RESULT+5]   ; R
        mov     [bp+F_DX], al
.not_readid:
        ; Extended fixed-disk sense returns geometry through the caller frame.
        cmp     byte [BV_REQ_CMD], 0x84
        jne     .setstatus
        cmp     ah, 0x20
        jae     .setstatus
        mov     al, [BV_REQ_DAUA]
        and     al, 0xf0
        cmp     al, 0x80
        je      .sasi_or_ide_geometry
        or      al, al
        je      .sasi_or_ide_geometry
        cmp     al, 0xa0                ; PC-9801-55 SCSI, CHS form
        je      .scsi_geometry
        cmp     al, 0x20                ; PC-9801-55 SCSI, linear form
        jne     .setstatus
.scsi_geometry:
        mov     word [bp+F_BX], 512
        mov     word [bp+F_CX], 0x0fff
        mov     word [bp+F_DX], 0x0811
        jmp     short .setstatus
.sasi_or_ide_geometry:
        call    sasi_media_type
        jnc     .setstatus               ; SASI SENSE returns the media code
        call    ide_identify_geometry
        jc      .setstatus
        mov     word [bp+F_BX], 512
        mov     ax, [BV_LBAL]             ; ATA IDENTIFY word 1: cylinders
        mov     [bp+F_CX], ax
        mov     ah, [BV_LBAH]             ; word 3: heads
        mov     al, [BV_TMP]              ; word 6: sectors per track
        mov     [bp+F_DX], ax

.setstatus:
        mov     [bp+F_AX+1], ah
        and     word [bp+F_FL], 0xfffe
        cmp     ah, 0x20
        jb      .ok
        or      word [bp+F_FL], 0x0001
.ok:    INTLEAVE

; Rebuild the register frame expected by the PC-9801-55 option ROM.  Its
; interrupt entry restores these nine registers itself and finishes with IRET.
scsi_option_chain:
        pop     es
        pop     ds
        popa
        push    ds
        push    si
        push    di
        push    es
        push    bp
        push    dx
        push    cx
        push    bx
        push    ax
        mov     bp, sp
        jmp     0xd200:0x0018

; Works entirely from the request block, so the bootstrap can call it too.
; Returns AH = status.
disk_operate:
        mov     al, [BV_REQ_DAUA]
        and     al, 0xf0
        cmp     al, 0x90                ; 1.25MB
        je      .fdd
        cmp     al, 0x30                ; 1.44MB
        je      .fdd
        cmp     al, 0xb0
        je      .fdd
        cmp     al, 0x70                ; 640KB
        je      .fdd
        cmp     al, 0x10
        je      .fdd
        cmp     al, 0xf0
        je      .fdd
        cmp     al, 0x80                ; IDE
        je      .hdd
        cmp     al, 0x00
        je      .hdd
        cmp     al, 0xa0                ; SCSI disk, CHS form
        je      .scsi
        cmp     al, 0x20                ; SCSI disk, linear form
        je      .scsi
        mov     ah, 0x40                ; no such device
        ret
.fdd:   jmp     fdd_operate
.hdd:   jmp     hdd_operate
.scsi: jmp     scsi_operate

; ---------------------------------------------------------------------------
;  Floppy. Everything below drives the uPD765A and the DMA controller.
; ---------------------------------------------------------------------------
fdd_operate:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        mov     byte [BV_FDBUSY], 1     ; INT 13h keeps its hands off

        call    fdd_setmode             ; media type -> registers and format
        mov     al, [BV_REQ_CMD]
        and     al, 0x0f
        cmp     al, 0x00
        je      .seek
        cmp     al, 0x01
        je      .read                   ; verify: read and throw away
        cmp     al, 0x02
        je      .read
        cmp     al, 0x03
        je      .init
        cmp     al, 0x04
        je      .sense
        cmp     al, 0x05
        je      .write
        cmp     al, 0x06
        je      .read
        cmp     al, 0x07
        je      .recal                  ; RECALIBRATE: back to cylinder 0
        cmp     al, 0x0a
        je      .readid
        cmp     al, 0x0d
        je      .format
        mov     ah, 0x40
        jmp     .out

.format:
        call    fdc_reset
        test    byte [BV_REQ_CMD], 0x10 ; seek first?
        jz      .fmtnoseek
        mov     al, [BV_REQ_C]
        call    fdc_seek
        jc      .seekerr
.fmtnoseek:
        call    fdc_format
        jc      .fail
        mov     ah, 0x00
        jmp     .out

.recal: call    fdc_reset
        call    fdc_recal
        jc      .seekerr
        mov     ah, 0x00
        jmp     .out

.init:  call    fdc_reset
        mov     ah, 0x00
        jmp     .out

.sense: mov     al, 0x04                ; SENSE DEVICE STATUS: ask the drive
        call    fdc_out
        jc      .snr
        mov     al, [BV_REQ_H]
        and     al, 1
        shl     al, 1
        shl     al, 1
        mov     ah, [BV_REQ_DAUA]
        and     ah, 3
        or      al, ah
        call    fdc_out
        jc      .snr
        call    fdc_in                  ; AL = ST3
        jc      .snr
        test    al, 0x20                ; RY: a disk in the drive
        jz      .snr
        mov     ah, 0x00
        test    al, 0x40                ; WP
        jz      .s1
        mov     ah, 0x10                ; write protected
.s1:    test    byte [BV_REQ_DAUA], 0x80
        jz      .sext
        or      ah, 0x01                ; 2HD
        jmp     short .sext
.snr:   mov     ah, 0x60                ; not ready
.sext:  mov     al, [BV_REQ_CMD]        ; extended sense (AX & 8F40h = 8400h)
        and     al, 0x8f                ; also says what the drive can take
        cmp     al, 0x84
        jne     .s2
        test    byte [BV_REQ_DAUA], 0x40
        jnz     .s2
        or      ah, 0x08                ; 1MB/640KB drive
        test    byte [BV_REQ_CMD], 0x40
        jz      .s2
        or      ah, 0x04                ; and 1.44MB
.s2:    jmp     .out

; Seek and READ ID leave the controller as it is, like a read: a reset costs the
; time a few sectors take to pass, and a program that reads IDs one after the
; other to see how the track lies (copy protections do) must see them as a real
; BIOS shows them.  A failure resets it for the next call.
.seek:  test    byte [BV_REQ_CMD], 0x10
        jz      .seekok
        mov     al, [BV_REQ_C]
        call    fdc_seek
        jc      .seekerr
.seekok:
        mov     ah, 0x00
        jmp     .out
.seekerr:
        call    fdc_reset
        mov     ah, 0xe0
        jmp     .out

.readid:
        test    byte [BV_REQ_CMD], 0x10 ; seek first?
        jz      .ridnoseek
        mov     al, [BV_REQ_C]
        call    fdc_seek
        jc      .seekerr
.ridnoseek:
        call    fdc_readid
        jc      .xfail
        mov     ah, 0x00
        jmp     .out

.read:  mov     byte [BV_DIR], 0
        jmp     short .xfer
.write: mov     byte [BV_DIR], 1
.xfer:  test    byte [BV_REQ_CMD], 0x10 ; seek first?
        jz      .noseek
        mov     al, [BV_REQ_C]
        call    fdc_seek
        jc      .seekerr
.noseek:
        call    fdd_transfer
        jc      .xfail
        mov     ah, 0x00
        jmp     short .out
.xfail: call    fdc_reset               ; leave the controller clean for the next call
                                        ; (a reset on every call costs a turn of the
                                        ; disk: the next sector goes by meanwhile)
.fail:  test    byte [W_DISK_RESULT], 0x08  ; ST0 NR: no disk in the drive
        jz      .fst
        mov     ah, 0x60                ; not ready
        jmp     .out
.fst:   mov     ah, [W_DISK_RESULT+1]   ; ST1 and ST2 say what went wrong,
        mov     al, [W_DISK_RESULT+2]   ; coded as the NEC BIOS codes them
        test    ah, 0x20                ; DE: CRC error
        jz      .e1
        test    al, 0x20                ; DD: in the data field
        jz      .e0
        mov     ah, 0xb0                ; data CRC error
        jmp     short .out
.e0:    mov     ah, 0xa0                ; ID CRC error
        jmp     short .out
.e1:    test    ah, 0x02                ; NW
        jz      .e2
        mov     ah, 0x70                ; write protected
        jmp     short .out
.e2:    test    ah, 0x01                ; MA
        jz      .e3
        test    al, 0x01                ; MD: no data address mark
        jz      .e2a
        mov     ah, 0xf0
        jmp     short .out
.e2a:   mov     ah, 0xe0                ; no ID address mark
        jmp     short .out
.e3:    test    ah, 0x04                ; ND
        jz      .e4
        mov     ah, 0xc0                ; sector not found
        jmp     short .out
.e4:    test    ah, 0x80                ; EN
        jz      .e5
        mov     ah, 0x30                ; end of cylinder
        jmp     short .out
.e5:    test    ah, 0x10                ; OR
        jz      .e6
        mov     ah, 0x50                ; overrun
        jmp     short .out
.e6:    test    al, 0x80                ; fdd_transfer's DMA boundary mark
        jz      .e7
        mov     ah, 0x20                ; DMA boundary, as the NEC BIOS says
        jmp     short .out
.e7:    mov     ah, 0xd0                ; anything else

.out:   mov     byte [BV_FDBUSY], 0
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

; The DA/UA says which interface and which format is in play.
fdd_setmode:
        push    ax
        mov     al, [BV_REQ_DAUA]
        and     al, 0xf0
        cmp     al, 0x70                ; 640KB media is the other uPD765A
        je      .dd
        cmp     al, 0x10
        je      .dd
        cmp     al, 0xf0
        je      .dd
        mov     byte [BV_FDBASE], FDC_MSR
        mov     byte [BV_FDMODE], 0x03  ; 2HD media, 90h register set
        cmp     al, 0x30                ; 1.44MB?
        je      .hd144
        cmp     al, 0xb0
        je      .hd144
        mov     byte [BV_FDSPT], 8      ; 1.25MB: 8 sectors of 1024 bytes
        mov     byte [BV_FDN], 3
        jmp     short .set
.hd144: mov     byte [BV_FDSPT], 18     ; 1.44MB: 18 sectors of 512
        mov     byte [BV_FDN], 2
        jmp     short .set
.dd:    mov     byte [BV_FDBASE], FDC2_MSR
        mov     byte [BV_FDMODE], 0x00  ; 2DD media, C8h register set
        mov     byte [BV_FDSPT], 9      ; 720KB: 9 sectors of 512
        mov     byte [BV_FDN], 2
.set:   mov     al, [BV_FDMODE]
        out     FDC_MODEREG, al
        JIODLY
        pop     ax
        ret

; BV_FDSPT / BV_FDGPL <- EOT and GPL for the request, from the parameter
; table 0:05F8 (2HD) or 0:05CC (2DD) points at, as NEC's BIOS takes them:
; the unit's table, eight bytes per N, the FM half when MF is clear.
; 1.44MB keeps fdd_setmode's 18.
fdd_spt:
        push    ax
        push    bx
        push    si
        push    es
        mov     byte [BV_FDGPL], 0x1b
        mov     al, [BV_REQ_DAUA]
        and     al, 0x70
        cmp     al, 0x30
        je      .done
        les     si, [W_F2HD_POINTER]
        cmp     byte [BV_FDMODE], 0
        jne     .p
        les     si, [W_F2DD_POINTER]
.p:     mov     bl, [BV_REQ_DAUA]
        and     bl, 3
        xor     bh, bh
        shl     bx, 1
        mov     si, [es:si+bx]          ; this unit's table
        mov     bl, [BV_REQ_N]
        cmp     bl, 3
        jbe     .n
        mov     bl, 3
.n:     xor     bh, bh
        shl     bx, 3
        add     si, bx
        test    byte [BV_REQ_CMD], 0x40 ; MF clear: the FM half
        jnz     .m
        add     si, 4
.m:     mov     al, [es:si]
        or      al, al
        jz      .done
        mov     [BV_FDSPT], al
        mov     al, [es:si+1]
        mov     [BV_FDGPL], al
.done:  pop     es
        pop     si
        pop     bx
        pop     ax
        ret

; ---- the sector loop -------------------------------------------------------
; Splits the caller's byte count into sectors and walks C/H/R as the format
; dictates. CF set if any sector failed.
fdd_transfer:
        push    ax
        push    bx
        push    dx

        mov     bx, [BV_REQ_LEN]
        or      bx, bx
        jz      .done

        ; the buffer as a linear address, and the 64KB boundary rule the
        ; DMA controller imposes
        mov     ax, [BV_REQ_SEG]
        mov     dx, ax
        shr     dx, 12
        shl     ax, 4
        add     ax, [BV_REQ_OFF]
        adc     dx, 0
        mov     [BV_XFERL], ax
        mov     [BV_XFERH], dx
        mov     ax, [BV_XFERL]
        add     ax, bx
        jc      .cross
        jmp     short .setup
.cross: or      ax, ax                  ; ends exactly on the boundary: fine
        jz      .setup
        pop     dx
        pop     bx
        pop     ax
        mov     byte [W_DISK_RESULT+1], 0
        mov     byte [W_DISK_RESULT+2], 0x80    ; never set by the FDC: marks
        stc                                     ; the DMA boundary for .fail
        ret

.setup: mov     al, [BV_REQ_C]
        mov     [BV_REQ_C], al
        mov     al, [BV_REQ_N]
        or      al, al
        jnz     .loop0
        mov     al, [BV_FDN]            ; no size given: use the format's
        mov     [BV_REQ_N], al
.loop0: call    fdd_spt                 ; sectors a track hold at this N
        jmp     short .loop

; The whole sectors left on this side of the track go in one command, as a real
; BIOS does: a command per sector would miss the next sector while it was set
; up, and wait a turn of the disk for every one.
.loop:  mov     cl, [BV_REQ_N]          ; sector size = 128 << N
        add     cl, 7
        mov     dx, 1
        shl     dx, cl                  ; DX = bytes a sector
        mov     al, [BV_REQ_R]
        mov     [BV_FDEOT], al
        mov     ax, bx
        cmp     ax, dx
        jb      .part                   ; a partial sector at the end
        mov     ax, dx
        mov     cl, [BV_REQ_CMD]        ; READ DIAGNOSTIC stays a sector a
        and     cl, 0x0f                ; command: what it reads is the
        cmp     cl, 0x02                ; track in its physical order
        je      .run
        xor     ax, ax                  ; AX = bytes of the run
.grow:  add     ax, dx
        mov     cl, [BV_FDEOT]
        cmp     cl, [BV_FDSPT]
        jae     .run                    ; the last sector of the side
        push    ax
        add     ax, dx
        jc      .full1                  ; another sector would pass 64KB
        cmp     ax, bx
        ja      .full1                  ; or the bytes asked for
        pop     ax
        inc     byte [BV_FDEOT]
        jmp     short .grow
.full1: pop     ax
        jmp     short .run
.part:  mov     ax, bx
.run:   mov     [BV_SECSIZE], ax

        call    fdc_rw
        jc      .fail

        mov     ax, [BV_SECSIZE]
        sub     bx, ax
        jbe     .done
        add     [BV_XFERL], ax
        adc     word [BV_XFERH], 0

        mov     al, [BV_FDEOT]          ; on past the run
        inc     al
        mov     [BV_REQ_R], al
        cmp     al, [BV_FDSPT]
        jbe     .loop
        mov     byte [BV_REQ_R], 1
        test    byte [BV_REQ_CMD], 0x80 ; multi track?
        jz      .nextcyl
        cmp     byte [BV_REQ_H], 0
        jne     .nextcyl
        mov     byte [BV_REQ_H], 1      ; carry on on the other side
        jmp     .loop
.nextcyl:
        mov     byte [BV_REQ_H], 0
        inc     byte [BV_REQ_C]
        mov     al, [BV_REQ_C]
        call    fdc_seek
        jc      .fail
        jmp     .loop

.done:  pop     dx
        pop     bx
        pop     ax
        clc
        ret
.fail:  pop     dx
        pop     bx
        pop     ax
        stc
        ret

; ---- uPD765A primitives ---------------------------------------------------

; AL = byte to hand the controller. CF set if it never asked for it.
fdc_out:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     ah, al
        mov     dl, [BV_FDBASE]
        xor     dh, dh
        mov     bx, FDC_PATIENCE        ; a controller busy with a command can
.o:     xor     cx, cx                  ; take a turn of the disk and more
.w:     in      al, dx
        and     al, 0xc0
        cmp     al, 0x80                ; RQM up, DIO down: it wants a byte
        je      .ok
        loop    .w
        dec     bx
        jnz     .o
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret
.ok:    add     dx, 2
        mov     al, ah
        out     dx, al
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret

; AL <- the next byte out of the controller. CF set on timeout.
fdc_in:
        push    bx
        push    cx
        push    dx
        mov     dl, [BV_FDBASE]
        xor     dh, dh
        mov     bx, FDC_PATIENCE        ; the result of a read comes when the
.o:     xor     cx, cx                  ; sector has gone by: up to a turn of
.w:     in      al, dx                  ; the disk, or more while the drive
        and     al, 0xc0                ; gets the track to it
        cmp     al, 0xc0                ; RQM and DIO both up
        je      .ok
        loop    .w
        dec     bx
        jnz     .o
        pop     dx
        pop     cx
        pop     bx
        stc
        ret
.ok:    add     dx, 2
        in      al, dx
        pop     dx
        pop     cx
        pop     bx
        clc
        ret

; CX result bytes into W_DISK_RESULT.
fdc_result:
        push    ax
        push    di
        mov     di, W_DISK_RESULT
.next:  call    fdc_in
        jc      .fail
        mov     [di], al
        inc     di
        loop    .next
        pop     di
        pop     ax
        clc
        ret
.fail:  pop     di
        pop     ax
        stc
        ret

; Pulses reset, spins the motor up and specifies the step timings.
fdc_reset:
        push    ax
        push    cx
        push    dx
        mov     dl, [BV_FDBASE]
        xor     dh, dh
        add     dx, 4                   ; the control register
        mov     al, 0x80                ; reset
        out     dx, al
        JIODLY
        mov     al, 0x08                ; released, motor running
        out     dx, al
        mov     cx, 0x0800
.d:     loop    .d
        mov     al, 0x03                ; SPECIFY
        call    fdc_out
        mov     al, 0xdf                ; step 3ms, head unload 240ms
        call    fdc_out
        mov     al, 0x02                ; head load 4ms, DMA
        call    fdc_out
        pop     dx
        pop     cx
        pop     ax
        ret

; RECALIBRATE the drive in the request: the head goes back to track 0
; whatever the BIOS thought it was on. CF set if the controller would not
; take the command.
fdc_recal:
        push    ax
        push    bx
        mov     bl, [BV_REQ_DAUA]
        and     bl, 0x03
        xor     bh, bh
        mov     al, 0x07                ; RECALIBRATE
        call    fdc_out
        jc      .fail
        mov     al, bl
        call    fdc_out
        jc      .fail
        mov     byte [bx+BV_FDTRACK], 0
        call    fdc_waitint
        pop     bx
        pop     ax
        clc
        ret
.fail:  pop     bx
        pop     ax
        stc
        ret

; AL = cylinder to move to. CF set if the controller would not take it.
fdc_seek:
        push    ax
        push    bx
        push    dx
        mov     dh, al
        mov     bl, [BV_REQ_DAUA]
        and     bl, 0x03
        xor     bh, bh
        mov     al, [bx+BV_FDTRACK]
        cmp     al, dh
        je      .done
        mov     al, 0x0f                ; SEEK
        call    fdc_out
        jc      .fail
        mov     al, [BV_REQ_H]
        and     al, 1
        shl     al, 1
        shl     al, 1
        or      al, bl
        call    fdc_out
        jc      .fail
        mov     al, dh
        call    fdc_out
        jc      .fail
        mov     [bx+BV_FDTRACK], dh
        call    fdc_waitint
.done:  pop     dx
        pop     bx
        pop     ax
        clc
        ret
.fail:  pop     dx
        pop     bx
        pop     ax
        stc
        ret

; SENSE INTERRUPT STATUS until the controller says the seek of the drive in the
; request finished: ST0 with SE (bit 5) and that drive's number.  Other reports
; - another drive's READY change after a reset, say - are taken and passed
; over; they come before a seek's end, which the controller sees only when its
; polling reaches the drive.  Gives up quietly: a drive that never reports in
; is dealt with by the read that follows failing, rather than by hanging here.
fdc_waitint:
        push    ax
        push    cx
        push    dx
        mov     dx, 0x300               ; a long seek on a slow drive
.again: mov     al, 0x08
        call    fdc_out
        jc      .out
        call    fdc_in                  ; ST0
        jc      .out
        mov     ah, al
        and     ah, 0xc0
        cmp     ah, 0x80                ; 80h means nothing has happened yet
        je      .retry
        mov     ah, al
        call    fdc_in                  ; PCN
        jc      .out
        test    ah, 0x20                ; a seek's end?
        jz      .again
        mov     al, [BV_REQ_DAUA]
        xor     al, ah
        test    al, 0x03                ; of this drive?
        jz      .out
        jmp     short .again
.retry: mov     cx, 0x2000
.spin:  loop    .spin
        dec     dx
        jnz     .again
.out:   pop     dx
        pop     cx
        pop     ax
        ret

; READ ID on the current track: how a disk of unknown format gets identified.
; The result is left in W_DISK_RESULT (ST0 ST1 ST2 C H R N).
fdc_readid:
        push    ax
        push    cx
        mov     al, [BV_REQ_CMD]        ; READ ID, MF as the caller gave it
        and     al, 0x40
        or      al, 0x0a
        call    fdc_out
        jc      .fail
        mov     al, [BV_REQ_H]
        and     al, 1
        shl     al, 1
        shl     al, 1
        mov     ah, [BV_REQ_DAUA]
        and     ah, 3
        or      al, ah
        call    fdc_out
        jc      .fail
        mov     cx, 7
        call    fdc_result
        jc      .fail
        test    byte [W_DISK_RESULT], 0xc0
        jnz     .fail
        pop     cx
        pop     ax
        clc
        ret
.fail:  pop     cx
        pop     ax
        stc
        ret

; FORMAT A TRACK (INT 1Bh xDh) on the track the head is over.
; CH = N, DL = filler byte, ES:BP = the C/H/R/N of each sector, BX = their
; length in bytes. SC and the format GPL come from the format half of the
; parameter table, as NEC's BIOS takes them.
fdc_format:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    es

        mov     byte [BV_DIR], 1        ; the ID list goes memory to port
        les     si, [W_F2HD_POINTER]
        cmp     byte [BV_FDMODE], 0
        jne     .p
        les     si, [W_F2DD_POINTER]
.p:     mov     bl, [BV_REQ_DAUA]
        and     bl, 3
        xor     bh, bh
        shl     bx, 1
        mov     si, [es:si+bx]          ; this unit's table
        mov     bl, [BV_REQ_N]
        cmp     bl, 3
        jbe     .n
        mov     bl, 3
.n:     xor     bh, bh
        shl     bx, 3
        add     si, bx
        test    byte [BV_REQ_CMD], 0x40 ; MF clear: the FM half
        jnz     .m
        add     si, 4
.m:     mov     cl, [es:si+2]           ; SC
        mov     ch, [es:si+3]           ; GPL for format
        mov     al, [BV_REQ_DAUA]       ; 1.44MB: 18 sectors of 512
        and     al, 0x70
        cmp     al, 0x30
        jne     .t
        mov     cx, 0x5412
.t:     pop     es

        mov     ax, [BV_REQ_SEG]        ; the ID list as a linear address
        mov     dx, ax
        shr     dx, 12
        shl     ax, 4
        add     ax, [BV_REQ_OFF]
        adc     dx, 0
        mov     [BV_XFERL], ax
        mov     [BV_XFERH], dx
        mov     ax, [BV_REQ_LEN]        ; four bytes per sector
        mov     bl, cl
        xor     bh, bh
        shl     bx, 2
        or      ax, ax
        jz      .len
        cmp     ax, bx
        jbe     .lenok
.len:   mov     ax, bx
.lenok: mov     [BV_SECSIZE], ax
        call    dma_setup

        mov     al, [BV_REQ_CMD]        ; FORMAT A TRACK, MF as the caller gave it
        and     al, 0x40
        or      al, 0x0d
        call    fdc_out
        jc      .fail
        mov     al, [BV_REQ_H]
        and     al, 1
        shl     al, 1
        shl     al, 1
        mov     ah, [BV_REQ_DAUA]
        and     ah, 3
        or      al, ah
        call    fdc_out
        jc      .fail
        mov     al, [BV_REQ_N]
        call    fdc_out
        jc      .fail
        mov     al, cl                  ; SC
        call    fdc_out
        jc      .fail
        mov     al, ch                  ; GPL
        call    fdc_out
        jc      .fail
        mov     al, [BV_REQ_R]          ; DL: the filler byte
        call    fdc_out
        jc      .fail

        mov     cx, 7
        call    fdc_result
        jc      .fail
        call    dma_mask
        test    byte [W_DISK_RESULT], 0xc0
        jnz     .fail2
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.fail:  call    dma_mask
.fail2: pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; One sector between the disk and BV_XFER..., over DMA channel 2 (channel 1
; for the 640KB interface). BV_DIR picks the direction.
fdc_rw:
        push    ax
        push    bx
        push    cx
        push    dx

        call    dma_setup

        mov     al, 0x06                ; READ DATA
        cmp     byte [BV_DIR], 0
        jne     .wr
        mov     ah, [BV_REQ_CMD]        ; INT 1Bh xx2h: READ DIAGNOSTIC, the
        and     ah, 0x0f                ; track as it lies from the index -
        cmp     ah, 0x02                ; copy protections check its errors
        jne     .mf
        mov     al, 0x02
        jmp     short .mf
.wr:    mov     al, 0x05                ; WRITE DATA
.mf:    mov     ah, [BV_REQ_CMD]        ; MF (bit 6) as the caller gave it
        and     ah, 0x40
        or      al, ah
.cmd:   call    fdc_out
        jc      .fail
        mov     al, [BV_REQ_H]
        and     al, 1
        shl     al, 1
        shl     al, 1
        mov     ah, [BV_REQ_DAUA]
        and     ah, 3
        or      al, ah
        call    fdc_out
        jc      .fail
        mov     al, [BV_REQ_C]
        call    fdc_out
        jc      .fail
        mov     al, [BV_REQ_H]
        call    fdc_out
        jc      .fail
        mov     al, [BV_REQ_R]
        call    fdc_out
        jc      .fail
        mov     al, [BV_REQ_N]
        call    fdc_out
        jc      .fail
        mov     al, [BV_FDEOT]          ; EOT: the last sector of the run
        call    fdc_out
        jc      .fail
        mov     al, [BV_FDGPL]          ; GPL, from the parameter table
        call    fdc_out
        jc      .fail
        mov     al, 0xff                ; DTL, ignored for N > 0
        call    fdc_out
        jc      .fail

        mov     cx, 7
        call    fdc_result
        jc      .fail
        call    dma_mask
        test    byte [W_DISK_RESULT], 0xc0
        jnz     .fail2
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.fail:  call    dma_mask
.fail2: pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; Points the DMA controller at BV_XFER... for BV_SECSIZE bytes.
dma_setup:
        push    ax
        push    bx
        push    cx
        push    dx
        cli
        call    dma_channel             ; AL = channel, BX/CX/DX = its ports
        mov     ah, al
        or      al, 0x04                ; masked while it is set up
        out     DMA_SMASK, al
        JIODLY
        xor     al, al
        out     DMA_CLRFF, al
        JIODLY
        mov     al, 0x44                ; single transfer, port to memory
        cmp     byte [BV_DIR], 0
        je      .mode
        mov     al, 0x48                ; memory to port
.mode:  or      al, ah                  ; plus the channel number
        out     DMA_MODE, al
        JIODLY
        mov     ax, [BV_XFERL]
        push    dx
        mov     dx, bx                  ; address register
        out     dx, al
        JIODLY
        mov     al, ah
        out     dx, al
        pop     dx
        JIODLY
        mov     ax, [BV_XFERH]
        push    dx
        mov     dx, cx                  ; bank register
        out     dx, al
        pop     dx
        JIODLY
        xor     al, al
        out     DMA_CLRFF, al
        JIODLY
        mov     ax, [BV_SECSIZE]
        dec     ax
        out     dx, al                  ; count register
        JIODLY
        mov     al, ah
        out     dx, al
        JIODLY
        call    dma_channel
        out     DMA_SMASK, al           ; and let it run
        sti
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

dma_mask:
        push    ax
        call    dma_channel
        or      al, 0x04
        out     DMA_SMASK, al
        pop     ax
        ret

; AL = channel, BX = address port, CX = bank port, DX = count port.
dma_channel:
        cmp     byte [BV_FDBASE], FDC_MSR
        jne     .dd
        mov     al, 2
        mov     bx, DMA_ADR2
        mov     cx, DMA_BANK2
        mov     dx, DMA_CNT2
        ret
.dd:    mov     al, 3                   ; the 640KB interface is on channel 3
        mov     bx, DMA_ADR3
        mov     cx, DMA_BANK3
        mov     dx, DMA_CNT3
        ret

; ---------------------------------------------------------------------------
;  SASI. The original PC-98 interface exposes an eight-bit data register at
;  80h and a control/phase register at 82h. Units zero and one share target
;  ID zero and are selected through the LUN bits in the six-byte command.
; ---------------------------------------------------------------------------
hdd_operate:
        call    sasi_media_type
        jc      ide_operate             ; no SASI media switch: use ATA
        jmp     sasi_operate

; Hard disk bits of the equipment word (0:055Dh, bit n = SASI unit n), filled at start-up as the real BIOS
; does after its device scan, so an operating system booted from a floppy still sees the SASI drives.
; (Booting from the drive also sets the bit; see boot_sasi.)  Units without a drive keep their bit clear.
; A present board also gets its interrupt unmasked, which the DOS driver relies on.
sasi_equip_init:
        push    ax
        push    bx
        push    cx
        xor     bl, bl
.unit:  mov     al, bl
        or      al, 0x80
        mov     [BV_REQ_DAUA], al
        call    sasi_media_type
        jc      .next
        mov     al, 1
        mov     cl, bl
        shl     al, cl
        or      [W_DISK_EQUIP+1], al
        in      al, PIC1_IMR            ; and let the board's interrupt (IRQ 9, slave IR1) through
        and     al, 0xfd
        out     PIC1_IMR, al
.next:  inc     bl
        cmp     bl, 2
        jb      .unit
        pop     cx
        pop     bx
        pop     ax
        ret

; AL = media type (0..6), CF set when the requested SASI unit is absent.
sasi_media_type:
        push    bx
        push    cx
        mov     al, [BV_REQ_DAUA]
        and     al, 0x0f
        cmp     al, 2
        jae     .none
        xor     al, al
        out     SASI_CTRL, al            ; read the drive type switches
        in      al, SASI_CTRL
        test    byte [BV_REQ_DAUA], 1
        jnz     .unit1
        mov     cl, 3
        shr     al, cl
.unit1: and     al, 7
        cmp     al, 7
        je      .none
        mov     byte [BV_SASI_READY], 1
        pop     cx
        pop     bx
        clc
        ret
.none:  mov     byte [BV_SASI_READY], 0xff
        pop     cx
        pop     bx
        stc
        ret

sasi_operate:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        mov     al, [BV_REQ_CMD]
        and     al, 0x0f
        cmp     al, 0x03
        je      .init
        cmp     al, 0x04
        je      .sense
        cmp     al, 0x01                ; verify uses an ordinary read
        je      .read
        cmp     al, 0x02
        je      .read
        cmp     al, 0x05
        je      .write
        cmp     al, 0x06
        je      .read
        cmp     al, 0x07
        je      .ok
        cmp     al, 0x0f
        je      .ok
        mov     ah, 0x40
        jmp     .out
.init: call     sasi_media_type
        jc      .absent
        mov     ax, 0x0100
        test    byte [BV_REQ_DAUA], 1
        jz      .equip
        shl     ax, 1
.equip: or      [W_DISK_EQUIP], ax
.ok:    xor     ah, ah
        jmp     short .out
.sense: call    sasi_media_type
        jc      .absent
        mov     ah, al                  ; legacy BIOS returns media type
        jmp     short .out
.read:  mov     byte [BV_DIR], 0
        jmp     short .transfer
.write: mov     byte [BV_DIR], 1
.transfer:
        call    sasi_compute_lba
        jc      .bad
        mov     ax, [BV_REQ_LEN]
        or      ax, ax
        jz      .bad
        test    al, al                  ; SASI sectors are 256 bytes
        jnz     .bad
        mov     bx, [BV_REQ_OFF]
        add     bx, ax
        jc      .bad
        mov     [BV_SECCNT], ah         ; one command transfers <=255 blocks
        call    sasi_begin
        jc      .ioerr
        mov     es, [BV_REQ_SEG]
        mov     di, [BV_REQ_OFF]
        mov     cx, [BV_REQ_LEN]
        cmp     byte [BV_DIR], 0
        jne     .wrloop
.rdloop:
        mov     ah, 0xa4                ; BSY|REQ|I/O, DATA IN
        call    sasi_wait_phase
        jc      .ioerr
        in      al, SASI_DATA
        stosb
        loop    .rdloop
        jmp     short .finish
.wrloop:
        mov     ah, 0xa0                ; BSY|REQ, DATA OUT
        call    sasi_wait_phase
        jc      .ioerr
        mov     al, [es:di]
        inc     di
        out     SASI_DATA, al
        loop    .wrloop
.finish:
        call    sasi_finish
        jc      .ioerr
        xor     ah, ah
        jmp     short .out
.bad:   mov     ah, 0xd0
        jmp     short .out
.ioerr: mov     ah, 0xd0
        jmp     short .out
.absent:
        mov     ah, 0x60
.out:   pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

; Convert the PC-98 CHS or 21-bit linear request to the SASI block number.
sasi_compute_lba:
        test    byte [BV_REQ_DAUA], 0x80
        jz      .linear
        call    sasi_media_type
        jc      .fail
        xor     bx, bx
        mov     bl, al
        mov     bl, [cs:sasi_heads+bx]
        xor     bh, bh
        mov     ax, [BV_REQ_WORD_CX]
        mul     bx
        mov     bl, [BV_REQ_H]
        xor     bh, bh
        add     ax, bx
        adc     dx, 0
        mov     bx, 33
        mul     bx
        mov     bl, [BV_REQ_R]
        xor     bh, bh
        add     ax, bx
        adc     dx, 0
        test    dh, 0xe0
        jnz     .fail
        mov     [BV_LBAL], ax
        mov     [BV_LBAH], dx
        clc
        ret
.linear:
        mov     ax, [BV_REQ_WORD_CX]
        mov     dx, [BV_REQ_WORD_DX]
        test    dh, 0xe0
        jnz     .fail
        and     dh, 0x1f
        mov     [BV_LBAL], ax
        mov     [BV_LBAH], dx
        clc
        ret
.fail:  stc
        ret

sasi_reset:
        mov     al, 0x08
        out     SASI_CTRL, al
        JIODLY
        xor     al, al
        out     SASI_CTRL, al
        ret

; Select the target and send a six-byte READ(6) or WRITE(6) command.
sasi_begin:
        call    sasi_reset
        mov     al, 0x40                ; phase/status instead of DIP switches
        out     SASI_CTRL, al
        mov     al, 1                   ; target ID zero
        out     SASI_DATA, al
        mov     al, 0x60                ; assert SEL while status mode remains
        out     SASI_CTRL, al
        call    sasi_wait_busy           ; target accepts selection with BSY
        jc      .fail
        mov     al, 0x40                ; release SEL
        out     SASI_CTRL, al
        mov     ah, 0xa8                ; then wait for command phase
        call    sasi_wait_phase
        jc      .fail

        mov     al, 0x08
        cmp     byte [BV_DIR], 0
        je      .c0
        mov     al, 0x0a
.c0:    call    sasi_put_command
        jc      .fail
        mov     al, [BV_LBAH]
        and     al, 0x1f
        mov     ah, [BV_REQ_DAUA]
        and     ah, 1
        mov     cl, 5
        shl     ah, cl
        or      al, ah
        call    sasi_put_command
        jc      .fail
        mov     al, [BV_LBAL+1]
        call    sasi_put_command
        jc      .fail
        mov     al, [BV_LBAL]
        call    sasi_put_command
        jc      .fail
        mov     al, [BV_SECCNT]
        call    sasi_put_command
        jc      .fail
        xor     al, al
        call    sasi_put_command
        ret
.fail:  stc
        ret

; AL = command byte. Every byte is gated by REQ in command phase.
sasi_put_command:
        push    ax
        mov     ah, 0xa8
        call    sasi_wait_phase
        pop     ax
        jc      .fail
        out     SASI_DATA, al
        clc
        ret
.fail:  stc
        ret

; AH = exact phase bits under mask BCh. Timeout is bounded on missing buses.
sasi_wait_phase:
        push    ax
        push    cx
        xor     cx, cx
.wait:  in      al, SASI_CTRL
        and     al, 0xbc
        cmp     al, ah
        je      .ok
        loop    .wait
        pop     cx
        pop     ax
        stc
        ret
.ok:    pop     cx
        pop     ax
        clc
        ret

sasi_wait_busy:
        push    ax
        push    cx
        xor     cx, cx
.wait:  in      al, SASI_CTRL
        test    al, 0x20
        jnz     .ok
        loop    .wait
        pop     cx
        pop     ax
        stc
        ret
.ok:    pop     cx
        pop     ax
        clc
        ret

sasi_finish:
        push    bx
        mov     ah, 0xac                ; status byte
        call    sasi_wait_phase
        jc      .fail
        in      al, SASI_DATA
        mov     bl, al
        mov     ah, 0xbc                ; command-complete message
        call    sasi_wait_phase
        jc      .fail
        in      al, SASI_DATA
        or      bl, bl
        jnz     .fail
        pop     bx
        clc
        ret
.fail:  pop     bx
        stc
        ret

; Same for the IDE drives (units without a SASI media switch): the equipment bit and nothing else, so a DOS
; booted from a floppy sees the IDE hard disks.  ide_present selects the unit and checks for DRDY; a machine
; without the board reads FFh from the ports.
ide_equip_init:
        push    ax
        push    bx
        push    cx
        xor     bl, bl
.unit:  mov     al, bl
        or      al, 0x80
        mov     [BV_REQ_DAUA], al
        call    sasi_media_type
        jnc     .next                   ; a SASI drive: sasi_equip_init did it
        call    ide_present
        jc      .next
        mov     al, 1
        mov     cl, bl
        shl     al, cl
        or      [W_DISK_EQUIP+1], al
.next:  inc     bl
        cmp     bl, 2
        jb      .unit
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
;  IDE, in LBA. The call names a cylinder, head and sector; they are turned
;  into a block number with the geometry the PC-98 disk BIOS uses.
; ---------------------------------------------------------------------------
ide_operate:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        mov     al, [BV_REQ_CMD]
        and     al, 0x0f
        cmp     al, 0x03
        je      .init
        cmp     al, 0x04
        je      .sense
        cmp     al, 0x07
        je      .ok
        cmp     al, 0x0e
        je      .ok
        cmp     al, 0x05
        je      .write
        cmp     al, 0x06
        je      .read
        cmp     al, 0x01
        je      .read
        cmp     al, 0x02
        je      .read
        mov     ah, 0x40
        jmp     short .out
.ok:    mov     ah, 0x00
        jmp     short .out
.init:  call    ide_present
        jc      .absent
        mov     ax, 0x0100
        test    byte [BV_REQ_DAUA], 1
        jz      .equip
        shl     ax, 1
.equip: or      [W_DISK_EQUIP], ax
        xor     ah, ah
        jmp     short .out
.sense: call    ide_present
        jc      .absent
        mov     ah, 0x0f
        jmp     short .out
.read:  mov     byte [BV_DIR], 0
        call    ide_transfer
        jmp     short .out
.write: mov     byte [BV_DIR], 1
        call    ide_transfer
        jmp     short .out
.absent:
        mov     ah, 0x60
.out:   pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

; Select the requested primary-controller drive and check that it exists.
ide_present:
        push    ax
        push    dx
        mov     dx, IDE_BANK
        xor     al, al
        out     dx, al
        mov     dx, IDE_DRVHD
        mov     al, [BV_REQ_DAUA]
        and     al, 1
        shl     al, 1
        shl     al, 1
        shl     al, 1
        shl     al, 1
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

; Read the drive's native CHS translation through standard ATA IDENTIFY.
; Returns CF clear with cylinders in BV_LBAL, heads in BV_LBAH and sectors
; per track in BV_TMP.  No emulator metadata or host callback is involved.
ide_identify_geometry:
        push    ax
        push    bx
        push    cx
        push    dx
        call    ide_present
        jc      .fail
        mov     word [BV_LBAL], 0
        mov     word [BV_LBAH], 0
        mov     word [BV_TMP], 0
        mov     dx, IDE_DEVCTL
        mov     al, 0x0a                ; nIEN: interrupts disabled (polled), controller active
        out     dx, al
        xor     al, al
        mov     dx, IDE_SECCNT
        out     dx, al
        mov     dx, IDE_SECNUM
        out     dx, al
        mov     dx, IDE_CYLLO
        out     dx, al
        mov     dx, IDE_CYLHI
        out     dx, al
        mov     dx, IDE_STATUS
        mov     al, 0xec                ; IDENTIFY DEVICE
        out     dx, al
        call    ide_drq
        jc      .fail
        mov     dx, IDE_DATA
        xor     bx, bx
        mov     cx, 256
.word: in      ax, dx
        cmp     bx, 1
        jne     .head
        mov     [BV_LBAL], ax
        jmp     short .next
.head: cmp     bx, 3
        jne     .sector
        mov     [BV_LBAH], ax
        jmp     short .next
.sector:
        cmp     bx, 6
        jne     .next
        mov     [BV_TMP], ax
.next: inc     bx
        loop    .word
        call    ide_wait
        jc      .fail
        cmp     word [BV_LBAL], 0
        je      .fail
        cmp     word [BV_LBAH], 1
        jb      .fail
        cmp     word [BV_LBAH], 16
        ja      .fail
        cmp     word [BV_TMP], 1
        jb      .fail
        cmp     word [BV_TMP], 255
        ja      .fail
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.fail: pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; Waits for BSY to drop. CF set if it never does.
ide_wait:
        push    ax
        push    cx
        push    dx
        mov     dx, IDE_STATUS
        xor     cx, cx
.w:     in      al, dx
        test    al, 0x80
        jnz     .next
        jmp     short .ok
.next:
        loop    .w
.fail:
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

; Waits for DRQ. CF set on an error or a timeout.
ide_drq:
        push    ax
        push    cx
        push    dx
        mov     dx, IDE_STATUS
        xor     cx, cx
.w:     in      al, dx
        test    al, 0x01                ; ERR
        jnz     .fail
        test    al, 0x80                ; still busy
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

; Returns AH = status.
ide_transfer:
        call    ide_present
        jc      .fail

        ; PC-98 HDD sector numbers are zero based.  CHS calls use the same
        ; native translation returned by AH=84h; linear calls supply DX:CX.
        test    byte [BV_REQ_DAUA], 0x80
        jz      .linear
        call    ide_identify_geometry
        jc      .fail
        mov     ax, [BV_REQ_WORD_CX]
        mov     bx, [BV_LBAH]
        mul     bx
        or      dx, dx
        jnz     .fail
        mov     bl, [BV_REQ_H]
        xor     bh, bh
        add     ax, bx
        mov     bx, [BV_TMP]
        mul     bx                      ; DX:AX
        mov     bl, [BV_REQ_R]
        xor     bh, bh
        add     ax, bx
        adc     dx, 0
        jmp     short .lba_ready
.linear:
        mov     ax, [BV_REQ_WORD_CX]
        mov     dx, [BV_REQ_WORD_DX]
.lba_ready:
        test    dh, 0xf0                ; ATA-1 LBA is 28 bits
        jnz     .fail
        mov     [BV_LBAL], ax
        mov     [BV_LBAH], dx

        mov     ax, [BV_REQ_LEN]
        or      ax, ax
        jz      .fail
        test    ax, 0x01ff              ; ATA PIO moves complete sectors
        jnz     .fail
        mov     bx, [BV_REQ_OFF]
        add     bx, ax                  ; do not wrap within the caller segment
        jnc     .fits
        or      bx, bx                  ; ...but ending exactly at the 64KB boundary is fine
        jnz     .fail
.fits:  shr     ax, 9
        cmp     ax, 255
        ja      .fail
        mov     [BV_SECCNT], ax

        mov     dx, IDE_DEVCTL
        mov     al, 0x0a                ; nIEN: polled transfer, no interrupt
        out     dx, al
        mov     dx, IDE_SECCNT
        mov     al, [BV_SECCNT]
        out     dx, al
        mov     dx, IDE_SECNUM
        mov     al, [BV_LBAL]
        out     dx, al
        mov     dx, IDE_CYLLO
        mov     al, [BV_LBAL+1]
        out     dx, al
        mov     dx, IDE_CYLHI
        mov     al, [BV_LBAH]
        out     dx, al
        mov     dx, IDE_DRVHD
        mov     al, [BV_LBAH+1]
        and     al, 0x0f
        or      al, 0xe0                ; LBA mode
        mov     ah, [BV_REQ_DAUA]
        and     ah, 0x01                ; unit 0 or 1
        shl     ah, 1
        shl     ah, 1
        shl     ah, 1
        shl     ah, 1
        or      al, ah
        out     dx, al

        mov     dx, IDE_STATUS
        mov     al, 0x20                ; READ SECTORS
        cmp     byte [BV_DIR], 0
        je      .cmd
        mov     al, 0x30                ; WRITE SECTORS
.cmd:   out     dx, al

        mov     es, [BV_REQ_SEG]
        mov     di, [BV_REQ_OFF]
        mov     si, di
        mov     cx, [BV_SECCNT]
.sector:
        push    cx
        call    ide_drq
        jc      .failpop
        mov     cx, 256
        mov     dx, IDE_DATA
        cmp     byte [BV_DIR], 0
        jne     .wr
        rep     insw
        jmp     short .nextsec
.wr:    push    ds
        mov     ax, es
        mov     ds, ax
        rep     outsw
        pop     ds
        mov     di, si
.nextsec:
        pop     cx
        loop    .sector
        call    ide_wait
        jc      .fail
        mov     dx, IDE_STATUS
        in      al, dx
        test    al, 0x01
        jnz     .fail
        mov     ah, 0x00
        ret
.failpop:
        pop     cx
.fail:  mov     ah, 0xd0
        ret

; ---------------------------------------------------------------------------
;  PC-9801-55/L/U compatible SCSI host adapter. The board contains a
;  WD33C93 addressed through base+0/base+2. Transfers below are asynchronous
;  programmed I/O, so neither a host callback nor a board-specific DMA setup
;  is required. Default board bases CC0h, CD0h, CE0h and CF0h are scanned.
; ---------------------------------------------------------------------------

; The conventional PC-9801-55 option window is D2000h.  Initialising a ROM
; found there gives emulators and real adapters that supply their own firmware
; the normal PC-98 path; the native WD33C93 driver remains the fallback.
scsi_option_init:
        push    ax
        push    bx
        push    es
        mov     ax, 0xd200
        mov     es, ax
        cmp     word [es:0x0009], 0xaa55
        jne     .none
        cmp     byte [es:0x000b], 0
        je      .none
        mov     word [BV_SCSI_ROM], 0xd200
        call    0xd200:0x000f
        jmp     short .done
.none:  mov     word [BV_SCSI_ROM], 0
.done:  pop     es
        pop     bx
        pop     ax
        ret

scsi_operate:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        mov     al, [BV_REQ_CMD]
        cmp     al, 0xb0                ; direct CDB, used by EPSON Startup
        je      .direct
        and     al, 0x0f
        cmp     al, 0x03
        je      .init
        cmp     al, 0x04
        je      .sense
        cmp     al, 0x01                ; verify by reading
        je      .read
        cmp     al, 0x02
        je      .read
        cmp     al, 0x05
        je      .write
        cmp     al, 0x06
        je      .read
        cmp     al, 0x07
        je      .ok
        cmp     al, 0x0a                ; requested 512-byte sectors
        je      .ok
        cmp     al, 0x0c                ; no replacement-sector information
        je      .ok
        cmp     al, 0x0f
        je      .ok
        mov     ah, 0x40
        jmp     .out
.init: call     scsi_initialize_devices
        jmp     short .out
.sense:
        call    scsi_test_current
        jc      .absent
        xor     ah, ah
        jmp     short .out
.read:  mov     byte [BV_DIR], 0
        jmp     short .transfer
.write: mov     byte [BV_DIR], 1
        jmp     short .transfer
.direct:
        call    scsi_direct_cdb
        jmp     short .out
.transfer:
        call    scsi_compute_lba
        jc      .bad
        mov     ax, [BV_REQ_LEN]
        or      ax, ax
        jz      .bad
        test    ax, 0x01ff              ; fixed disks use 512-byte blocks
        jnz     .bad
        mov     bx, [BV_REQ_OFF]
        add     bx, ax
        jc      .bad
        mov     cl, 9
        shr     ax, cl
        mov     [BV_SECCNT], ax
        call    scsi_build_rw_cdb
        mov     si, SCSI_CDB_BUF
        mov     es, [BV_REQ_SEG]
        mov     di, [BV_REQ_OFF]
        call    scsi_execute
        jc      .ioerr
.ok:    xor     ah, ah
        jmp     short .out
.bad:   mov     ah, 0xd0
        jmp     short .out
.ioerr: mov     ah, 0xd0
        jmp     short .out
.absent:
        mov     ah, 0x60
.out:   pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

; INT 1Bh AH=B0h direct SCSI command.
;   ES:SI = CDB, DX = CDB length, BX = data length, ES:BP = data.
; EPSON Startup uses INQUIRY, PREVENT/ALLOW MEDIUM REMOVAL and READ CAPACITY.
scsi_direct_cdb:
        mov     cx, [BV_REQ_WORD_DX]
        cmp     cx, 6
        jb      .bad
        cmp     cx, 12
        ja      .bad
        mov     es, [BV_REQ_SEG]
        mov     si, [BV_REQ_WORD_CX]
        mov     di, SCSI_CDB_BUF
        push    cx
.copy: mov     al, [es:si]
        inc     si
        mov     [di], al
        inc     di
        loop    .copy
        pop     cx

        cmp     word [BV_SCSI_ROM], 0
        jne     .option_rom
        mov     al, [SCSI_CDB_BUF]
        cmp     al, 0x12                ; INQUIRY
        je      .data_in
        cmp     al, 0x25                ; READ CAPACITY(10)
        je      .data_in
        cmp     al, 0x1e                ; PREVENT/ALLOW MEDIUM REMOVAL
        jne     .bad
        cmp     word [BV_REQ_LEN], 0
        jne     .bad
        mov     byte [BV_DIR], 0
        jmp     short .execute
.data_in:
        cmp     word [BV_REQ_LEN], 0
        je      .bad
        mov     byte [BV_DIR], 0
.execute:
        mov     si, SCSI_CDB_BUF
        mov     bx, [BV_REQ_LEN]
        mov     es, [BV_REQ_SEG]
        mov     di, [BV_REQ_OFF]
        call    scsi_execute
        jc      .ioerr
        xor     ah, ah
        ret
.option_rom:
        call    scsi_option_direct_cdb
        ret
.bad:  mov     ah, 0x40
        ret
.ioerr:
        mov     ah, 0xd0
        ret

; EPSON Startup asks the SCSI BIOS for only three direct commands.  The
; standard option-ROM disk interface does not expose raw CDBs, so answer them
; from the equipment/geometry table populated by that ROM.  No emulator-only
; port or host callback is used here.
scsi_option_direct_cdb:
        mov     al, [BV_REQ_DAUA]
        and     al, 7
        mov     cl, al
        mov     al, 1
        shl     al, cl
        test    [W_DISK_EQUIPS], al
        jz      .absent

        mov     al, [SCSI_CDB_BUF]
        cmp     al, 0x12                ; INQUIRY
        je      .inquiry
        cmp     al, 0x25                ; READ CAPACITY(10)
        je      .capacity
        cmp     al, 0x1e                ; PREVENT/ALLOW MEDIUM REMOVAL
        jne     .bad
        cmp     word [BV_REQ_LEN], 0
        jne     .bad
        xor     ah, ah
        ret

.inquiry:
        mov     cx, [BV_REQ_LEN]
        or      cx, cx
        jz      .bad
        mov     es, [BV_REQ_SEG]
        mov     di, [BV_REQ_OFF]
        xor     al, al
        rep     stosb                   ; direct-access, non-removable device
        mov     di, [BV_REQ_OFF]
        cmp     word [BV_REQ_LEN], 3
        jb      .success
        mov     byte [es:di+2], 1       ; ANSI SCSI-1
        cmp     word [BV_REQ_LEN], 4
        jb      .success
        mov     byte [es:di+3], 1       ; SCSI-1 response format
        cmp     word [BV_REQ_LEN], 5
        jb      .success
        mov     byte [es:di+4], 31      ; 36-byte standard response
        jmp     short .success

.capacity:
        cmp     word [BV_REQ_LEN], 8
        jb      .bad
        xor     bx, bx
        mov     bl, [BV_REQ_DAUA]
        and     bl, 7
        shl     bx, 1
        shl     bx, 1
        add     bx, W_SCSI_INFO

        xor     ax, ax
        mov     al, [bx]                ; sectors per track
        xor     dx, dx
        mov     dl, [bx+1]
        and     dx, 0x000f              ; heads
        mul     dx
        mov     si, ax                  ; sectors per cylinder
        or      si, si
        jz      .bad

        mov     ax, [bx+2]
        mov     cx, ax
        and     ax, 0x0fff              ; low 12 cylinder bits
        test    cx, 0x4000
        jz      .have_cylinders
        xor     dx, dx
        mov     dl, [bx+1]
        and     dx, 0x00f0              ; high four cylinder bits
        mov     cl, 8
        shl     dx, cl
        add     ax, dx
.have_cylinders:
        or      ax, ax
        jz      .bad
        mul     si                      ; DX:AX = total logical blocks
        sub     ax, 1
        sbb     dx, 0                   ; READ CAPACITY returns last LBA

        mov     es, [BV_REQ_SEG]
        mov     di, [BV_REQ_OFF]
        mov     [es:di], dh
        mov     [es:di+1], dl
        mov     [es:di+2], ah
        mov     [es:di+3], al
        mov     word [es:di+4], 0
        mov     byte [es:di+6], 2       ; 00000200h = 512 bytes/block
        mov     byte [es:di+7], 0
.success:
        xor     ah, ah
        ret
.absent:
        mov     ah, 0x60
        ret
.bad:  mov     ah, 0x40
        ret

; The 55-series BIOS translation is eight heads and seventeen sectors.
scsi_compute_lba:
        mov     al, [BV_REQ_DAUA]
        test    al, 0x80
        jz      .linear
        mov     ax, [BV_REQ_WORD_CX]
        mov     bx, 8
        mul     bx
        mov     bl, [BV_REQ_H]
        xor     bh, bh
        add     ax, bx
        adc     dx, 0
        mov     bx, 17
        mul     bx
        mov     bl, [BV_REQ_R]
        xor     bh, bh
        add     ax, bx
        adc     dx, 0
        mov     [BV_LBAL], ax
        mov     [BV_LBAH], dx
        clc
        ret
.linear:
        mov     ax, [BV_REQ_WORD_CX]
        mov     dx, [BV_REQ_WORD_DX]
        mov     [BV_LBAL], ax
        mov     [BV_LBAH], dx
        clc
        ret

scsi_build_rw_cdb:
        mov     di, SCSI_CDB_BUF
        cmp     byte [BV_LBAH+1], 0
        jne     .cdb10
        test    byte [BV_LBAH], 0xe0
        jnz     .cdb10
        mov     al, 0x08                ; READ(6), accepted by early SCSI-1 disks
        cmp     byte [BV_DIR], 0
        je      .op6
        mov     al, 0x0a                ; WRITE(6)
.op6:  mov     [di], al
        mov     al, [BV_LBAH]
        and     al, 0x1f
        mov     [di+1], al
        mov     al, [BV_LBAL+1]
        mov     [di+2], al
        mov     al, [BV_LBAL]
        mov     [di+3], al
        mov     al, [BV_SECCNT]
        mov     [di+4], al
        mov     byte [di+5], 0
        mov     cl, 6
        ret
.cdb10:
        mov     al, 0x28                ; READ(10)
        cmp     byte [BV_DIR], 0
        je      .opcode
        mov     al, 0x2a                ; WRITE(10)
.opcode:
        mov     [di], al
        mov     byte [di+1], 0
        mov     al, [BV_LBAH+1]
        mov     [di+2], al
        mov     al, [BV_LBAH]
        mov     [di+3], al
        mov     al, [BV_LBAL+1]
        mov     [di+4], al
        mov     al, [BV_LBAL]
        mov     [di+5], al
        mov     byte [di+6], 0
        mov     al, [BV_SECCNT+1]
        mov     [di+7], al
        mov     al, [BV_SECCNT]
        mov     [di+8], al
        mov     byte [di+9], 0
        mov     cl, 10
        ret

; Probe the board once and initialise its WD33C93 in polled asynchronous mode.
scsi_detect:
        cmp     byte [BV_SCSI_READY], 1
        je      .present
        cmp     byte [BV_SCSI_READY], 0xff
        je      .absent
        push    ax
        push    bx
        push    dx
        mov     dx, SCSI_IO_FIRST
.scan: in      al, dx                  ; auxiliary status
        cmp     al, 0xff
        je      .next
        mov     [BV_SCSI_BASE], dx
        mov     al, 0x33                ; board RESENT/configuration latch
        call    scsi_read_reg
        mov     bl, al
        and     al, 0x38
        cmp     al, 0x28                ; six legal IRQ selector values
        ja      .next
        and     bl, 7
        mov     [BV_SCSI_ID], bl
        call    scsi_chip_reset
        jc      .next
        mov     byte [BV_SCSI_READY], 1
        pop     dx
        pop     bx
        pop     ax
.present:
        clc
        ret
.next: add     dx, 0x10
        cmp     dx, SCSI_IO_LAST
        jbe     .scan
        mov     byte [BV_SCSI_READY], 0xff
        pop     dx
        pop     bx
        pop     ax
.absent:
        stc
        ret

scsi_chip_reset:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     dx, [BV_SCSI_BASE]
        add     dx, SCSI_BOARD
        mov     al, 0x02                ; disable board DMA gate
        out     dx, al
        mov     al, 0x30                ; memory-bank/control register
        call    scsi_read_reg
        and     al, 0xf9                ; interrupts off, bus reset released
        mov     bl, al
        or      al, 0x02                ; assert the adapter's SCSI bus reset
        mov     ah, al
        mov     al, 0x30
        call    scsi_write_reg
        mov     cx, 0x1000              ; comfortably exceeds 25 us on a 486
.reset_delay:
        JIODLY
        loop    .reset_delay
        mov     ah, bl                  ; release reset, keep board IRQ off
        mov     al, 0x30
        call    scsi_write_reg
        mov     al, [BV_SCSI_ID]
        or      al, 0x68                ; EAF, RAF, 12-15 MHz clock range
        mov     ah, al
        xor     al, al                  ; OWN ID
        call    scsi_write_reg
        mov     ax, 0x0c01              ; CONTROL = IDI|EDI, polled
        call    scsi_write_reg
        mov     ax, 0x0011              ; asynchronous transfer
        call    scsi_write_reg
        mov     ax, 0x0018              ; RESET command
        call    scsi_write_reg
        call    scsi_wait_interrupt
        jc      .fail
        mov     al, 0x17
        call    scsi_read_reg            ; clear reset interrupt
        cmp     al, 1
        ja      .fail
        mov     ax, 0x1402              ; 200 ms selection timeout
        call    scsi_write_reg
        mov     ax, 0x0c01
        call    scsi_write_reg
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

; Initialise the BIOS SCSI equipment byte and four-byte target descriptors.
scsi_initialize_devices:
        call    scsi_detect
        jc      .absent
        mov     byte [W_DISK_EQUIPS], 0
        push    es
        xor     ax, ax
        mov     es, ax
        mov     di, W_SCSI_INFO
        mov     cx, 0x10
        rep     stosw                    ; clear all eight descriptors
        pop     es
        xor     dl, dl
.loop: mov     al, dl
        or      al, 0xa0
        mov     [BV_REQ_DAUA], al
        call    scsi_test_current
        jc      .next
        mov     al, 1
        mov     cl, dl
        shl     al, cl
        or      [W_DISK_EQUIPS], al
        xor     bx, bx
        mov     bl, dl
        shl     bx, 1
        shl     bx, 1
        mov     byte [bx+W_SCSI_INFO], 17
        mov     byte [bx+W_SCSI_INFO+1], 8
        mov     word [bx+W_SCSI_INFO+2], 0x1fff ; 4095 cyl, 512 bytes
.next: inc     dl
        cmp     dl, 4
        jb      .loop
        xor     ah, ah
        ret
.absent:
        mov     ah, 0x60
        ret

; TEST UNIT READY for the target encoded in BV_REQ_DAUA.
scsi_test_current:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es
        push    word [BV_REQ_LEN]
        xor     ax, ax
        mov     al, [BV_DIR]
        push    ax
        mov     word [BV_REQ_LEN], 0
        mov     byte [BV_DIR], 0
        mov     di, SCSI_CDB_BUF
        xor     ax, ax
        mov     [di], ax
        mov     [di+2], ax
        mov     [di+4], ax
        mov     si, di
        mov     cl, 6
        xor     bx, bx
        xor     ax, ax
        mov     es, ax
        mov     di, SCSI_CDB_BUF+0x20
        call    scsi_execute
        jnc     .success
        cmp     word [BV_TMP], 0xffff   ; selection timeout: no target
        je      .failure

        ; A reset commonly leaves Unit Attention. REQUEST SENSE clears it,
        ; after which TEST UNIT READY is tried once more.
        mov     di, SCSI_CDB_BUF
        xor     ax, ax
        mov     [di], ax
        mov     [di+2], ax
        mov     [di+4], ax
        mov     byte [di], 0x03         ; REQUEST SENSE(6)
        mov     byte [di+4], 18
        mov     word [BV_REQ_LEN], 18
        mov     si, di
        mov     cl, 6
        xor     ax, ax
        mov     es, ax
        mov     di, SCSI_CDB_BUF+0x20
        call    scsi_execute
        mov     di, SCSI_CDB_BUF
        xor     ax, ax
        mov     [di], ax
        mov     [di+2], ax
        mov     [di+4], ax
        mov     word [BV_REQ_LEN], 0
        mov     si, di
        mov     cl, 6
        xor     bx, bx
        call    scsi_execute
        jc      .failure
.success:
        pop     ax
        mov     [BV_DIR], al
        pop     ax
        mov     [BV_REQ_LEN], ax
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.failure:
        pop     ax
        mov     [BV_DIR], al
        pop     ax
        mov     [BV_REQ_LEN], ax
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; DS:SI CDB, CL length, BX data length, ES:DI data, BV_DIR direction.
; CF clear only when transport, message and target status all succeeded.
scsi_execute:
        call    scsi_detect
        jc      .fail
        call    scsi_wait_idle
        jc      .fail
        push    bx
        push    cx
        push    si
        push    di
        mov     ax, 0x000f              ; TARGET LUN zero
        call    scsi_write_reg
        mov     ax, 0x0011              ; asynchronous transfer
        call    scsi_write_reg
        mov     ah, [BV_REQ_DAUA]
        and     ah, 7
        cmp     byte [BV_DIR], 0
        jne     .dstid
        or      ah, 0x40                ; DPD: target-to-initiator data phase
.dstid:
        mov     al, 0x15                ; destination ID
        call    scsi_write_reg
        mov     ax, 0x0016              ; no reselection/disconnect
        call    scsi_write_reg
        mov     ax, 0x0010              ; command phase starts at zero
        call    scsi_write_reg

        mov     dx, [BV_SCSI_BASE]
        mov     al, 0x03                ; first CDB register
        out     dx, al
        add     dx, SCSI_WD_REG
        xor     ch, ch
.cdb:  mov     al, [si]
        inc     si
        out     dx, al
        loop    .cdb
        mov     ax, 0x0012              ; transfer count = zero: stop at data
        call    scsi_write_reg
        mov     ax, 0x0013
        call    scsi_write_reg
        mov     ax, 0x0014
        call    scsi_write_reg
        mov     ax, 0x0c01              ; polled, halt-on-interrupt controls
        call    scsi_write_reg
        mov     ax, 0x0918              ; SELECT WITHOUT ATN AND TRANSFER
        call    scsi_write_reg
        mov     word [BV_TMP], 0xffff   ; target status not read yet

.event: call    scsi_wait_interrupt
        jc      .restore_fail
        mov     al, 0x17
        call    scsi_read_reg            ; status also clears the interrupt
        cmp     al, 0x16                ; select-and-transfer complete
        je      .complete
        cmp     al, 0x42                ; selection timeout
        je      .restore_fail
        mov     ah, al
        and     al, 7
        cmp     al, 1                   ; DATA IN
        je      .data_in
        cmp     al, 0                   ; DATA OUT
        je      .data_out
        cmp     al, 3                   ; STATUS IN
        je      .status
        cmp     al, 7                   ; MESSAGE IN
        je      .message
        jmp     short .restore_fail
.data_in:
        cmp     byte [BV_DIR], 0
        jne     .restore_fail
        call    scsi_pio_data
        jc      .restore_fail
        jmp     short .event
.data_out:
        cmp     byte [BV_DIR], 1
        jne     .restore_fail
        call    scsi_pio_data
        jc      .restore_fail
        jmp     short .event
.status:
        call    scsi_read_one
        jc      .restore_fail
        mov     [BV_TMP], al
        jmp     short .event
.message:
        call    scsi_read_one
        jc      .restore_fail
        or      al, al                  ; COMMAND COMPLETE
        jnz     .restore_fail
        mov     ax, 0x0318              ; negate ACK and allow bus free
        call    scsi_write_reg
        call    scsi_wait_interrupt
        jc      .restore_fail
        mov     al, 0x17
        call    scsi_read_reg
        cmp     al, 0x85                ; disconnected
        jne     .restore_fail
        jmp     short .check_target
.complete:
        cmp     word [BV_TMP], 0xffff
        jne     .check_target
        mov     al, 0x0f                ; target status is latched here
        call    scsi_read_reg
        mov     [BV_TMP], al
.check_target:
        cmp     byte [BV_TMP], 0
        jne     .restore_fail
        pop     di
        pop     si
        pop     cx
        pop     bx
        clc
        ret
.restore_fail:
        pop     di
        pop     si
        pop     cx
        pop     bx
.fail:  stc
        ret

; Move BV_REQ_LEN bytes through WD_DATA using TRANSFER INFORMATION.
scsi_pio_data:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        mov     bx, [BV_REQ_LEN]
        mov     ax, 0x0012              ; high byte of 24-bit count is zero
        call    scsi_write_reg
        mov     ah, [BV_REQ_LEN+1]
        mov     al, 0x13
        call    scsi_write_reg
        mov     ah, [BV_REQ_LEN]
        mov     al, 0x14
        call    scsi_write_reg
        mov     ax, 0x0c01
        call    scsi_write_reg
        mov     ax, 0x2018              ; TRANSFER INFORMATION
        call    scsi_write_reg
        mov     cx, bx
        xor     si, si
.wait: mov     dx, [BV_SCSI_BASE]
        in      al, dx                  ; auxiliary status
        cmp     al, 0xff
        je      .fail
        test    al, 1                   ; data buffer ready
        jz      .maybe_done
        push    dx
        mov     al, 0x19
        out     dx, al
        add     dx, SCSI_WD_REG
        cmp     byte [BV_DIR], 0
        jne     .write
        in      al, dx
        stosb
        jmp     short .moved
.write: mov     al, [es:di]
        inc     di
        out     dx, al
.moved: pop     dx
        xor     si, si                  ; reset the no-progress timeout
        loop    .wait
.drain:
        in      al, dx
        test    al, 0x80
        jnz     .ok                    ; leave the interrupt for event loop
        cmp     al, 0xff
        je      .fail
        dec     si
        jnz     .drain
        jmp     short .fail
.maybe_done:
        test    al, 0x80
        jnz     .ended
        dec     si
        jnz     .wait
        jmp     short .fail
.ended:
        or      cx, cx
        jnz     .fail                  ; target ended the phase too early
.ok:    pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.fail:  pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; Read one status or message byte in single-byte-transfer mode.
scsi_read_one:
        push    bx
        push    cx
        push    dx
        mov     ax, 0x0c01
        call    scsi_write_reg
        mov     ax, 0xa018              ; TRANSFER INFO | SBT
        call    scsi_write_reg
        xor     bx, bx                  ; BL=data, BH=received flag
        xor     cx, cx
.wait: mov     dx, [BV_SCSI_BASE]
        in      al, dx
        cmp     al, 0xff
        je      .fail
        test    al, 1
        jz      .int
        mov     al, 0x19
        out     dx, al
        add     dx, SCSI_WD_REG
        in      al, dx
        mov     bl, al
        mov     bh, 1
        sub     dx, SCSI_WD_REG
.int:  in      al, dx
        test    al, 0x80
        jnz     .done
        loop    .wait
        jmp     short .fail
.done: or      bh, bh
        jz      .fail
        mov     al, bl
        pop     dx
        pop     cx
        pop     bx
        clc
        ret
.fail:  pop     dx
        pop     cx
        pop     bx
        stc
        ret

; AL=register -> AL=value.
scsi_read_reg:
        push    dx
        mov     dx, [BV_SCSI_BASE]
        out     dx, al
        add     dx, SCSI_WD_REG
        in      al, dx
        pop     dx
        ret

; AL=register, AH=value.
scsi_write_reg:
        push    dx
        mov     dx, [BV_SCSI_BASE]
        out     dx, al
        add     dx, SCSI_WD_REG
        mov     al, ah
        out     dx, al
        pop     dx
        ret

scsi_wait_interrupt:
        push    ax
        push    cx
        push    dx
        mov     dx, [BV_SCSI_BASE]
        xor     cx, cx
.wait: in      al, dx
        cmp     al, 0xff
        je      .fail
        test    al, 0x80
        jnz     .ok
        loop    .wait
.fail: pop     dx
        pop     cx
        pop     ax
        stc
        ret
.ok:   pop     dx
        pop     cx
        pop     ax
        clc
        ret

scsi_wait_idle:
        push    ax
        push    cx
        push    dx
        mov     dx, [BV_SCSI_BASE]
        xor     cx, cx
.wait: in      al, dx
        cmp     al, 0xff
        je      .fail
        test    al, 0x30                ; chip busy or command in progress
        jz      .ok
        loop    .wait
.fail: pop     dx
        pop     cx
        pop     ax
        stc
        ret
.ok:   pop     dx
        pop     cx
        pop     ax
        clc
        ret

; ===========================================================================
;  Bootstrap: floppies, SASI, IDE and PC-9801-55 SCSI. This works without a
;  host-side boot callback or an expansion-board option ROM.
; ===========================================================================
boot:
        mov     si, msg_boot
        call    puts

        mov     ch, 0x90                ; 1.25MB or 1.44MB, ports 90h-94h
        call    boot_fds
        jnc     .go
        mov     ch, 0x70                ; 2DD, ports C8h-CCh
        call    boot_fds
        jnc     .go

        xor     cx, cx
.sasiloop:
        mov     al, cl
        push    cx
        call    boot_sasi
        pop     cx
        jnc     .go
        inc     cl
        cmp     cl, 2
        jb      .sasiloop

        xor     cx, cx
.ideloop:
        mov     al, cl
        push    cx
        call    boot_hd
        pop     cx
        jnc     .go
        inc     cl
        cmp     cl, 2
        jb      .ideloop

        xor     cx, cx
.scsiloop:
        mov     al, cl
        push    cx
        call    boot_scsi
        pop     cx
        jnc     .go
        inc     cl
        cmp     cl, 4
        jb      .scsiloop
        jmp     short .none
.go:    jmp     boot_jump               ; AX = segment the sector landed in
.none:  ret

; CH = floppy interface/media class. CF clear and AX = load segment on success.
boot_fds:
        xor     cl, cl
.loop:  mov     al, cl
        mov     ah, ch
        call    boot_fd
        jnc     .ok
        inc     cl
        cmp     cl, 4
        jb      .loop
        stc
        ret
.ok:    clc
        ret

; AH = media class, AL = unit. CF clear and AX = load segment when booted.
boot_fd:
        push    bx
        push    cx
        push    dx
        mov     byte [BV_FDBUSY], 1     ; INT 13h keeps its hands off
        and     al, 0x03
        or      al, ah
        mov     [BV_REQ_DAUA], al
        mov     byte [BV_REQ_CMD], 0x46 ; read, MFM
        mov     byte [BV_REQ_C], 0
        mov     byte [BV_REQ_H], 0
        mov     byte [BV_REQ_R], 1
        mov     byte [BV_REQ_N], 3
        mov     word [BV_REQ_LEN], 1024
        mov     word [BV_REQ_SEG], IPL_SEG1024
        mov     word [BV_REQ_OFF], 0

        call    fdd_setmode
        call    fdc_reset
        mov     al, 0
        call    fdc_seek
        call    fdc_readid              ; what is this disk, if anything?
        jc      .fail

        mov     al, [W_DISK_RESULT+6]   ; N as the disk really is
        mov     [BV_REQ_N], al
        mov     dl, [BV_REQ_DAUA]
        and     dl, 0xf0
        cmp     dl, 0x70
        je      .n2dd
        cmp     al, 3
        je      .n3
        cmp     al, 2
        jne     .fail
        mov     word [BV_REQ_LEN], 512
        mov     word [BV_REQ_SEG], IPL_SEG512
        mov     byte [BV_FDSPT], 18
        mov     byte [BV_FDN], 2
        mov     dl, 0x30                ; 1.44MB
        jmp     short .go
.n3:    mov     byte [BV_FDSPT], 8
        mov     byte [BV_FDN], 3
        mov     dl, 0x90
        jmp     short .go
.n2dd:  cmp     al, 2
        jne     .fail
        mov     word [BV_REQ_LEN], 512
        mov     word [BV_REQ_SEG], IPL_SEG512
        mov     byte [BV_FDSPT], 9
        mov     byte [BV_FDN], 2
.go:    mov     al, [W_DISK_RESULT+3]   ; the cylinder it actually found
        mov     [BV_REQ_C], al
        mov     byte [BV_REQ_R], 1      ; the IPL is sector 1. READ ID only says
                                        ; which sector was passing the head
        mov     byte [BV_DIR], 0
        call    fdd_transfer
        jc      .fail

        mov     al, [BV_REQ_DAUA]
        and     al, 0x03
        or      al, dl
        mov     [W_DISK_BOOT], al       ; the IPL reads this to find itself
        mov     ax, [BV_REQ_SEG]
        mov     byte [BV_FDBUSY], 0
        pop     dx
        pop     cx
        pop     bx
        clc
        ret
.fail:  mov     byte [BV_FDBUSY], 0
        pop     dx
        pop     cx
        pop     bx
        stc
        ret

; AL = SASI unit. Four 256-byte blocks form the PC-98 hard-disk IPL.
boot_sasi:
        and     al, 1
        or      al, 0x80
        mov     [BV_REQ_DAUA], al
        mov     byte [BV_REQ_CMD], 0x06
        mov     word [BV_REQ_LEN], 1024
        mov     word [BV_REQ_WORD_CX], 0
        mov     word [BV_REQ_WORD_DX], 0
        mov     byte [BV_REQ_H], 0
        mov     byte [BV_REQ_R], 0
        mov     word [BV_REQ_SEG], IPL_SEG1024
        mov     word [BV_REQ_OFF], 0
        mov     byte [BV_DIR], 0
        call    sasi_media_type
        jc      .fail
        call    sasi_operate
        or      ah, ah
        jnz     .fail
        mov     al, [BV_REQ_DAUA]
        mov     [W_DISK_BOOT], al
        mov     al, 1
        test    byte [BV_REQ_DAUA], 1
        jz      .equipped
        shl     al, 1
.equipped:
        or      [W_DISK_EQUIP+1], al
        mov     ax, IPL_SEG1024
        clc
        ret
.fail:  stc
        ret

; AL = IDE unit. Load the first 1024 bytes using CHS sector zero.
boot_hd:
        and     al, 1
        or      al, 0x80
        mov     byte [BV_REQ_DAUA], al
        mov     byte [BV_REQ_CMD], 0x06
        mov     word [BV_REQ_LEN], 1024
        mov     word [BV_REQ_WORD_CX], 0
        mov     word [BV_REQ_WORD_DX], 0
        mov     byte [BV_REQ_H], 0
        mov     byte [BV_REQ_R], 0
        mov     byte [BV_REQ_N], 3
        mov     word [BV_REQ_SEG], IPL_SEG1024
        mov     word [BV_REQ_OFF], 0
        mov     byte [BV_DIR], 0
        call    ide_transfer
        cmp     ah, 0
        jne     .fail
        mov     al, [BV_REQ_DAUA]
        mov     byte [W_DISK_BOOT], al
        mov     al, 1                   ; EPSON IPL checks 0000:055Dh directly
        test    byte [BV_REQ_DAUA], 1
        jz      .equipped
        shl     al, 1
.equipped:
        or      [W_DISK_EQUIP+1], al
        mov     ax, IPL_SEG1024
        clc
        ret
.fail:  stc
        ret

; AL = SCSI target ID. Two 512-byte blocks form the hard-disk IPL.
boot_scsi:
        and     al, 3
        or      al, 0xa0
        mov     [BV_REQ_DAUA], al
        cmp     word [BV_SCSI_ROM], 0
        je      .native

        ; Read through the installed PC-9801-55 option BIOS. This is the
        ; standard board-firmware interface and covers virtual adapters whose
        ; WD33C93 model does not implement disk READ at register level.
        push    bp
        push    es
        mov     ax, IPL_SEG1024
        mov     es, ax
        mov     ax, 0x46a0              ; linear read, SCSI target below
        mov     al, [BV_REQ_DAUA]
        mov     bx, 1024
        xor     cx, cx
        xor     dx, dx
        xor     bp, bp
        int     0x1b
        pop     es
        pop     bp
        jc      .fail
        jmp     short .loaded

.native:
        mov     byte [BV_REQ_CMD], 0x06
        mov     word [BV_REQ_LEN], 1024
        mov     word [BV_REQ_WORD_CX], 0
        mov     word [BV_REQ_WORD_DX], 0
        mov     byte [BV_REQ_H], 0
        mov     byte [BV_REQ_R], 0
        mov     word [BV_REQ_SEG], IPL_SEG1024
        mov     word [BV_REQ_OFF], 0
        mov     byte [BV_DIR], 0
        call    scsi_operate
        or      ah, ah
        jnz     .fail
.loaded:
        mov     al, [BV_REQ_DAUA]
        and     al, 7
        mov     cl, al
        mov     ah, 1
        shl     ah, cl
        or      [W_DISK_EQUIPS], ah
        cmp     word [BV_SCSI_ROM], 0
        jne     .descriptor_ready       ; option ROM supplied real geometry
        xor     ah, ah
        mov     bx, ax
        shl     bx, 1
        shl     bx, 1
        mov     byte [bx+W_SCSI_INFO], 17
        mov     byte [bx+W_SCSI_INFO+1], 8
        mov     word [bx+W_SCSI_INFO+2], 0x1fff
.descriptor_ready:
        mov     al, [BV_REQ_DAUA]
        mov     [W_DISK_BOOT], al
        mov     ax, IPL_SEG1024
        clc
        ret
.fail:  stc
        ret

; AX = the segment the boot sector was read into.
boot_jump:
        mov     bx, ax
        xor     ax, ax
        mov     ds, ax
        call    cursor_off              ; the IPL gets the screen with no
        xor     ax, ax                  ; cursor, as from NEC's BIOS
        mov     es, ax
        mov     ss, ax
        mov     sp, STACK_TOP
        push    bx                      ; the far return address the IPL uses
        push    ax
        sti
        retf

; ===========================================================================
;  Tables
; ===========================================================================

; PC-98 SASI media switch values zero through six. All have 33 sectors;
; only the number of heads changes between the standard capacity classes.
sasi_heads:
        db 4, 4, 6, 8, 4, 6, 8

; Where each INT 18h function goes. Zero means it is not implemented.
int18_table:
        dw i18_00, i18_01, i18_02, i18_03, i18_04, i18_05, 0, 0
        dw 0, 0, i18_0a, i18_0b, i18_0c, i18_0d, i18_0e, i18_0f
        dw i18_10, i18_11, i18_12, i18_13, i18_14, 0, i18_16, i18_17
        dw i18_18, 0, 0, 0, 0, 0, 0, 0
        times (0x40 - 0x20) dw 0
        dw i18_40, i18_41, i18_42, i18_43, i18_44, 0, 0, i18_47
        dw i18_48, i18_49, i18_4a
        times (0x50 - 0x4b) dw 0

; Vector number, then the offset in this ROM that goes into it.
vectors:
        db 0x08
        dw int08
        db 0x09
        dw int09
        db 0x0a
        dw irq_master_eoi
        db 0x0b
        dw irq_master_eoi
        db 0x0c
        dw irq_master_eoi
        db 0x0d
        dw irq_master_eoi
        db 0x0e
        dw irq_master_eoi
        db 0x0f
        dw irq_master_eoi
        db 0x10
        dw irq_slave_eoi
        db 0x11
        dw irq_sasi
        db 0x12
        dw irq_slave_eoi
        db 0x13
        dw int13
        db 0x14
        dw irq_slave_eoi
        db 0x15
        dw irq_slave_eoi
        db 0x16
        dw irq_slave_eoi
        db 0x17
        dw irq_slave_eoi
        db 0x18
        dw int18
        db 0x1b
        dw int1b
        db 0x1a
        dw int1a
        db 0x1c
        dw int1c
        db 0x1f
        dw int1f
vectors_end:

; Mode flip-flop 1, written a bit at a time: (bit << 1) | value. Bits 3 and
; 7 end up set, which is what a machine sitting at a DOS prompt reads back.
crtc_400_25:
        db 0x00, 0x0f, 0x10, 0x00, 0x01, 0x00
gdc_mode1:
        db 0x00, 0x02, 0x04, 0x07, 0x08, 0x0a, 0x0c, 0x0f
gdc_mode2:
        db 0x00, 0x02, 0x04, 0x06

; 640x400, 24.83kHz.
gdc_tsync:
        db 0x10, 0x4e, 0x07, 0x25, 0x07, 0x07, 0x90, 0x65
gdc_gsync:
        db 0x06, 0x26, 0x03, 0x11, 0x83, 0x07, 0x90, 0x65
; Cursor: displayed, 16 raster lines a row, filling the cell.
gdc_csrform:
        db 0x8f, 0x20, 0x7b
; Display area 0 starts at the top of VRAM and is 400 raster lines tall.
gdc_tpram:                              ; SAD 0, LEN 400: LEN's low 4 bits
        db 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00  ; in byte 2, the rest in 3
gdc_gpram:
        db 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00

; Memory switches: boot off the 1MB floppy interface, 80x25 text.
msw_default:
        db 0x48, 0x05, 0x04, 0x00, 0x01, 0x00, 0x00, 0x6e

; The format parameter blocks the work area points at: sector size code,
; sectors per track, gap length, gap for format, filler, step rate.
; NEC's floppy parameter tables, which 0:05F8 (2HD) and 0:05CC (2DD) point
; at: one table offset per unit, each table eight bytes per N = 0-3:
; MFM read/write EOT, GPL, MFM format SC, GPL, then the same for FM.
; Software (MAKO-DOS for one) puts its own here for formats of its own.
fdfmt_2hd:
        dw fdp_2hd, fdp_2hd, fdp_2hd, fdp_2hd
fdp_2hd:
        db 0x00, 0x00, 0x00, 0x00, 0x1a, 0x07, 0x1a, 0x1b
        db 0x1a, 0x0e, 0x1a, 0x36, 0x0f, 0x0e, 0x0f, 0x2a
        db 0x0f, 0x1b, 0x0f, 0x50, 0x08, 0x1b, 0x08, 0x3a
        db 0x08, 0x35, 0x08, 0x74, 0x00, 0x00, 0x00, 0x00
fdfmt_2dd:
        dw fdp_2dd, fdp_2dd, fdp_2dd, fdp_2dd
fdp_2dd:
        db 0x00, 0x00, 0x00, 0x00, 0x10, 0x07, 0x10, 0x1b
        db 0x10, 0x0e, 0x10, 0x36, 0x09, 0x0e, 0x09, 0x2a
        db 0x09, 0x2a, 0x09, 0x50, 0x05, 0x1b, 0x05, 0x3a
        db 0x05, 0x35, 0x05, 0x74, 0x00, 0x00, 0x00, 0x00

msg_boot:
        db 'Booting', 0x0d, 0x0a, 0
msg_nosystem:
        db 0x0d, 0x0a, 'No system disk. Insert one and reset.', 0x0d, 0x0a, 0

%include "keytables.inc"


; ---------------------------------------------------------------------------
; Signature, reset vector, checksum.
; ---------------------------------------------------------------------------
        times 0x7fc0 - ($ - $$) db 0xff
signature:
        db 'Compatible BIOS by Codex'

        times 0x7ff0 - ($ - $$) db 0xff
        jmp     ROM_SEG:entry

        times (0x8000 - 1) - ($ - $$) db 0xff
        db 0x00                         ; build.sh writes the checksum here

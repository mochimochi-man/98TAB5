Sources for the two ROMs built into the firmware (main/rom/).

PC98N.ROM              <- pc98_bios_native/pc98n.asm, via its build.sh
    The PC-98 compatible BIOS, and the one that does the work: 8,663 bytes of
    code at F8000h that bring a machine up to the DOS prompt from a floppy and
    from a hard disk on its own. Needs nasm and python3; build.sh writes
    PC98N.ROM, which is main/rom/PC98N.ROM byte for byte (sha256
    369472ae8ffe322530d985a520de89d82c9ba25eb262516a70bad13a14884119), and
    verify_rom.py checks the size, the signature and the reset vector before it
    will hand one over.

    Checked against a genuine EPSON BIOS.ROM before it was adopted: not one run
    of 16 bytes or longer is common to the two images, the only printable
    strings they share are 'PSQRVW' and '_^ZY[X' (which are what a PUSH and a
    POP of every register look like as text, and unavoidable in any x86 code),
    and it carries no NEC or EPSON copyright notice - only its own,
    "Compatible BIOS by Codex", at the offset a real machine puts its vendor
    string.

FONT_ESP.ROM           <- mkfont_esp.py --sym  (FONT_ESP.new.ROM, adopted as main/rom/FONT_ESP.ROM)
    A PC-98 FONT.ROM (T98-Next layout) built from the Shinonome bitmap fonts,
    which are public domain. Needs pcf2bdf and the xfonts-shinonome package.
    With --sym, mkfont_pc98sym.py then adds the PC-98 specific symbols, drawn
    from scratch on an 8x16 grid (lines, rectangles, circles, implicit
    curves; 8x8 faces are made from the 8x16 ones): the half-width graphics
    ANK 80h-9Fh / E0h-F7h / FCh, and the two-byte half-width characters 29xx
    (= ANK 21h-7Eh) and 2A21h-2A5Fh (= ANK A1h-DFh). The yen / year / month /
    day / hour / minute / second characters (ANK F1h-F7h) are reduced copies
    of the Shinonome 16x16 kanji. No other emulator's font data and no NEC
    FONT.ROM was used to design any glyph. Without --sym the output is the
    plain Shinonome ROM (the previous main/rom/FONT_ESP.ROM).
    Not in the ROM (blank): 2A60h-2A7Eh, 2Bxx and 2C24h-2C6Fh; video/font.c
    fills exactly those ranges (and any other range found blank) at load time.

Neither contains any NEC code or data, which is what makes the firmware image
redistributable on its own. See LICENSE.md section 3.

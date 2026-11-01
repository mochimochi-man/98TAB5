#!/usr/bin/env python3
"""PC-98 specific symbols for FONT_ESP.ROM, drawn from scratch (clean room).

Nothing here is copied from, or traced over, NEC's FONT.ROM or any other emulator's font data.
Every glyph is drawn with lines, rectangles, circles and a few implicit curves on
an 8x16 pixel grid (x = 0..7 left to right, y = 0..15 top to bottom).

Which code holds which symbol comes from public tables (see MIGRATION_LOG,
"stage 5-2b (first half)"):
  * ANK 80h-9Fh / E0h-FFh: Linux/98 console table pc9800.uni
    (lkml.indiana.edu/0303.1/0167.html), cross-checked with the PC-8001 graph
    character table on ja.wikipedia "Graph key".
  * 2-byte half-width JIS 29xx (= ANK 21h-7Eh) and 2A21h-2A5Fh (= ANK A1h-DFh):
    ku 9 / ku 10 of the PC-98 extension (ja.wikipedia / wdic "98 characters":
    half-width 2-byte = Shift-JIS 8540h-869Eh).

Not done (the layout of these could not be settled from public text):
  * 2Bxx (rest of the half-width 2-byte area) and 2C24h-2C6Fh (98 ruled lines).

Geometry conventions (so that neighbouring cells join without a gap):
  vertical stroke   : column x = 4 (mask 08h) over all 16 rows
  horizontal stroke : row y = 7 over all 8 columns
  double horizontal : rows y = 5 and y = 10
  block elements    : 1/8 steps (2 rows of 16, 1 column of 8)
"""

import math

W, H = 8, 16
VX = 4          # vertical stroke column
HY = 7          # horizontal stroke row
DY = (5, 10)    # double horizontal rows


class Cv:
    def __init__(self):
        self.p = [[0] * W for _ in range(H)]

    def set(self, x, y, v=1):
        if 0 <= x < W and 0 <= y < H:
            self.p[y][x] = v

    def hline(self, y, x0, x1):
        for x in range(x0, x1 + 1):
            self.set(x, y)

    def vline(self, x, y0, y1):
        for y in range(y0, y1 + 1):
            self.set(x, y)

    def rect(self, x0, y0, x1, y1, fill=True):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if fill or x in (x0, x1) or y in (y0, y1):
                    self.set(x, y)

    def line(self, x0, y0, x1, y1):
        n = max(abs(x1 - x0), abs(y1 - y0), 1)
        for i in range(n + 1):
            self.set(round(x0 + (x1 - x0) * i / n), round(y0 + (y1 - y0) * i / n))

    def arc(self, cx, cy, r, a0, a1):
        n = 64
        for i in range(n + 1):
            a = math.radians(a0 + (a1 - a0) * i / n)
            self.set(round(cx + r * math.cos(a)), round(cy + r * math.sin(a)))

    def fill_if(self, f):
        for y in range(H):
            for x in range(W):
                if f(x + 0.5, y + 0.5):
                    self.p[y][x] = 1

    def rows(self):
        return [sum(1 << (7 - x) for x in range(W) if self.p[y][x]) for y in range(H)]


# ---------------------------------------------------------------- pieces
def lines(up=0, down=0, left=0, right=0):
    c = Cv()
    if up:
        c.vline(VX, 0, HY)
    if down:
        c.vline(VX, HY, H - 1)
    if left:
        c.hline(HY, 0, VX)
    if right:
        c.hline(HY, VX, W - 1)
    return c


def lower(n):          # lower n/8 block
    c = Cv()
    c.rect(0, H - 2 * n, W - 1, H - 1)
    return c


def left_blk(n):       # left n/8 block
    c = Cv()
    c.rect(0, 0, n - 1, H - 1)
    return c


def corner_arc(down, right):
    """Rounded corner: stroke leaves the centre towards (down?, right?)."""
    c = Cv()
    r = 3
    sx = 1 if right else -1
    sy = 1 if down else -1
    # arc centre sits diagonally away from the corner so both ends land on the strokes
    cx = VX + sx * r
    cy = HY + sy * r
    # corner point at (VX, HY); arc from (VX, cy) to (cx, HY)
    a_start = math.degrees(math.atan2(0, -sx))        # direction from centre to (VX, cy)
    a_end = math.degrees(math.atan2(-sy, 0))          # direction from centre to (cx, HY)
    # sweep the short way (90 degrees)
    d = (a_end - a_start + 540) % 360 - 180
    c.arc(cx, cy, r, a_start, a_start + d)
    if down:
        c.vline(VX, cy, H - 1)
    else:
        c.vline(VX, 0, cy)
    if right:
        c.hline(HY, cx, W - 1)
    else:
        c.hline(HY, 0, cx)
    return c


def tri(kind):
    c = Cv()
    if kind == "lr":
        c.fill_if(lambda x, y: y / H >= 1 - x / W)
    elif kind == "ll":
        c.fill_if(lambda x, y: y / H >= x / W)
    elif kind == "ur":
        c.fill_if(lambda x, y: y / H <= x / W)
    else:
        c.fill_if(lambda x, y: y / H <= 1 - x / W)
    return c


def suit(kind):
    """Card suits, 8 pixels wide, rows 4..11."""
    c = Cv()
    cx, cy = 3.5, 7.5

    def heart(u, v):             # u,v in about -1.3..1.3, v up
        return (u * u + v * v - 1) ** 3 - u * u * v ** 3 <= 0

    if kind == "heart":
        c.fill_if(lambda x, y: heart((x - cx) / 3.6, -(y - cy - 0.2) / 3.3 + 0.0))
    elif kind == "diamond":
        c.fill_if(lambda x, y: abs(x - cx) / 3.9 + abs(y - cy) / 4.6 <= 1)
    elif kind == "spade":
        c.fill_if(lambda x, y: heart((x - cx) / 3.6, (y - cy + 0.8) / 3.3) and y < 11.0)
        c.rect(3, 10, 4, 11)
        c.line(2, 12, 2, 12)
        c.hline(12, 2, 5)
    elif kind == "club":
        for (px, py, r) in ((3.5, 5.3, 2.0), (1.8, 8.2, 2.0), (5.2, 8.2, 2.0)):
            c.fill_if(lambda x, y, px=px, py=py, r=r: (x - px) ** 2 + (y - py) ** 2 <= r * r)
        c.rect(3, 9, 4, 11)
        c.hline(12, 2, 5)
    return c


def disc(ring):
    c = Cv()
    cx, cy, r = 4.0, 8.0, 3.5
    if ring:
        c.fill_if(lambda x, y: (r - 1.0) ** 2 <= (x - cx) ** 2 + (y - cy) ** 2 <= r * r)
    else:
        c.fill_if(lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r)
    return c


def diag(kind):
    c = Cv()
    if kind in ("slash", "cross"):
        c.line(W - 1, 0, 0, H - 1)
    if kind in ("back", "cross"):
        c.line(0, 0, W - 1, H - 1)
    return c


def backslash():
    c = Cv()
    c.line(1, 2, 6, 13)
    return c



class Art:
    """Glyph given as strings ('#' = on); optional separate 8x8 form."""
    def __init__(self, a16, a8=None, top16=4):
        self.a16 = a16; self.a8 = a8; self.top16 = top16

    def rows(self):
        r = [0] * H
        for i, line in enumerate(self.a16):
            r[self.top16 + i] = int(line.replace("#", "1").replace(".", "0"), 2)
        return r

    def rows8(self):
        if self.a8 is None:
            return None
        r = [int(l.replace("#", "1").replace(".", "0"), 2) for l in self.a8]
        return r + [0] * (8 - len(r))


HEART = ["..#..#..".replace("..#..#..", ".##..##."), "########", "########", "########", "########",
         ".######.", "..####..", "...##..."]
HEART8 = [".##..##.", "########", "########", "########", ".######.", "..####..", "...##..."]
DIAMOND = ["...##...", "..####..", ".######.", "########", "########", ".######.", "..####..", "...##..."]
DIAMOND8 = list(DIAMOND)
SPADE = ["...##...", "..####..", ".######.", "########", "########", "########", "...##...", "..####.."]
SPADE8 = ["...##...", "..####..", ".######.", "########", "########", "...##...", "..####.."]
CLUB = ["..####..", "..####..", "..####..", "##.##.##", "########", "########", "...##...", "..####.."]
CLUB8 = ["..####..", "..####..", "##.##.##", "########", "...##...", "..####.."]

# ---------------------------------------------------------------- tables
def ank_table():
    """code -> (name, Cv)  for 80h-9Fh and E0h-FFh that have a symbol."""
    t = {}
    for n in range(7):
        t[0x80 + n] = ("lower %d/8" % (n + 1), lower(n + 1))
    t[0x87] = ("full block", lower(8))
    for n in range(7):                       # 88h left 1/8 ... 8Eh left 7/8
        t[0x88 + n] = ("left %d/8" % (n + 1), left_blk(n + 1))
    t[0x8F] = ("cross", lines(1, 1, 1, 1))
    t[0x90] = ("up-tee", lines(1, 0, 1, 1))
    t[0x91] = ("down-tee", lines(0, 1, 1, 1))
    t[0x92] = ("left-tee", lines(1, 1, 1, 0))
    t[0x93] = ("right-tee", lines(1, 1, 0, 1))
    c = Cv(); c.hline(0, 0, 7); c.hline(1, 0, 7)
    t[0x94] = ("overline", c)
    t[0x95] = ("horizontal", lines(0, 0, 1, 1))
    t[0x96] = ("vertical", lines(1, 1, 0, 0))
    c = Cv(); c.vline(7, 0, 15)
    t[0x97] = ("right 1/8", c)
    t[0x98] = ("down-right", lines(0, 1, 0, 1))
    t[0x99] = ("down-left", lines(0, 1, 1, 0))
    t[0x9A] = ("up-right", lines(1, 0, 0, 1))
    t[0x9B] = ("up-left", lines(1, 0, 1, 0))
    t[0x9C] = ("arc down-right", corner_arc(True, True))
    t[0x9D] = ("arc down-left", corner_arc(True, False))
    t[0x9E] = ("arc up-right", corner_arc(False, True))
    t[0x9F] = ("arc up-left", corner_arc(False, False))
    # E0h
    c = Cv(); [c.hline(y, 0, 7) for y in DY]
    t[0xE0] = ("double horizontal", c)
    c = lines(1, 1, 0, 0); [c.hline(y, VX, 7) for y in DY]
    t[0xE1] = ("vert + right double", c)
    c = lines(1, 1, 0, 0); [c.hline(y, 0, 7) for y in DY]
    t[0xE2] = ("vert + horiz double", c)
    c = lines(1, 1, 0, 0); [c.hline(y, 0, VX) for y in DY]
    t[0xE3] = ("vert + left double", c)
    t[0xE4] = ("tri lower-right", tri("lr"))
    t[0xE5] = ("tri lower-left", tri("ll"))
    t[0xE6] = ("tri upper-right", tri("ur"))
    t[0xE7] = ("tri upper-left", tri("ul"))
    t[0xE8] = ("spade", Art(SPADE, SPADE8))
    t[0xE9] = ("heart", Art(HEART, HEART8))
    t[0xEA] = ("diamond", Art(DIAMOND, DIAMOND8))
    t[0xEB] = ("club", Art(CLUB, CLUB8))
    cv = disc(False); cv.rows8 = lambda: [int(l.replace("#", "1").replace(".", "0"), 2) for l in
        ["..####..", ".######.", "########", "########", "########", "########", ".######.", "..####.."]]
    t[0xEC] = ("black circle", cv)
    cv = disc(True); cv.rows8 = lambda: [int(l.replace("#", "1").replace(".", "0"), 2) for l in
        ["..####..", ".#....#.", "#......#", "#......#", "#......#", "#......#", ".#....#.", "..####.."]]
    t[0xED] = ("white circle", cv)
    t[0xEE] = ("slash", diag("slash"))
    t[0xEF] = ("backslash line", diag("back"))
    t[0xF0] = ("cross", diag("cross"))
    t[0xFC] = ("backslash", backslash())
    return t



# ---- 円年月日時分秒 (F1h-F7h): shrunk from the Shinonome 16x16 kanji (public domain) that
# mkfont_esp.py has already written into the ROM.  No NEC or other emulator's data is involved.
KANJI7 = [(0xF1, "円"), (0xF2, "年"), (0xF3, "月"), (0xF4, "日"), (0xF5, "時"), (0xF6, "分"), (0xF7, "秒")]

# How each glyph is shrunk to 8 columns (8x16 face).  Rule: OR of the two columns of each pair
# ("or").  Hand-picked exceptions, listed here as the only per-glyph differences:
#   "even": take only the even column of each pair, "odd": only the odd one.
COLMODE = {}      # none needed: plain OR of column pairs was the most readable for all seven
# (a seam-merging variant was tried and was worse for 時 分 秒)
# For the 8x8 face: 2x2 OR ("or"); exceptions by "even"/"odd" for the columns only.
COLMODE8 = {}


def kanji16(rom, ch):
    e = ch.encode("euc_jp")
    ku, ten = e[0] - 0xA0, e[1] - 0xA0
    base = 0x1800 + (ku - 1) * 0xC00 + ten * 32
    return [(rom[base + j] << 8) | rom[base + 16 + j] for j in range(16)]


def shrink_cols(rows16, mode):
    out = []
    for r in rows16:
        v = 0
        for i in range(8):
            a = (r >> (15 - 2 * i)) & 1
            b = (r >> (14 - 2 * i)) & 1
            bit = {"or": a | b, "even": a, "odd": b}[mode]
            v |= bit << (7 - i)
        out.append(v)
    return out


def kanji_glyphs(rom, code, ch):
    g16 = kanji16(rom, ch)
    r16 = shrink_cols(g16, COLMODE.get(code, "or"))
    c8 = shrink_cols(g16, COLMODE8.get(code, "or"))
    r8 = [c8[2 * i] | c8[2 * i + 1] for i in range(8)]
    return r16, r8


def ank8(rows16):
    """8x8 from 8x16: OR of row pairs, same rule mkfont_esp.py uses for the Shinonome ANK."""
    return [rows16[i * 2] | rows16[i * 2 + 1] for i in range(8)]


def half_from_ank(g16):
    """2-byte half-width left half from an ANK glyph: a real CG ROM keeps the leftmost
    column empty, so a glyph that uses it and leaves the rightmost free moves right by 1."""
    used = 0
    for r in g16:
        used |= r
    if (used & 0x80) and not (used & 0x01):
        return [r >> 1 for r in g16]
    return list(g16)


def apply(rom):
    """Add the glyphs to a bytearray FONT.ROM (T98-Next layout). Returns counts."""
    n_ank = 0
    for code, (_, cv) in ank_table().items():
        r = cv.rows()
        off = 0x0800 + code * 16 if code < 0x80 else 0x1000 + (code - 0x80) * 16
        rom[off:off + 16] = bytes(r)
        r8 = cv.rows8() if hasattr(cv, 'rows8') else None
        rom[code * 8:code * 8 + 8] = bytes(r8 if r8 else ank8(r))
        n_ank += 1

    for code, ch in KANJI7:
        r16, r8 = kanji_glyphs(rom, code, ch)
        rom[0x1000 + (code - 0x80) * 16:0x1000 + (code - 0x80) * 16 + 16] = bytes(r16)
        rom[code * 8:code * 8 + 8] = bytes(r8)
        n_ank += 1

    def ank16(c):
        off = 0x0800 + c * 16 if c < 0x80 else 0x1000 + (c - 0x80) * 16
        return list(rom[off:off + 16])

    def put_half(ku, ten, g):
        off = 0x1800 + (ku - 1) * 0xC00 + (ten - 0x20) * 32   # slot = JIS byte - 20h
        rom[off:off + 16] = bytes(half_from_ank(g))
        rom[off + 16:off + 32] = bytes(16)

    n_half = 0
    for ten in range(0x21, 0x7F):
        put_half(9, ten, ank16(ten)); n_half += 1
    for ten in range(0x21, 0x60):
        put_half(10, ten, ank16(ten + 0x80)); n_half += 1
    return n_ank, n_half

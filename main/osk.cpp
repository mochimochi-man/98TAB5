// The on-screen keyboard: a PC-98 keyboard drawn under the picture while the
// panel is in portrait (lcd_mipi.cpp, lcd_set_osk). Touches on it become key
// events through the same queue a USB or Bluetooth keyboard feeds.
//
// SHIFT, CTRL and GRPH latch: a tap holds the key down until the next key is
// released (or the same key is tapped again). CAPS and KANA lock like the real
// keys. MENU opens the built-in menu.

#include <math.h>
#include <string.h>

#include "esp_cache.h"

extern "C" {
#include <compiler.h>
#include <memmap.h>
#include <video.h>
extern UINT8 mem[];
}

#include "board_pins.h"
#include "osk.h"

extern "C" void hid_key_event(uint8_t nkey, uint8_t down);   // usb_kbd.cpp
extern volatile int g_menu_req, g_menu_active;              // usb_kbd.cpp

#define PAN_W BRD_LCD_H_RES
#define PAN_H BRD_LCD_V_RES

#define K_MENU 0xfe

typedef struct {
    const char *label;
    uint8_t nkey;
    uint8_t w;          // width in quarter units
} KeyDef;

typedef struct {
    int16_t x0, y0, x1, y1;
    const char *label;
    uint8_t nkey;
} Key;

#define ROW(...) { __VA_ARGS__, { NULL, 0, 0 } }

static const KeyDef r1[] = ROW(
    { "f1", 0x62, 4 }, { "f2", 0x63, 4 }, { "f3", 0x64, 4 }, { "f4", 0x65, 4 }, { "f5", 0x66, 4 },
    { "f6", 0x67, 4 }, { "f7", 0x68, 4 }, { "f8", 0x69, 4 }, { "f9", 0x6a, 4 }, { "f10", 0x6b, 4 });
static const KeyDef r2[] = ROW(
    { "1", 0x01, 4 }, { "2", 0x02, 4 }, { "3", 0x03, 4 }, { "4", 0x04, 4 }, { "5", 0x05, 4 },
    { "6", 0x06, 4 }, { "7", 0x07, 4 }, { "8", 0x08, 4 }, { "9", 0x09, 4 }, { "0", 0x0a, 4 }, { "-", 0x0b, 4 },
    { "^", 0x0c, 4 }, { "\\", 0x0d, 4 });
static const KeyDef r3[] = ROW(
    { "Q", 0x10, 4 }, { "W", 0x11, 4 }, { "E", 0x12, 4 }, { "R", 0x13, 4 }, { "T", 0x14, 4 },
    { "Y", 0x15, 4 }, { "U", 0x16, 4 }, { "I", 0x17, 4 }, { "O", 0x18, 4 }, { "P", 0x19, 4 }, { "@", 0x1a, 4 },
    { "[", 0x1b, 4 });
static const KeyDef r4[] = ROW(
    { "A", 0x1d, 4 }, { "S", 0x1e, 4 }, { "D", 0x1f, 4 }, { "F", 0x20, 4 }, { "G", 0x21, 4 },
    { "H", 0x22, 4 }, { "J", 0x23, 4 }, { "K", 0x24, 4 }, { "L", 0x25, 4 }, { ";", 0x26, 4 }, { ":", 0x27, 4 },
    { "]", 0x28, 4 });
static const KeyDef r5[] = ROW(
    { "Z", 0x29, 4 }, { "X", 0x2a, 4 }, { "C", 0x2b, 4 }, { "V", 0x2c, 4 }, { "B", 0x2d, 4 },
    { "N", 0x2e, 4 }, { "M", 0x2f, 4 }, { ",", 0x30, 4 }, { ".", 0x31, 4 }, { "/", 0x32, 4 }, { "_", 0x33, 4 });
static const KeyDef r6[] = ROW(
    { "\xb6\xc5", 0x72, 5 }, { "GRPH", 0x73, 5 }, { "NFER", 0x51, 5 }, { "", 0x34, 16 },
    { "XFER", 0x35, 5 });
static const KeyDef *const s_rows[] = { r1, r2, r3, r4, r5, r6 };
#define NROWS ((int)(sizeof(s_rows) / sizeof(s_rows[0])))

// Under the rows, five tiers of a row's height. The first lines up with the columns below it: STOP and COPY,
// RUP and RDWN, then HOME, HELP, - and /. Under it: on the left ESC, TAB, CTRL and CAPS, the two SHIFTs; in the
// middle INS and DEL over the cursor keys - up, then left and right, then down; then the numeric keypad and its * + =, and MENU and BS over RETURN.
typedef struct { const char *label; uint8_t nkey; uint8_t tier, col, cols; } BlockKey;
#define LEFT_W  200     // two keys as wide as XFER
#define PAD_W   210
#define CUR_W   130
#define OP_W    70
#define RET_W   (PAN_W - LEFT_W - PAD_W - CUR_W - OP_W)
static const BlockKey s_top_left[] = { { "STOP", 0x60, 0, 0, 1 }, { "COPY", 0x61, 0, 1, 1 } };
static const BlockKey s_top_cur[] = { { "RUP", 0x36, 0, 0, 1 }, { "RDWN", 0x37, 0, 1, 1 } };
static const BlockKey s_top_pad[] = {
    { "HOME", 0x3e, 0, 0, 1 }, { "HELP", 0x3f, 0, 1, 1 }, { "-", 0x40, 0, 2, 1 }, { "/", 0x41, 0, 3, 1 },
};
static const BlockKey s_left[] = {      // halves of LEFT_W
    { "ESC", 0x00, 0, 0, 2 }, { "TAB", 0x0f, 1, 0, 2 }, { "CTRL", 0x74, 2, 0, 1 }, { "CAPS", 0x71, 2, 1, 1 },
    { "LSFT", 0x70, 3, 0, 1 }, { "RSFT", 0x7d, 3, 1, 1 },
};
static const BlockKey s_pad[] = {       // thirds of PAD_W
    { "7", 0x42, 0, 0, 1 }, { "8", 0x43, 0, 1, 1 }, { "9", 0x44, 0, 2, 1 },
    { "4", 0x46, 1, 0, 1 }, { "5", 0x47, 1, 1, 1 }, { "6", 0x48, 1, 2, 1 },
    { "1", 0x4a, 2, 0, 1 }, { "2", 0x4b, 2, 1, 1 }, { "3", 0x4c, 2, 2, 1 },
    { "0", 0x4e, 3, 0, 1 }, { ",", 0x4f, 3, 1, 1 }, { ".", 0x50, 3, 2, 1 },
};
static const BlockKey s_op[] = {
    { "*", 0x45, 0, 0, 1 }, { "+", 0x49, 1, 0, 1 }, { "=", 0x4d, 2, 0, 1 },
};
static const BlockKey s_ret[] = { { "MENU", K_MENU, 0, 0, 1 }, { "BS", 0x0e, 1, 0, 1 }, { "\x05", 0x1c, 2, 0, 1 } };
static const BlockKey s_cursor[] = {    // halves of CUR_W; the arrows under them are add_cursor's
    { "INS", 0x38, 0, 0, 1 }, { "DEL", 0x39, 0, 1, 1 },
};

#define MAXKEYS 112
static Key s_keys[MAXKEYS];
static int s_rx0 = -1, s_ry0, s_ry1;   // Landscape-GAME: RETURN's arm, reaching left under P
static int s_lamp_x0 = -1, s_lamp_y0, s_lamp_x1, s_lamp_y1;   // Landscape: the gap under INS / DEL, for the lamps
static int s_nkeys;
static bool s_laid;

// What is held: by touch, latched (SHIFT/CTRL/GRPH) or locked (CAPS/KANA).
static uint8_t s_touch[MAXKEYS];
static volatile bool s_wait_lift;   // the layout changed under a finger: nothing is pressed until it lifts
static bool s_latch_shift, s_latch_rshift, s_latch_ctrl, s_latch_grph, s_lock_caps, s_lock_kana;

static uint16_t *s_fb;

// The PC-9801VM/VX keyboard: warm ivory letter keys and warm grey function keys, dark brown-grey legends.
#define C_BG      0x0000
#define C_KEY     0xc5b1
#define C_KEYHI   0xcdf2
#define C_KEYDN   0xa4ad
#define C_FKEY    0x7b6b
#define C_FKEYHI  0x83cc
#define C_FKEYDN  0x5aa8
#define C_LATCH   0xcc47
#define C_TEXT    0x2944
// Landscape's function keys, down the side of the picture: much darker, so they do not draw the eye.
#define C_FNKEY   0x31a6
#define C_FNKEYHI 0x39e7
#define C_FNKEYDN 0x2124
#define C_FNTEXT  0x8c51
// MENU, a step darker than the keys around it: it is the emulator's, not the PC-98's.
#define C_MENU    0x4a27
#define C_MENUHI  0x5268
#define C_MENUDN  0x39a5
#define C_FNMENU   0x18e3
#define C_FNMENUHI 0x2124
#define C_FNMENUDN 0x1082

// Landscape: the main keys, then RETURN, the cursor keys, keypad and its * + =.
#define L_CUR_W  130
#define L_PAD_W  210
#define L_OP_W   70
#define L_RET_W  100
#define L_MAIN_W (1280 - L_CUR_W - L_PAD_W - L_OP_W - L_RET_W)

// The keyboard's own coordinates: portrait is the panel's (720x1280), landscape is the picture's way up
// (1280x720), which on the panel is x -> panel row 1279 - x, y -> panel column y (lcd_mipi.cpp, ROTATE_180).
static int s_mode = OSK_PORTRAIT;
#define LAND (s_mode == OSK_LANDSCAPE || s_mode == OSK_GAME)
#define GAME (s_mode == OSK_GAME)
#define LW   (LAND ? PAN_H : PAN_W)
#define LH   (LAND ? PAN_W : PAN_H)
#define TOP  (LAND ? OSKL_TOP : OSK_TOP)
static inline uint16_t &PX(int x, int y) {
    return LAND ? s_fb[(size_t)(PAN_H - 1 - x) * PAN_W + y] : s_fb[(size_t)y * PAN_W + x];
}

// A key in the keyboard's own pixels, inside the 2-pixel margin every key keeps.
static void add_px(const char *label, uint8_t nk, int x0, int x1, int y0, int y1) {
    const int gap = 4;
    if (s_nkeys >= MAXKEYS) return;
    Key *o = &s_keys[s_nkeys++];
    o->x0 = (int16_t)(x0 + gap / 2); o->x1 = (int16_t)(x1 - gap / 2);
    o->y0 = (int16_t)(y0 + gap / 2); o->y1 = (int16_t)(y1 - gap / 2);
    o->label = label;
    o->nkey = nk;
}

// The arrows in a w x h block at (x0, y0): up across the top, left and right side by side, down across the bottom,
// with up and down as low as makes all four keys, as seen inside their margins, the same area.
static void add_cursor(int x0, int y0, int w, int h) {
    const int g = 4;
    const double a = w - g, b = w / 2.0 - g, h1 = (b * (h - g) + a * g) / (a + 2 * b);
    const int ya = y0 + (int)(h1 + 0.5), yb = y0 + h - (int)(h1 + 0.5), xm = x0 + w / 2;
    add_px("\x02", 0x3a, x0, x0 + w, y0, ya);
    add_px("\x01", 0x3b, x0, xm, ya, yb);
    add_px("\x04", 0x3c, xm, x0 + w, ya, yb);
    add_px("\x03", 0x3d, x0, x0 + w, yb, y0 + h);
}

// Landscape's keys beside the picture are placed in units of a 1038-wide plan, scaled to the panel's 1280, and in
// fractions of the keyboard's height down from its top (negative: beside the picture).
static void add_plan(const char *label, uint8_t nk, double x0, double x1, double r0, double r1) {
    const int gap = 4, T0 = OSKL_TOP, H = 720 - OSKL_TOP;
    if (s_nkeys >= MAXKEYS) return;
    Key *o = &s_keys[s_nkeys++];
    o->x0 = (int16_t)(x0 * 1280 / 1038 + gap / 2);
    o->x1 = (int16_t)(x1 * 1280 / 1038 - gap / 2);
    o->y0 = (int16_t)(T0 + r0 * H + gap / 2);
    o->y1 = (int16_t)(T0 + r1 * H - gap / 2);
    o->label = label;
    o->nkey = nk;
}
#define PLAN_FX (1038 - 140)   // the function keys' column, at the right edge

// Two keys side by side, 70 wide each and kr pixels high, at plan x and y pixels from the keyboard's top.
static void add_pair(const char *l0, uint8_t n0, const char *l1, uint8_t n1, double x, double y, double kr) {
    const double H = 720 - OSKL_TOP;
    add_plan(l0, n0, x, x + 70, y / H, (y + kr) / H);
    add_plan(l1, n1, x + 70, x + 140, y / H, (y + kr) / H);
}

// The column right of the picture, keys kr pixels high: F1-F5 and F6-F10 side by side from the top, ROLL UP /
// ROLL DOWN, INS / DEL, then the drive lamps side by side (lcd_mipi.cpp, 6 high), with the room left over shared
// equally among the gaps above and under ROLL UP / ROLL DOWN and above and under the lamps - the keys' own 2-pixel
// margins counted, so the gaps look alike. Returns that gap, for the column at the left.
// half: the gaps as seen half as high, and the lamps that half gap above the keyboard (the room left over between
// INS / DEL and the lamps). rshift: the right SHIFT, two columns wide, under the function keys and a gap above
// ROLL UP / ROLL DOWN.
static double side_right(double kr, bool half, bool rshift) {
    const int gap = 4, T0 = OSKL_TOP;
    const double top = -(double)T0, sep0 = (T0 - 7 * kr - 6 - gap) / 4;
    const double sep = half ? (sep0 + gap) / 2 - gap : sep0;
    const double H = 720 - OSKL_TOP;
    for (int k = 0; k < 5; k++) add_pair(r1[k].label, r1[k].nkey, r1[k + 5].label, r1[k + 5].nkey, PLAN_FX, top + k * kr, kr);
    double y = top + 5 * kr + sep;
    if (rshift) {
        add_plan("SHIFT", 0x7d, PLAN_FX, PLAN_FX + 140, y / H, (y + kr) / H);
        y += kr + sep;
    }
    add_pair("RUP", 0x36, "RDWN", 0x37, PLAN_FX, y, kr);
    add_pair("INS", 0x38, "DEL", 0x39, PLAN_FX, y + kr + sep, kr);
    const Key &ins = s_keys[s_nkeys - 2], &del = s_keys[s_nkeys - 1];
    s_lamp_x0 = ins.x0; s_lamp_x1 = del.x1;
    if (half) {   // just the lamps' 6 rows: lcd_mipi.cpp centres them in the slot
        s_lamp_y1 = (int)(T0 + gap / 2 - (sep + gap));
        s_lamp_y0 = s_lamp_y1 - 6;
    } else {
        s_lamp_y0 = ins.y1; s_lamp_y1 = T0 + gap / 2;
    }
    return sep;
}

// Landscape-GAME: only the keys games use, in four rows under the picture - letters with ':', then \ (yen), BS and RETURN,
// the cursor keys, the keypad - with MENU ... KANA GRPH at the picture's left and the function keys at its right.
static void layout_game(void) {
    const int T0 = OSKL_TOP, H = 720 - OSKL_TOP;
    s_nkeys = 0;
    auto add = add_plan;
    const double L = 1038 - 410 - 70, w = L / 9, xr = L, xc = xr + 70, xp = xc + 130;
    auto row = [&](const KeyDef *d, const uint8_t *keep, int n, int r) {
        int k = 0;
        for (const KeyDef *e = d; e->label && k < n; e++)
            if (e->nkey == keep[k]) { add(e->label, e->nkey, k * w, (k + 1) * w, r / 4.0, (r + 1) / 4.0); k++; }
    };
    static const uint8_t q[] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x17, 0x18, 0x19};
    static const uint8_t a[] = {0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x24, 0x25};
    static const uint8_t z[] = {0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31};
    row(r3, q, 9, 0);
    row(r4, a, 8, 1);
    row(r5, z, 9, 2);
    // A second RETURN and NFER from the left and XFER at the right, each half as wide again as a letter; the space bar between.
    add("\x05", 0x1c, 0, 1.5 * w, 0.75, 1); add("NFER", 0x51, 1.5 * w, 3 * w, 0.75, 1);
    add("", 0x34, 3 * w, 7.5 * w, 0.75, 1); add("XFER", 0x35, 7.5 * w, 9 * w, 0.75, 1);
    add(":", 0x27, 8 * w, 9 * w, 0.25, 0.5);
    add("\\", 0x0d, xr, xr + 70, 0, 0.25); add("BS", 0x0e, xr, xr + 70, 0.25, 0.5);
    add("\x05", 0x1c, xr, xr + 70, 0.5, 1);
    s_rx0 = -1;                                   // RETURN is a plain column now: ':' sits where its arm was
    {
        const int cx0 = (int)(xc * 1280 / 1038), cx1 = (int)((xc + 130) * 1280 / 1038);
        add_cursor(cx0, T0, cx1 - cx0, H);
    }
    static const BlockKey pad[] = {
        { "HOME", 0x3e, 0, 0, 1 }, { "HELP", 0x3f, 0, 1, 1 }, { "-", 0x40, 0, 2, 1 }, { "/", 0x41, 0, 3, 1 },
        { "7", 0x42, 1, 0, 1 }, { "8", 0x43, 1, 1, 1 }, { "9", 0x44, 1, 2, 1 }, { "*", 0x45, 1, 3, 1 },
        { "4", 0x46, 2, 0, 1 }, { "5", 0x47, 2, 1, 1 }, { "6", 0x48, 2, 2, 1 }, { "=", 0x4d, 2, 3, 1 },
        { "1", 0x4a, 3, 0, 1 }, { "2", 0x4b, 3, 1, 1 }, { "3", 0x4c, 3, 2, 1 }, { "0", 0x4e, 3, 3, 1 },
    };
    for (const BlockKey &b : pad) add(b.label, b.nkey, xp + b.col * 70, xp + (b.col + 1) * 70, b.tier / 4.0, (b.tier + 1) / 4.0);
    // Beside the picture, in rows as high as the keyboard's: at the right, above '-' and '/', side_right's column. At
    // the left, the same way: MENU, a gap as wide, then STOP COPY, ESC, TAB, CTRL CAPS, SHIFT, KANA GRPH - two columns
    // wide but STOP / COPY, CTRL / CAPS and KANA / GRPH.
    const double kr = 0.25 * H, top = -(double)T0, sep = side_right(kr, false, false);
    auto pair = [&](const char *l0, uint8_t n0, const char *l1, uint8_t n1, double x, double y) { add_pair(l0, n0, l1, n1, x, y, kr); };
    add("MENU", K_MENU, 0, 140, top / H, (top + kr) / H);
    pair("STOP", 0x60, "COPY", 0x61, 0, top + kr + sep);
    static const struct { const char *label; uint8_t nkey; } side[] = { { "ESC", 0x00 }, { "TAB", 0x0f } };
    for (int k = 0; k < 2; k++) {
        const double y = top + (k + 2) * kr + sep;
        add(side[k].label, side[k].nkey, 0, 140, y / H, (y + kr) / H);
    }
    pair("CTRL", 0x74, "CAPS", 0x71, 0, top + 4 * kr + sep);
    add("SHIFT", 0x70, 0, 140, (top + 5 * kr + sep) / H, (top + 6 * kr + sep) / H);
    // KANA GRPH under SHIFT, its foot no lower than the keyboard's top.
    const double ky = top + 6 * kr + sep, kb = ky + kr < 0 ? ky + kr : 0;
    add("\xb6\xc5", 0x72, 0, 70, ky / H, kb / H); add("GRPH", 0x73, 70, 140, ky / H, kb / H);
    s_laid = true;
}

static void layout(void) {
    if (GAME) { layout_game(); return; }
    s_rx0 = -1;
    const int gap = 4;
    const int rowh = LAND ? OSKL_ROWH : OSK_ROWH;
    s_nkeys = 0;
    for (int r = 0; r < NROWS; r++) {
        int units = 0;
        for (const KeyDef *k = s_rows[r]; k->label; k++) units += k->w;
        const int y0 = TOP + (LAND ? r - 1 : r) * rowh;
        // Landscape: beside the picture as in GAME, in rows as high as the keyboard's and with gaps half as high - at
        // the right side_right's column with the right SHIFT, at the left MENU, the same gap, STOP COPY, ESC, TAB,
        // CTRL CAPS, SHIFT - and under it the main keys from the left edge.
        if (LAND && r == 0) {
            const double kr = rowh, H = 720 - OSKL_TOP, top = -(double)OSKL_TOP, sep = side_right(kr, true, true);
            add_plan("MENU", K_MENU, 0, 140, top / H, (top + kr) / H);
            add_pair("STOP", 0x60, "COPY", 0x61, 0, top + kr + sep, kr);
            add_plan("ESC", 0x00, 0, 140, (top + 2 * kr + sep) / H, (top + 3 * kr + sep) / H);
            add_plan("TAB", 0x0f, 0, 140, (top + 3 * kr + sep) / H, (top + 4 * kr + sep) / H);
            add_pair("CTRL", 0x74, "CAPS", 0x71, 0, top + 4 * kr + sep, kr);
            add_plan("SHIFT", 0x70, 0, 140, (top + 5 * kr + sep) / H, (top + 6 * kr + sep) / H);
            continue;
        }
        const int rx = 0;
        const int rw = LAND ? L_MAIN_W : PAN_W;
        int acc = 0;
        for (const KeyDef *k = s_rows[r]; k->label && s_nkeys < MAXKEYS; k++) {
            Key *o = &s_keys[s_nkeys++];
            o->x0 = (int16_t)(rx + acc * rw / units + gap / 2);
            acc += k->w;
            o->x1 = (int16_t)(rx + acc * rw / units - gap / 2);
            o->y0 = (int16_t)(y0 + gap / 2);
            o->y1 = (int16_t)(y0 + rowh - gap / 2);
            o->label = k->label;
            o->nkey = k->nkey;
        }
    }
    const int by = LAND ? TOP : TOP + NROWS * rowh;
    const int xl = 0, lw = LEFT_W;
    // Landscape puts the BS / RETURN column between the main keys and the cursor keys.
    const int rw = LAND ? L_RET_W : RET_W;
    const int xc = LAND ? L_MAIN_W + L_RET_W : LEFT_W, cw = LAND ? L_CUR_W : CUR_W;
    const int xp = xc + cw, pw = LAND ? L_PAD_W : PAD_W;
    const int xo = xp + pw, ow = LAND ? L_OP_W : OP_W;
    const int xr = LAND ? L_MAIN_W : xo + ow;
    auto block = [&](const BlockKey *t, int n, int x0, int w, int div, int toff) {
        for (int i = 0; i < n && s_nkeys < MAXKEYS; i++) {
            Key *o = &s_keys[s_nkeys++];
            const int y0 = by + (t[i].tier + toff) * rowh;
            o->x0 = (int16_t)(x0 + t[i].col * w / div + gap / 2);
            o->x1 = (int16_t)(x0 + (t[i].col + t[i].cols) * w / div - gap / 2);
            o->y0 = (int16_t)(y0 + gap / 2);
            o->y1 = (int16_t)(y0 + (t == s_ret && i == 2 ? 3 : 1) * rowh - gap / 2);
            o->label = t[i].label;
            o->nkey = t[i].nkey;
        }
    };
    auto put = [&](const char *label, uint8_t nk, int x0, int x1, int y0, int y1) {
        if (s_nkeys >= MAXKEYS) return;
        Key *o = &s_keys[s_nkeys++];
        o->x0 = (int16_t)(x0 + gap / 2); o->x1 = (int16_t)(x1 - gap / 2);
        o->y0 = (int16_t)(y0 + gap / 2); o->y1 = (int16_t)(y1 - gap / 2);
        o->label = label;
        o->nkey = nk;
    };
    if (LAND) {
        // MENU, ROLL UP ... DEL are beside the picture: BS on top of the RETURN column and RETURN four tiers high under
        // it; the arrows over all five tiers.
        put("BS", 0x0e, xr, xr + rw, by, by + rowh);
        put("\x05", 0x1c, xr, xr + rw, by + rowh, by + 5 * rowh);
        add_cursor(xc, by, cw, 5 * rowh);
        put("\x05", 0x1c, xo, xo + ow, by + 4 * rowh, by + 5 * rowh);   // a second RETURN under '='
    } else {
        block(s_top_left, 2, xl, lw, 2, 0);
        block(s_left, (int)(sizeof(s_left) / sizeof(s_left[0])), xl, lw, 2, 1);
        block(s_top_cur, 2, xc, cw, 2, 0);
        block(s_cursor, (int)(sizeof(s_cursor) / sizeof(s_cursor[0])), xc, cw, 2, 1);
        add_cursor(xc, by + 2 * rowh, cw, 3 * rowh);   // under INS / DEL
        block(s_ret, 3, xr, rw, 1, 0);   // MENU, BS, then RETURN three tiers high
    }
    block(s_top_pad, 4, xp, pw + ow, 4, 0);   // as wide as the keypad's keys
    block(s_pad, (int)(sizeof(s_pad) / sizeof(s_pad[0])), xp, pw, 3, 1);
    block(s_op, (int)(sizeof(s_op) / sizeof(s_op[0])), xo, ow, 1, 1);
    s_laid = true;
}

static bool key_latched(const Key *k) {
    switch (k->nkey) {
    case 0x70: return s_latch_shift;
    case 0x7d: return s_latch_rshift;
    case 0x74: return s_latch_ctrl;
    case 0x73: return s_latch_grph;
    case 0x71: return s_lock_caps;
    case 0x72: return s_lock_kana;
    default: return false;
    }
}

// Write back the panel rows under a rectangle in the keyboard's coordinates.
static void sync_rect(int x0, int y0, int x1, int y1) {
    if (LAND) { y0 = PAN_H - x1; y1 = PAN_H - x0; }
    esp_cache_msync(s_fb + (size_t)y0 * PAN_W, (size_t)(y1 - y0) * PAN_W * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// The legends printed beside the key's own: shifted, kana, and the small kana kana-shift gives. By key code.
typedef struct { uint8_t nkey; const char *shift, *kana, *kshift; } Legend;
static const Legend s_legend[] = {
    { 0x01, "!", "\xc7", nullptr }, { 0x02, "\"", "\xcc", nullptr }, { 0x03, "#", "\xb1", "\xa7" },
    { 0x04, "$", "\xb3", "\xa9" }, { 0x05, "%", "\xb4", "\xaa" }, { 0x06, "&", "\xb5", "\xab" },
    { 0x07, "'", "\xd4", "\xac" }, { 0x08, "(", "\xd5", "\xad" }, { 0x09, ")", "\xd6", "\xae" },
    { 0x0a, nullptr, "\xdc", "\xa6" }, { 0x0b, "=", "\xce", nullptr }, { 0x0c, "`", "\xcd", nullptr },
    { 0x0d, "|", "\xb0", nullptr },
    { 0x10, nullptr, "\xc0", nullptr }, { 0x11, nullptr, "\xc3", nullptr }, { 0x12, nullptr, "\xb2", "\xa8" },
    { 0x13, nullptr, "\xbd", nullptr }, { 0x14, nullptr, "\xb6", nullptr }, { 0x15, nullptr, "\xdd", nullptr },
    { 0x16, nullptr, "\xc5", nullptr }, { 0x17, nullptr, "\xc6", nullptr }, { 0x18, nullptr, "\xd7", nullptr },
    { 0x19, nullptr, "\xbe", nullptr }, { 0x1a, "~", "\xde", nullptr }, { 0x1b, "{", "\xdf", "\xa2" },
    { 0x1d, nullptr, "\xc1", nullptr }, { 0x1e, nullptr, "\xc4", nullptr }, { 0x1f, nullptr, "\xbc", nullptr },
    { 0x20, nullptr, "\xca", nullptr }, { 0x21, nullptr, "\xb7", nullptr }, { 0x22, nullptr, "\xb8", nullptr },
    { 0x23, nullptr, "\xcf", nullptr }, { 0x24, nullptr, "\xc9", nullptr }, { 0x25, nullptr, "\xd8", nullptr },
    { 0x26, "+", "\xda", nullptr }, { 0x27, "*", "\xb9", nullptr }, { 0x28, "}", "\xd1", "\xa3" },
    { 0x29, nullptr, "\xc2", "\xaf" }, { 0x2a, nullptr, "\xbb", nullptr }, { 0x2b, nullptr, "\xbf", nullptr },
    { 0x2c, nullptr, "\xcb", nullptr }, { 0x2d, nullptr, "\xba", nullptr }, { 0x2e, nullptr, "\xd0", nullptr },
    { 0x2f, nullptr, "\xd3", nullptr }, { 0x30, "<", "\xc8", "\xa4" }, { 0x31, ">", "\xd9", "\xa1" },
    { 0x32, "?", "\xd2", "\xa5" }, { 0x33, nullptr, "\xdb", nullptr },
};

// Text in the 8x16 ANK face, scaled by hx/2 across and hy/2 down (2 is the face's own size, 3 half as large again).
static void draw_cells(int tx, int ty, const char *str, int cw, int ch, uint16_t fg);
static void draw_text(int tx, int ty, const char *str, int hx, int hy, uint16_t fg) {
    draw_cells(tx, ty, str, 8 * hx / 2, 16 * hy / 2, fg);
}

// The same, each cell drawn cw x ch pixels.
static void draw_cells(int tx, int ty, const char *str, int cw, int ch, uint16_t fg) {
    for (int c = 0; str[c]; c++) {
        const uint8_t *g = cgrom + 0x80000 + (unsigned)(uint8_t)str[c] * 16;
        for (int oy = 0; oy < ch; oy++) {
            const uint8_t b = g[oy * 16 / ch];
            if (!b) continue;
            for (int ox = 0; ox < cw; ox++)
                if (b & (0x80 >> (ox * 8 / cw))) PX(tx + c * cw + ox, ty + oy) = fg;
        }
    }
}

static void paint_key(int i, bool sync) {
    const Key *k = &s_keys[i];
    const bool down = s_touch[i] != 0;
    const bool lat = key_latched(k);
    // Letters, digits, symbols and the space bar are ivory; the keys named by a word, and the arrows, grey.
    const bool grey = k->nkey != 0x34 && (k->label[0] < 0x20 || k->label[1] != 0);
    // The keys beside the picture in landscape are the dark ones.
    const bool fn = LAND && k->y1 <= TOP;
    const bool mn = k->nkey == K_MENU;
    const uint16_t bg = lat ? C_LATCH : mn ? (fn ? (down ? C_FNMENUDN : C_FNMENU) : (down ? C_MENUDN : C_MENU))
                      : fn ? (down ? C_FNKEYDN : C_FNKEY)
                      : down ? (grey ? C_FKEYDN : C_KEYDN) : (grey ? C_FKEY : C_KEY);
    const uint16_t hi = mn ? (fn ? C_FNMENUHI : C_MENUHI) : fn ? C_FNKEYHI : grey ? C_FKEYHI : C_KEYHI;
    const uint16_t fg = (fn || mn) ? C_FNTEXT : C_TEXT;   // MENU is dark enough to want the light legend
    const int r = 10;
    for (int y = k->y0; y < k->y1; y++) {
        const int dy = (y - k->y0 < k->y1 - 1 - y) ? y - k->y0 : k->y1 - 1 - y;
        for (int x = k->x0; x < k->x1; x++) {
            const int dx = (x - k->x0 < k->x1 - 1 - x) ? x - k->x0 : k->x1 - 1 - x;
            if (dx < r && dy < r && (r - dx - 1) * (r - dx - 1) + (r - dy - 1) * (r - dy - 1) > r * r) {
                PX(x, y) = C_BG;
                continue;
            }
            PX(x, y) = (y <= k->y0 + 2 && !down && !lat) ? hi : bg;
        }
    }
    const int w = k->x1 - k->x0, h = k->y1 - k->y0;
    // RETURN: the bent arrow, down from the top right and back to the left.
    if (k->label[0] == 5) {
        if (GAME && s_rx0 >= 0) {
            // GAME's RETURN is L-shaped: an arm reaching left, rounded on its left only and run into the body.
            for (int y = s_ry0; y < s_ry1; y++) {
                const int dy = (y - s_ry0 < s_ry1 - 1 - y) ? y - s_ry0 : s_ry1 - 1 - y;
                for (int x = s_rx0; x <= k->x0 + r; x++) {
                    const int dx = x - s_rx0;
                    if (dx < r && dy < r && (r - dx - 1) * (r - dx - 1) + (r - dy - 1) * (r - dy - 1) > r * r) continue;
                    PX(x, y) = (y <= s_ry0 + 2 && !down) ? hi : bg;
                }
            }
            if (sync) sync_rect(s_rx0, s_ry0, k->x0 + r + 1, s_ry1);
        }
        const int a = (h < w ? h : w) / 3, cx = k->x0 + w / 2, cy = k->y0 + h / 2 + a / 4;
        const int xr = cx + a / 2, xl = cx - a / 2, th = 2;
        for (int y = cy - a; y <= cy + th; y++)
            for (int x = xr - th; x <= xr + th; x++) PX(x, y) = fg;
        for (int y = cy - th; y <= cy + th; y++)
            for (int x = xl; x <= xr; x++) PX(x, y) = fg;
        const int hd = a / 3 + 2;
        for (int t = 0; t <= hd; t++)
            for (int u = -t; u <= t; u++) PX(xl - hd / 2 + t, cy + u) = fg;
        if (sync) sync_rect(k->x0, k->y0, k->x1, k->y1);
        return;
    }
    // The cursor keys: a triangle, 1 left, 2 up, 3 down, 4 right. All four the same size, from the smallest of the
    // four keys' areas (they are laid out alike, add_cursor), so a wide key and a tall one show the same triangle.
    if (k->label[0] >= 1 && k->label[0] <= 4) {
        int a = 1 << 30;
        for (int j = 0; j < s_nkeys; j++) {
            const Key *c = &s_keys[j];
            if (c->label[0] < 1 || c->label[0] > 4) continue;
            const int s = (int)sqrtf((float)((c->x1 - c->x0) * (c->y1 - c->y0))) / 5;
            if (s < a) a = s;
        }
        const int d = k->label[0];
        const int cx = k->x0 + w / 2, cy = k->y0 + h / 2;
        for (int t = 0; t <= a; t++) {
            for (int u = -t; u <= t; u++) {
                int x, y;
                if (d == 1) { x = cx - a / 2 + t; y = cy + u; }
                else if (d == 4) { x = cx + a / 2 - t; y = cy + u; }
                else if (d == 2) { x = cx + u; y = cy - a / 2 + t; }
                else { x = cx + u; y = cy + a / 2 - t; }
                PX(x, y) = fg;
            }
        }
        if (sync) sync_rect(k->x0, k->y0, k->x1, k->y1);
        return;
    }
    // The label in the 8x16 ANK face, as large as fits.
    const int n = (int)strlen(k->label);
    const Legend *lg = nullptr;
    for (const Legend &l : s_legend)
        if (l.nkey == k->nkey) lg = &l;
    if (lg) {
        // A key with more than one legend, as printed: the key's own large at the top left, the shifted character
        // small at the top right, the kana at the bottom right, and the small kana at the bottom left where it clears.
        // The kana half as large again as the face, so they read; the shifted character at the face's own size.
        const int tx = k->x0 + 6, ty = k->y0 + 3;
        // Landscape's low keys take the key's own legend at half as large again, to leave the bottom left free.
        const int hm = LAND ? 3 : 4, mh = 8 * hm;
        draw_text(tx, ty, k->label, hm, hm, fg);
        // As large as the key allows: twice the face on portrait's taller keys, half as large again on landscape's.
        const int hk = h >= 56 ? 4 : 3;
        // The shifted character half as large again on portrait's taller keys, where it clears the kana.
        const int hs = h >= 56 ? 3 : 2;
        if (lg->shift) draw_text(k->x1 - 5 - 4 * hs, ty, lg->shift, hs, hs, fg);
        if (lg->kana) draw_text(k->x1 - 5 - 4 * hk, k->y1 - 3 - 8 * hk + (LAND ? 1 : 0), lg->kana, hk, hk, fg);
        // The small kana in whatever is left under the key's own legend, as large as that allows.
        // The glyph's ink, not its cell, is scaled to fill the corner between the key's legend and the kana.
        const int kh = k->y1 - 3 - (ty + mh + 2), kw = k->x1 - 5 - 4 * hk - 3 - tx;
        if (lg->kshift && kh >= 8) {
            const uint8_t *g = cgrom + 0x80000 + (unsigned)(uint8_t)lg->kshift[0] * 16;
            int l = 8, r = -1, t = 16, bt = -1;
            for (int gy = 0; gy < 16; gy++)
                for (int gx = 0; gx < 8; gx++)
                    if (g[gy] & (0x80 >> gx)) {
                        if (gx < l) l = gx;
                        if (gx > r) r = gx;
                        if (gy < t) t = gy;
                        if (gy > bt) bt = gy;
                    }
            if (r >= 0) {
                const int iw = r - l + 1, ih = bt - t + 1;
                int sc = (kw * ih < kh * iw) ? kw * 16 / iw : kh * 16 / ih;   // sixteenths
                if (sc > 8 * hk) sc = 8 * hk;    // no larger than the kana beside it: the marks and brackets stay small
                const int ow = iw * sc / 16, oh = ih * sc / 16, oy = k->y1 - 5 - oh;
                for (int y = 0; y < oh; y++) {
                    const uint8_t b = g[t + y * 16 / sc];
                    for (int x = 0; x < ow; x++)
                        if (b & (0x80 >> (l + x * 16 / sc))) PX(tx + x, oy + y) = fg;
                }
            }
        }
    } else {
        // Twice the size, or as wide as fits where a long label would not: 1.5 or 1 times across.
        int hx = 4;
        while (hx > 2 && n * 8 * hx / 2 > w - 8) hx--;
        // Centred on the ink, not the cells: the face leaves its blank columns on the right of a glyph.
        int l = 8, r = -1;
        for (int c = 0; c < n; c++)
            for (int gy = 0; gy < 16; gy++) {
                const uint8_t b = cgrom[0x80000 + (unsigned)(uint8_t)k->label[c] * 16 + gy];
                for (int gx = 0; gx < 8; gx++)
                    if (b & (0x80 >> gx)) {
                        if (c == 0 && gx < l) l = gx;
                        if (c == n - 1 && gx > r) r = gx;
                    }
            }
        if (r < 0) { l = 0; r = 7; }
        const int ink0 = l * hx / 2, ink1 = ((n - 1) * 8 + r + 1) * hx / 2;
        draw_text(k->x0 + (w - (ink1 - ink0)) / 2 - ink0, k->y0 + (h - 32) / 2, k->label, hx, 4, fg);
    }
    if (sync) sync_rect(k->x0, k->y0, k->x1, k->y1);
}

static void paint_nkey(uint8_t nkey) {
    for (int i = 0; i < s_nkeys; i++)
        if (s_keys[i].nkey == nkey) paint_key(i, true);
}

extern "C" void osk_paint(uint16_t *fb, int mode) {
    s_fb = fb;
    if (!s_fb) return;
    if (!s_laid || mode != s_mode) {
        // A finger still on a key (RET, from the menu that switched the layout) is let go first: the held flags are
        // by key number, and in the new layout that number is another key - the old one never came up again and
        // the PC-98 saw it held, repeating.
        for (int i = 0; i < s_nkeys; i++)
            if (s_touch[i] && s_keys[i].nkey < 0x80 && !(s_keys[i].nkey >= 0x70 && s_keys[i].nkey <= 0x74) && s_keys[i].nkey != 0x7d)
                hid_key_event(s_keys[i].nkey, 0);
        memset(s_touch, 0, sizeof(s_touch));
        // And that finger does not press whatever key is under it now. Not at power on: a key held from before the
        // keyboard came up (F1 for a game's start-up option) has to go down.
        s_wait_lift = s_laid;
        s_mode = mode;
        layout();
    }
    for (int y = TOP; y < LH; y++)
        for (int x = 0; x < LW; x++) PX(x, y) = C_BG;
    if (LAND)
        for (int y = 0; y < TOP; y++)
            for (int x = 0; x < LW; x++)
                if (x < OSKL_PIC_X || x >= OSKL_PIC_X + OSKL_PIC_W) PX(x, y) = C_BG;
    for (int i = 0; i < s_nkeys; i++) paint_key(i, false);
    if (LAND) sync_rect(0, 0, LW, LH);
    else sync_rect(0, TOP, LW, LH);
}

extern "C" bool osk_lamp_slot(int *x0, int *y0, int *x1, int *y1) {
    if (!s_laid || !LAND || s_lamp_x0 < 0) return false;
    *x0 = s_lamp_x0; *y0 = s_lamp_y0; *x1 = s_lamp_x1; *y1 = s_lamp_y1;
    return true;
}

static int key_at(int x, int y) {
    for (int i = 0; i < s_nkeys; i++) {
        const Key *k = &s_keys[i];
        if (x >= k->x0 - 2 && x < k->x1 + 2 && y >= k->y0 - 2 && y < k->y1 + 2) return i;
        // GAME's RETURN arm
        if (GAME && k->nkey == 0x1c && s_rx0 >= 0 && x >= s_rx0 - 2 && x < k->x0 && y >= s_ry0 - 2 && y < s_ry1 + 2) return i;
    }
    return -1;
}

// A key that is not a modifier went up: the latched modifiers let go with it.
static void release_latches(void) {
    if (s_latch_shift) { s_latch_shift = false; hid_key_event(0x70, 0); paint_nkey(0x70); }
    if (s_latch_rshift) { s_latch_rshift = false; hid_key_event(0x7d, 0); paint_nkey(0x7d); }
    if (s_latch_ctrl)  { s_latch_ctrl = false;  hid_key_event(0x74, 0); paint_nkey(0x74); }
    if (s_latch_grph)  { s_latch_grph = false;  hid_key_event(0x73, 0); paint_nkey(0x73); }
}

static void key_down(const Key *k) {
    bool *flag = nullptr;
    switch (k->nkey) {
    case 0x70: flag = &s_latch_shift; break;
    case 0x7d: flag = &s_latch_rshift; break;
    case 0x74: flag = &s_latch_ctrl; break;
    case 0x73: flag = &s_latch_grph; break;
    case 0x71: flag = &s_lock_caps; break;
    case 0x72: flag = &s_lock_kana; break;
    case K_MENU:
        if (!g_menu_active) g_menu_req = 1;
        return;
    default:
        hid_key_event(k->nkey, 1);
        return;
    }
    *flag = !*flag;
    hid_key_event(k->nkey, *flag ? 1 : 0);
    paint_nkey(k->nkey);
}

static void key_up(const Key *k) {
    switch (k->nkey) {
    case 0x70: case 0x7d: case 0x74: case 0x73: case 0x71: case 0x72: case K_MENU:
        return;
    default:
        hid_key_event(k->nkey, 0);
        release_latches();
    }
}

extern "C" unsigned osk_touch(const uint16_t *tx, const uint16_t *ty, int n) {
    if (!s_fb || !s_laid) return 0;
    uint8_t now[MAXKEYS];
    unsigned used = 0;
    memset(now, 0, sizeof(now));
    for (int p = 0; p < n; p++) {
        const int x = LAND ? PAN_H - 1 - ty[p] : tx[p], y = LAND ? tx[p] : ty[p];
        if (y < TOP && !(LAND && (x < OSKL_PIC_X || x >= OSKL_PIC_X + OSKL_PIC_W))) continue;
        used |= 1u << p;
        const int i = key_at(x, y);
        if (i >= 0) now[i] = 1;
    }
    if (s_wait_lift) {
        if (used) return used;
        s_wait_lift = false;
    }
    for (int i = 0; i < s_nkeys; i++) {
        if (now[i] == s_touch[i]) continue;
        s_touch[i] = now[i];
        if (now[i]) key_down(&s_keys[i]);
        else key_up(&s_keys[i]);
        paint_key(i, true);
    }
    return used;
}

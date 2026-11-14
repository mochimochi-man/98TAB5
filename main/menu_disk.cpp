// Disk image swap menu (runtime FDD1/FDD2/HDD exchange).
//
// Modal text UI on the ST7789. Entered via the Pause/Break key (g_menu_req is
// set in usb_kbd.cpp); the emulator loop calls menu_disk_run() and pauses
// machine_frame while it is active. Runs on the emulator task (core 1), so SD
// file operations happen in the same safe context as the boot-time mounts
// (fopen inside machine_frame crashes — that is why the delayed setfdd path is
// not used).

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_attr.h"

extern "C" int ets_printf(const char *fmt, ...);
#include <sys/stat.h>
#include <ctype.h>
#include <nvs.h>
#include <esp_system.h>   // esp_restart() for the RESET menu item
#include <esp_app_desc.h> // the version on the title line
#include <dirent.h>       // POSIX opendir/readdir over the SD VFS ("/sd")
#include <stdio.h>
#include <time.h>

extern "C" {
#include <compiler.h>
#include <pc98.h>
#include <hostfile.h>
#include "media/drives.h"
#include "dev/atapi.h"
#include "util/pcstr.h"
#include <state.h>       // pc98_state_save / pc98_state_load: the State Save screen
#include "board_pins.h"
int  ets_printf(const char *fmt, ...);
int  usb_kbd_pop(uint8_t *nkey, uint8_t *down);   // usb_kbd.cpp
}

extern volatile int g_speed_req;    // main.cpp applies the new CPU multiple
extern "C" int g_cpu_mode;          // main.cpp: 0 = V30, 1 = 80286, 2 = 80286 + 386 instructions
extern "C" int g_cpu_mode_boot;     // ...and the one this boot is running

static const char *cpu_name(int m) {
    return m == 0 ? "V30" : m == 1 ? "80286" : "80286 + 386 Instructions";
}

// Same rule as the other value rows: arrows stop at the ends, RET wraps.
static int cpu_step(int cur, int dir, bool wrap) {
    int v = cur + dir;
    if (v > 2) {
        v = wrap ? 0 : 2;
    } else if (v < 0) {
        v = wrap ? 2 : 0;
    }
    return v;
}

// LCD text helpers (lcd_st7789.cpp)
extern "C" void lcd_menu_clear(void);
extern "C" void lcd_menu_line(int row, const char *s, uint16_t fg, uint16_t bg);
extern "C" void lcd_menu_flush(void);
extern "C" void lcd_menu_blank_rows(int first, int last);
extern "C" void lcd_menu_line_ex(int row, const char *s, uint16_t fg, uint16_t fg2, int split,
                                 uint16_t bg, uint16_t bar, int c0, int c1, uint16_t stripe);
extern "C" void lcd_menu_rule(int y, int x0, int x1, uint16_t color);
extern "C" void lcd_menu_set_bg(uint16_t c);
extern "C" void panel_tab5_blank_early(void);  // panel_tab5.cpp: backlight + panel rail off
extern "C" void tab5_backlight_set(int percent);   // panel_tab5.cpp
extern "C" void audio_set_volume(int percent);     // audio_codec.cpp
extern "C" int  audio_get_volume(void);

// Backlight and volume are both a percentage in steps of 10. Kept out of the
// emulator core: neither is a PC-98 property, they are this tablet's.
#define LEVEL_STEP 10
#define LEVEL_MIN  10
extern "C" int g_backlight_pct;   // main.cpp owns it so boot can apply it
extern "C" int g_line200;         // main.cpp: how 200-line screens are drawn
extern "C" void line200_apply(int m);
extern "C" void line200_refresh(void);
extern "C" void audio_set_fx(int mode);  // audio_codec.cpp: 0 Original, 2 Hall, 3 Synthesizer, 4 Orchestra
extern "C" int  audio_get_fx(void);
extern "C" void lcd_lamps_enable(bool on);   // lcd_mipi.cpp: the drive lamps
extern "C" bool lcd_lamps_enabled(void);
extern "C" void lcd_set_osk(int mode);        // lcd_mipi.cpp: the on-screen keyboard, 0 off, 1 portrait, 2 landscape, 3 landscape-game
extern "C" int  lcd_osk_mode(void);
extern "C" int  touch_mouse_speed(void);       // touch_mouse.cpp: 0 low, 1 medium, 2 high
extern "C" void touch_mouse_set_speed(int s);

// Rows with three or more choices step round and round with LEFT / RIGHT as well as RET. The number rows
// (CPU clock, backlight, volume) and the two-way ones (clock family, drive lamps) stop at their ends.
static int wrap_step(int cur, int dir, int n) { return ((cur + dir) % n + n) % n; }
// The screen keyboard modes in the order the row steps through them: the landscape ones first.
static const int s_osk_order[4] = { 0, 2, 3, 1 };
static int osk_step(int dir) {
    int at = 0;
    for (int i = 0; i < 4; i++) if (s_osk_order[i] == lcd_osk_mode()) at = i;
    return s_osk_order[wrap_step(at, dir, 4)];
}

// RET wraps around; the arrow keys nudge, which is what anyone actually wants
// when the far end of the range is one step in the wrong direction.
// The three rows that hold a value - CPU clock, backlight, volume - all behave
// the same way:
//
//   LEFT / RIGHT   step down or up, and STOP at the ends
//   RET            step up, and wrap round to the bottom at the top
//
// The split matters rather than being tidiness. Left and right are directional,
// so running off the end and reappearing at the other one is a surprise, and on
// the backlight it is a nasty one: a single RIGHT press at 100% would drop the
// screen to its dimmest. RET is not directional - it is the one key that says
// "next" - so it is the one that wraps, which keeps every value reachable
// without arrow keys.
static int level_step(int cur, int dir, bool wrap, int minimum=LEVEL_MIN) {
    int v = ((cur + LEVEL_STEP - 1) / LEVEL_STEP) * LEVEL_STEP + dir * LEVEL_STEP;
    if (v > 100) {
        v = wrap ? minimum : 100;
    } else if (v < minimum) {
        v = wrap ? 100 : minimum;
    }
    return v;
}

// Same rule for the CPU multiplier, which moves along a fixed ladder rather
// than in even steps.
static UINT clock_step(UINT cur, int dir, bool wrap) {
    static const uint8_t ladder[] = {BRD_CPU_MULT_LADDER};
    const int n = (int)sizeof(ladder);
    int i = 0;
    while (i < n && ladder[i] != cur) {
        i++;
    }
    if (i >= n) {
        i = 0;                      // not on the ladder: start from the bottom
    }
    i += dir;
    if (i < 0) {
        i = wrap ? n - 1 : 0;
    } else if (i >= n) {
        i = wrap ? 0 : n - 1;
    }
    return ladder[i];
}

// Every menu loop polls the keyboard, so this is the one place that is always
// reached once a screen has finished drawing - and therefore the right place to
// push the accumulated drawing out to the panel.
//
// It is also where a held arrow key repeats. The keyboard reports a press and a
// release and nothing in between, so without this a long list has to be walked
// one tap per row. Only the arrows repeat: a held RET or ESC repeating would
// fire whatever the row does - a mount, a RESET - again and again.
#define MENU_REPEAT_DELAY_US    400000
#define MENU_REPEAT_INTERVAL_US  60000
static inline bool key_repeats(uint8_t nk);
static uint8_t s_held_nk = 0xff;     // the arrow being held, 0xff = none
static int64_t s_held_next;          // when it next repeats

static bool menu_key_pop(uint8_t *nk, uint8_t *dn) {
    lcd_menu_flush();
    if (usb_kbd_pop(nk, dn)) {
        if (*dn && key_repeats(*nk)) {
            s_held_nk = *nk;
            s_held_next = esp_timer_get_time() + MENU_REPEAT_DELAY_US;
        } else if (!*dn && *nk == s_held_nk) {
            s_held_nk = 0xff;
        } else if (*dn) {
            s_held_nk = 0xff;        // another key pressed: the arrow stops
        }
        return true;
    }
    if (s_held_nk != 0xff) {
        const int64_t now = esp_timer_get_time();
        if (now >= s_held_next) {
            s_held_next = now + MENU_REPEAT_INTERVAL_US;
            *nk = s_held_nk;
            *dn = 1;
            return true;
        }
    }
    return false;
}
extern "C" int  lcd_get_scale_mode(void);        // lcd_st7789.cpp
extern "C" void lcd_set_scale_mode(int m);
extern "C" int  lcd_scale_mode_count(void);      // number of scaler modes to cycle

// nkey codes used for navigation. The numeric keypad doubles as arrows here:
// keypad 8 = up, keypad 2 = down (keypad Enter already maps to RETURN).
enum { NK_ESC = 0x00, NK_RET = 0x1c, NK_UP = 0x3a, NK_DOWN = 0x3d,
       NK_LEFT = 0x3b, NK_RIGHT = 0x3c, NK_KP8 = 0x43, NK_KP2 = 0x4b,
       NK_KP4 = 0x46, NK_KP6 = 0x48 };

// Treat main-row arrows and the keypad 8/2 the same for menu navigation.
static inline bool key_is_up(uint8_t nk)   { return nk == NK_UP   || nk == NK_KP8; }
static inline bool key_is_down(uint8_t nk) { return nk == NK_DOWN || nk == NK_KP2; }
static inline bool key_is_left(uint8_t nk)  { return nk == NK_LEFT  || nk == NK_KP4; }
static inline bool key_is_right(uint8_t nk) { return nk == NK_RIGHT || nk == NK_KP6; }
static inline bool key_repeats(uint8_t nk) {
    return key_is_up(nk) || key_is_down(nk) || key_is_left(nk) || key_is_right(nk);
}

// RGB565 colors (same values as TFT_eSPI's TFT_* macros)
#define COL_WHITE  0xFFFF
#define COL_BLACK  0x0000
#define COL_YELLOW 0xFFE0

// The look: a near-black page, settings in rounded groups with hairlines between the rows, values a step dimmer
// on the right, one blue accent. COL_WHITE / COL_BLACK / COL_YELLOW above are the roles the plain text screens ask
// for; mline() maps them onto this palette.
#define UI_PAGE    0x0862   // #0B0D10
#define UI_GROUP   0x18C4   // #181B20
#define UI_LINE    0x2146   // #262A31
#define UI_TEXT    0xE75D   // #E6E8EB
#define UI_DIM     0x8C93   // #8A919B
#define UI_CAP     0x6B90   // #6B7280  section headings
#define UI_ACCENT  0x4C7F   // #4C8DFF
#define UI_SEL     0x21AA   // #243651  the selected row
#define UI_SELVAL  0xB67F   // #B4CCFF  a value on the selected row
#define UI_KEY     0x2966   // #2A2E35  key caps
#define UI_BAR_C0  1
#define UI_BAR_C1  79
#define UI_RH      24       // row height in a group
#define UI_L       24       // page margins
#define UI_R       616

extern "C" void lcd_ui_fill(int x0, int y0, int x1, int y1, uint16_t c, int r);
extern "C" int  lcd_ui_text(int x, int y, const char *s, uint16_t c);
extern "C" void lcd_ui_chev(int x, int y, uint16_t c, int dir);
extern "C" void lcd_ui_tri(int x, int y, uint16_t c, int dir);
extern "C" void lcd_ui_dot(int x, int y, uint16_t c);

static void mline(int row, const char *s, uint16_t fg, uint16_t bg) {
    if (bg == COL_YELLOW) {
        lcd_menu_line_ex(row, s, COL_WHITE, COL_WHITE, 80, UI_PAGE, UI_SEL, UI_BAR_C0, UI_BAR_C1, UI_ACCENT);
        return;
    }
    lcd_menu_line(row, s, fg == COL_YELLOW ? UI_ACCENT : fg == COL_WHITE ? UI_TEXT : fg, bg == COL_BLACK ? UI_PAGE : bg);
}

// The title line: "98TAB5 v1.3.0" on the main screen, "98TAB5 > FDD1" one level down.
static void ui_header(const char *crumb) {
    lcd_menu_clear();
    int x = lcd_ui_text(UI_L, 13, "98TAB5", crumb ? UI_DIM : UI_TEXT);
    if (crumb) {
        lcd_ui_chev(x + 6, 17, UI_DIM, 1);
        lcd_ui_text(x + 18, 13, crumb, UI_TEXT);
    } else {
        char v[40];
        snprintf(v, sizeof(v), "v%s", esp_app_get_description()->version);
        lcd_ui_text(x + 10, 13, v, UI_CAP);
    }
    lcd_ui_fill(UI_L, 38, UI_R, 39, UI_LINE, 0);
}

// A section heading over a group, with an optional note on the right.
static void ui_section(int x0, int x1, int y, const char *title, const char *note) {
    lcd_ui_text(x0 + 4, y, title, UI_CAP);
    if (note && note[0]) lcd_ui_text(x1 - 4 - (int)strlen(note) * 8, y, note, UI_CAP);
}

enum { UI_VALUE, UI_ADJUST, UI_ACTION };
typedef struct {
    const char *label;
    const char *value;   // NULL: none
    uint8_t kind;        // UI_VALUE, UI_ADJUST (left/right change it), UI_ACTION (RET does something: a chevron)
    uint8_t mark;        // a blue dot before the value (the disk in this drive)
    uint8_t dim;         // the label in the dim colour (not available)
} UiRow;

static int s_ui_vcol;   // nonzero: values start this far into the row instead of ending at its right
// One group of rows at y. `on` is the selected row (or -1). `right` leaves room on the right (a scrollbar).
static int ui_group(int x0, int x1, int y, const UiRow *rows, int n, int on, int right) {
    const int xe = x1 - right;
    lcd_ui_fill(x0, y, x1, y + UI_RH * n, UI_GROUP, 5);
    for (int i = 0; i < n; i++) {
        const int ry = y + i * UI_RH;
        const UiRow *w = &rows[i];
        const bool sel = (i == on);
        if (sel) {
            // The band keeps the group's rounded corners where it meets them.
            const bool first = (i == 0), last = (i == n - 1);
            lcd_ui_fill(x0, ry, xe, ry + UI_RH, UI_SEL, (first || last) && !right ? 5 : 0);
            if (first && !last && !right) lcd_ui_fill(x0, ry + 6, xe, ry + UI_RH, UI_SEL, 0);
            if (last && !first && !right) lcd_ui_fill(x0, ry, xe, ry + UI_RH - 6, UI_SEL, 0);
            lcd_ui_fill(x0, ry + 5, x0 + 3, ry + UI_RH - 5, UI_ACCENT, 0);
        } else if (i && !(i - 1 == on)) {
            lcd_ui_fill(x0 + 12, ry, xe, ry + 1, UI_LINE, 0);
        }
        lcd_ui_text(x0 + 12, ry + 4, w->label, w->dim ? UI_CAP : UI_TEXT);
        // Room for the value: what is left of the row after the label, less the chevron / arrows.
        int vx1 = xe - 12;
        if (w->kind == UI_ACTION) {
            lcd_ui_chev(xe - 14, ry + 8, sel ? UI_SELVAL : UI_DIM, 1);
            vx1 = xe - 24;
        } else if (w->kind == UI_ADJUST && sel) {
            lcd_ui_chev(xe - 14, ry + 8, UI_ACCENT, 1);
            vx1 = xe - 26;
        }
        if (w->value && w->value[0]) {
            const int room = (vx1 - (x0 + 12 + (int)strlen(w->label) * 8) - 16) / 8;
            char v[64];
            int len = (int)strlen(w->value);
            if (room < 3) len = 0;
            else if (len > room) {
                snprintf(v, sizeof(v), "%.*s..", room - 2, w->value);
                len = room;
            } else {
                snprintf(v, sizeof(v), "%s", w->value);
            }
            if (len) {
                const int vx = s_ui_vcol ? x0 + s_ui_vcol : vx1 - len * 8;
                lcd_ui_text(vx, ry + 4, v, sel ? UI_SELVAL : UI_DIM);
                if (w->kind == UI_ADJUST && sel) lcd_ui_chev(vx - 12, ry + 8, UI_ACCENT, -1);
                if (w->mark) lcd_ui_dot(vx - 10, ry + 12, UI_ACCENT);
            }
        }
    }
    return y + UI_RH * n;
}

// The key hints along the bottom.
enum { UK_UPDOWN = 1, UK_LEFTRIGHT = 2 };
typedef struct { int kind; const char *cap; const char *hint; } UiKey;

static void ui_footer(const UiKey *k, int n) {
    const int fy = 372;
    int x = UI_L;
    for (int i = 0; i < n; i++) {
        const int w = k[i].kind ? 30 : (int)strlen(k[i].cap) * 8 + 12;
        lcd_ui_fill(x, fy, x + w, fy + 20, UI_KEY, 4);
        if (k[i].kind == UK_UPDOWN) {
            lcd_ui_tri(x + 6, fy + 4, UI_TEXT, -1);
            lcd_ui_tri(x + 17, fy + 12, UI_TEXT, 1);
        } else if (k[i].kind == UK_LEFTRIGHT) {
            lcd_ui_chev(x + 8, fy + 7, UI_TEXT, -1);
            lcd_ui_chev(x + 18, fy + 7, UI_TEXT, 1);
        } else {
            lcd_ui_text(x + 6, fy + 2, k[i].cap, UI_TEXT);
        }
        x = lcd_ui_text(x + w + 8, fy + 2, k[i].hint, UI_DIM) + 24;
    }
}

// A list longer than its group: a thin track and thumb inside the group's right edge.
static void ui_scrollbar(int x1, int y0, int y1, int top, int vis, int total) {
    if (total <= vis) return;
    const int t0 = y0 + 4, t1 = y1 - 4, h = t1 - t0;
    int th = h * vis / total;
    if (th < 8) th = 8;
    const int tp = t0 + (h - th) * top / (total - vis);
    lcd_ui_fill(x1 - 6, t0, x1 - 4, t1, UI_LINE, 0);
    lcd_ui_fill(x1 - 6, tp, x1 - 4, tp + th, UI_DIM, 0);
}

// Room for a card full of images: past this many the rest are simply not
// listed, which looks like files going missing. Kept in PSRAM - 64KB is
// nothing there and a lot in internal RAM.
#define MAX_ENTRIES 256
#define NAME_LEN    (1 + 255 + 1)    // '/' + the longest d_name FatFs gives + NUL
#define VISIBLE     9                // rows of a list on screen

EXT_RAM_BSS_ATTR static char s_names[MAX_ENTRIES][NAME_LEN];
static int  s_count;

#define ROMNAME_LEN 40             // main.cpp sizes the buffers to match
extern "C" char g_bios_file[ROMNAME_LEN];   // "" = the built-in default
extern "C" char g_font_file[ROMNAME_LEN];
static void save_settings(void);   // defined below; used by the HDD-eject reboot
extern "C" void usb_msc_request(int mode);  // usb_msc.cpp: arm the USB Mode boot flag
extern "C" bool screenshot_save(char *name, size_t cap);  // screenshot.cpp
extern "C" bool fdd_ext_is(const char *name);
extern "C" bool fdd_ext_mount(int drv, const char *path);
extern "C" void sd_unmount(void);           // sd_tab5.cpp: leave the card idle
// The full-screen modes indent their text to where the menu rows start.
#define MODE_INDENT "  "

#define USB_MODE_SD    1
#define USB_MODE_IMAGE 2

static const char *drv_label(int d) {
    return d == 0 ? "FDD1" : d == 1 ? "FDD2" : d == 3 ? "CD-ROM" : "HDD ";
}

static const char *drv_current(int d) {
    if (d == 2) return g_settings.hdd_image[0][0] ? (const char *)g_settings.hdd_image[0] : "(empty)";
    if (d == 3) return g_settings.cd_image[0] ? (const char *)g_settings.cd_image : "(empty)";
    return g_settings.fdd_image[d][0] ? (const char *)g_settings.fdd_image[d] : "(empty)";
}

static int ext_match(const char *name, int drive) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    char e[8]; int i = 0;
    for (const char *p = dot + 1; *p && i < 7; p++) e[i++] = tolower((uint8_t)*p);
    e[i] = 0;
    if (drive == 3)
        return !strcmp(e, "iso") || !strcmp(e, "cue");
    if (drive == 2)
        return !strcmp(e, "hdi") || !strcmp(e, "nhd") || !strcmp(e, "thd") || !strcmp(e, "vhd");
    return !strcmp(e, "nfd") || !strcmp(e, "d88") || !strcmp(e, "fdi") || !strcmp(e, "fdd") || !strcmp(e, "hdm") ||
           !strcmp(e, "ftk");
}

// Rows 0..10 are all written below, so this repaints in place: no clear, no
// black frame between the old screen and the new one.
// Shown on the screenshot row once one has been taken.
static char s_last_shot[24] = {0};

// The main screen: two columns of grouped settings. sel 0..21 runs down the left column and on into the right.
static int sound_step(int current,int direction,bool wrap) {
    static const int order[]={0,2,3,4};
    int at=0;for(int i=0;i<4;i++)if(order[i]==current)at=i;
    int next=at+direction;
    if(wrap)next=(next+4)%4;
    if(next<0)next=0;
    if(next>3)next=3;
    return order[next];
}

static void draw_drives(int sel) {
    static const char *const l200[] = {"Original", "Doubled", "Interpolated"};
    static const char *const sfx[] = {"Original", "Original", "Hall", "Synthesizer (HVY)", "Orchestra (HVY)"};
    static const char *const oskn[] = {"Off", "Portrait", "Landscape", "Landscape-GAME"};
    static const char *const msp[] = {"Low", "Medium", "High"};
    char clk[16], cpu[48], bios[32], font[32], bl[16], vol[16];
    const unsigned mhz100 = (unsigned)((uint64_t)g_settings.base_clock * g_settings.clock_mul / 10000u);
    snprintf(clk, sizeof(clk), "%u.%02uMHz x%u", mhz100 / 100, mhz100 % 100, (unsigned)g_settings.clock_mul);
    const bool clock8 = g_settings.base_clock == CLK_BASE8;
    const bool clock_pending = g_settings.base_clock != g_mach.base_clock;
    const char *family = clock8 ? (clock_pending ? "8MHz (Reset)" : "8MHz") :
                                  (clock_pending ? "10MHz (Reset)" : "10MHz");
    // Read at boot, so a change waits for RESET - and says so.
    snprintf(cpu, sizeof(cpu), "%s%s", cpu_name(g_cpu_mode), g_cpu_mode != g_cpu_mode_boot ? " (Reset)" : "");
    // The ROMs in use, chosen from whatever BIOS*.ROM / FONT*.ROM sit in the SD root; read once at machine_init.
    snprintf(bios, sizeof(bios), "%s", g_bios_file[0] ? g_bios_file : "Built-in");
    snprintf(font, sizeof(font), "%s", g_font_file[0] ? g_font_file : "Built-in");
    snprintf(bl, sizeof(bl), "%d%%", g_backlight_pct);
    snprintf(vol, sizeof(vol), "%d%%", audio_get_volume());
    const char *d[4];
    for (int i = 0; i < 4; i++) {
        const char *p = drv_current(i);
        d[i] = !strcmp(p, "(empty)") ? "Empty" : (p[0] == '/' ? p + 1 : p);
    }
    char cdv[CFG_PATH_MAX + 8];
    snprintf(cdv, sizeof(cdv), "%s%s", d[3], (g_settings.cd_image[0] != 0) != (atapi_fitted() != 0) ? " (Reset)" : "");
    const UiRow disks[5] = {
        { "FDD1", d[0], UI_ACTION, 0, 0 }, { "FDD2", d[1], UI_ACTION, 0, 0 }, { "HDD", d[2], UI_ACTION, 0, 0 },
        { "CD-ROM", cdv, UI_ACTION, 0, 0 }, { "Create New Disk", NULL, UI_ACTION, 0, 0 },
    };
    const UiRow sys[6] = {
        { "CPU Clock", clk, UI_ADJUST, 0, 0 }, { "Clock Family", family, UI_ADJUST, 0, 0 },
        { "CPU", cpu, UI_ADJUST, 0, 0 },
        { "BIOS", bios, UI_ACTION, 0, 0 }, { "Font", font, UI_ACTION, 0, 0 },
        { "Touch Mouse Speed", msp[touch_mouse_speed()], UI_ADJUST, 0, 0 },
    };
    const UiRow av[6] = {
        { "Backlight", bl, UI_ADJUST, 0, 0 }, { "Volume", vol, UI_ADJUST, 0, 0 },
        // How a 200-line screen's missing lines are drawn (main.cpp).
        { "200-Line Graphics", l200[(g_line200 >= 0 && g_line200 <= 2) ? g_line200 : 0], UI_ADJUST, 0, 0 },
        // A colouring for the sound on its way out (audio_codec.cpp).
        { "FM Sound", sfx[audio_get_fx()], UI_ADJUST, 0, 0 },
        // The FDD1 / FDD2 / CD-ROM / HDD lamps under the picture (lcd_mipi.cpp).
        { "Drive Lamps", lcd_lamps_enabled() ? "On" : "Off", UI_ADJUST, 0, 0 },
        { "Screen Keyboard", oskn[lcd_osk_mode()], UI_ADJUST, 0, 0 },
    };
    // Screenshot shows the last file written, so the row itself is the confirmation. The readers reboot into the
    // USB modes (usb_msc.cpp); Reset sits last, where an action that throws the machine away belongs.
    const UiRow tools[5] = {
        { "Screenshot", s_last_shot[0] ? s_last_shot : NULL, UI_ACTION, 0, 0 },
        { "State Save", NULL, UI_ACTION, 0, 0 },
        { "SD Card Reader", NULL, UI_ACTION, 0, 0 }, { "Disk Image Reader", NULL, UI_ACTION, 0, 0 },
        { "Reset", "Save & Reboot", UI_ACTION, 0, 0 },
    };
    ui_header(NULL);
    const int lx0 = UI_L, lx1 = 308, rx0 = 332, rx1 = UI_R;
    int y = 50;
    ui_section(lx0, lx1, y, "DISKS", NULL);
    y = ui_group(lx0, lx1, y + 22, disks, 5, sel, 0) + 8;   // 8, as on the right: SYSTEM has six rows
    ui_section(lx0, lx1, y, "SYSTEM", NULL);
    ui_group(lx0, lx1, y + 22, sys, 6, sel - 5, 0);
    y = 50;
    ui_section(rx0, rx1, y, "DISPLAY & SOUND", NULL);
    y = ui_group(rx0, rx1, y + 22, av, 6, sel - 11, 0) + 8;   // 8, not 14: TOOLS has five rows now and must clear the key caps
    ui_section(rx0, rx1, y, "TOOLS", NULL);
    ui_group(rx0, rx1, y + 22, tools, 5, sel - 17, 0);
    static const UiKey keys[4] = {
        { UK_UPDOWN, NULL, "Select" }, { UK_LEFTRIGHT, NULL, "Change" }, { 0, "Enter", "OK" }, { 0, "Esc", "Close" },
    };
    ui_footer(keys, 4);
}

static int name_cmp(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}

// Collect matching SD images into s_names. Returns entry count.
static int scan_images(int drive) {
    s_count = 0;
    // Inject the currently-mounted image first (belt-and-suspenders; de-duped
    // below) so a drive never shows "No Image" while an image is mounted.
    const char *mnt = (drive == 3) ? (const char *)g_settings.cd_image
                    : (drive == 2) ? (const char *)g_settings.hdd_image[0]
                                   : (const char *)g_settings.fdd_image[drive];
    if (mnt && mnt[0]) {
        const char *mb = strrchr(mnt, '/'); mb = mb ? mb + 1 : mnt;
        if (ext_match(mb, drive)) {
            snprintf(s_names[s_count], NAME_LEN, "/%s", mb);
            s_count++;
        }
    }
    // List the SD root with POSIX readdir. Unlike Arduino SD's openNextFile()
    // (which open()s each entry and stops at the first file already held open by
    // the emulator, e.g. FONT.ROM/HDD.NHD), readdir only reads directory entries,
    // so every image is listed whether or not it is currently mounted.
    DIR *dir = opendir("/sd");        // Arduino SD VFS mountpoint (see dosio_sd.cpp)
    if (!dir) return s_count;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_count < MAX_ENTRIES) {
        const char *base = ent->d_name;
        if (!ext_match(base, drive)) continue;
        int dup = 0;                        // s_names entries are "/BASE"
        for (int i = 0; i < s_count; i++)
            if (!strcmp(s_names[i] + 1, base)) { dup = 1; break; }
        if (!dup) {
            snprintf(s_names[s_count], NAME_LEN, "/%s", base);
            s_count++;
        }
    }
    closedir(dir);
    // By name, whatever order the card's directory happens to hold them in -
    // a file copied on later, or the mounted one put in above, would
    // otherwise sit out of place.
    qsort(s_names, s_count, NAME_LEN, name_cmp);
    return s_count;
}

// ROM chooser. The SD root is listed for BIOS*.ROM / FONT*.ROM (case-insensitive),
// so several dumps can sit on the card at once - BIOS.ROM and BIOS_9821.ROM
// both match - and the choice is a filename rather than a flag. The compatible
// BIOS is not among them: it is built into the firmware and is what runs when
// nothing here is chosen.
static int scan_roms(const char *prefix) {
    s_count = 0;
    DIR *dir = opendir("/sd");
    if (!dir) return 0;
    const size_t pl = strlen(prefix);
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_count < MAX_ENTRIES) {
        const char *b = ent->d_name;
        if (strncasecmp(b, prefix, pl) != 0) continue;
        const char *dot = strrchr(b, '.');
        if (!dot || strcasecmp(dot, ".rom") != 0) continue;
        snprintf(s_names[s_count], NAME_LEN, "/%s", b);
        s_count++;
    }
    closedir(dir);
    return s_count;
}

// which: 0 = BIOS, 1 = FONT. Writes the chosen name into g_bios_file/g_font_file.
static void browse_rom(int which) {
    const char *prefix = which ? "FONT" : "BIOS";
    char *dst = which ? g_font_file : g_bios_file;
    scan_roms(prefix);
    lcd_menu_clear();
    // One row above whatever is on the card: the ROM built into the firmware.
    // Without it a choice could be made but never taken back, since the list
    // is otherwise SD files only.
    const int total = s_count + 1;
    int sel = 0, top = 0;
    for (;;) {
        ui_header(which ? "Font" : "BIOS");
        ui_section(UI_L, UI_R, 50, "ROM IMAGES", "Applies after Reset");
        {
            const int vis = total - top < VISIBLE ? total - top : VISIBLE;
            UiRow rows[VISIBLE];
            for (int i = 0; i < vis; i++) {
                const int idx = top + i;
                const char *nm = idx == 0 ? "Built-in" : s_names[idx - 1];
                const bool used = idx == 0 ? !dst[0] : !strcmp(dst, s_names[idx - 1]);
                rows[i] = (UiRow){ nm[0] == '/' ? nm + 1 : nm, used ? "In use" : "", UI_VALUE, (uint8_t)used, 0 };
            }
            const int y1 = ui_group(UI_L, UI_R, 72, rows, vis, sel - top, total > VISIBLE ? 12 : 0);
            ui_scrollbar(UI_R, 72, y1, top, VISIBLE, total);
            static const UiKey keys[3] = { { UK_UPDOWN, NULL, "Select" }, { 0, "Enter", "Choose" }, { 0, "Esc", "Back" } };
            ui_footer(keys, 3);
        }
        uint8_t nk, dn;
        for (;;) {
            if (menu_key_pop(&nk, &dn) && dn) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (nk == NK_ESC) return;
        if (key_is_up(nk)   && sel > 0)            sel--;
        if (key_is_down(nk) && sel < total - 1)  sel++;
        if (sel < top) top = sel;
        if (sel >= top + VISIBLE) top = sel - VISIBLE + 1;
        if (nk == NK_RET) {
            if (sel == 0) {
                dst[0] = 0;            // empty = the firmware's own ROM
                ets_printf("menu: %s ROM <- built-in\n", prefix);
            } else {
                snprintf(dst, ROMNAME_LEN, "%s", s_names[sel - 1]);
                ets_printf("menu: %s ROM <- %s\n", prefix, dst);
            }
            return;
        }
    }
}

// SD images followed by the eject action. HDD eject reboots.
static void draw_files(int drive, int sel, int top) {
    const char *f0 = (const char *)g_settings.fdd_image[0], *f1 = (const char *)g_settings.fdd_image[1];
    const char *here = (drive == 3) ? (const char *)g_settings.cd_image
                     : (drive == 2) ? (const char *)g_settings.hdd_image[0] : drive ? f1 : f0;
    const char *other = (drive >= 2) ? "" : drive ? f0 : f1;
    if (top > s_count - VISIBLE) top = s_count > VISIBLE ? s_count - VISIBLE : 0;
    if (top < 0) top = 0;
    const int vis = s_count - top < VISIBLE ? s_count - top : VISIBLE;

    ui_header(drv_label(drive));
    char note[32];
    snprintf(note, sizeof(note), "%d on the SD card", s_count);
    ui_section(UI_L, UI_R, 50, drive == 3 ? "CD-ROM IMAGES" : drive == 2 ? "HARD DISK IMAGES" : "DISK IMAGES", note);
    int y = 72;
    if (vis > 0) {
        UiRow rows[VISIBLE];
        static char tag[VISIBLE][12];
        for (int i = 0; i < vis; i++) {
            const char *nm = s_names[top + i];
            const bool in_here = here[0] && !strcmp(nm, here);
            const bool in_other = other[0] && !strcmp(nm, other);
            snprintf(tag[i], sizeof(tag[i]), "%s", in_here ? "In use" : in_other ? (drive ? "In FDD1" : "In FDD2") : "");
            rows[i] = (UiRow){ nm[0] == '/' ? nm + 1 : nm, tag[i], UI_VALUE, (uint8_t)in_here, 0 };
        }
        const int y1 = ui_group(UI_L, UI_R, y, rows, vis, sel - top, 12);
        ui_scrollbar(UI_R, y, y1, top, VISIBLE, s_count);
        y = y1 + 12;
    } else {
        ui_section(UI_L, UI_R, y + 4, "No disk images on the SD card.", NULL);
        y += 32;
    }
    UiRow more[1] = {
        { drive == 2 ? "Eject HDD and Reboot" : "Eject", NULL, UI_ACTION, 0, 0 },
    };
    ui_group(UI_L, UI_R, y, more, 1, sel - s_count, 0);
    static const UiKey keys[4] = {
        { UK_UPDOWN, NULL, "Select" }, { UK_LEFTRIGHT, NULL, "Page" }, { 0, "Enter", "Insert" }, { 0, "Esc", "Back" },
    };
    ui_footer(keys, 4);
}

static void apply_image(int drive, const char *path) {
    if (drive == 3) {
        // A disc goes in or out at once when the drive is fitted; otherwise the drive comes with the next reset.
        pcs_copy(g_settings.cd_image, path ? path : "", sizeof(g_settings.cd_image));
        if (atapi_fitted()) atapi_insert(path);
    } else if (drive == 2) {
        // The HDD interfaces open g_settings.hdd_image[0] at the next reset (dev/sasi_bind.c).
        pcs_copy(g_settings.hdd_image[0], path ? path : "", sizeof(g_settings.hdd_image[0]));
    } else if (path && fdd_ext_is(path)) {
        if (!fdd_ext_mount(drive, path)) {
            g_settings.fdd_image[drive][0] = '\0';
        }
    } else if (path) {
        // Out, then in 0.4s later (drv_fd_tick), as a hand swapping
        // disks: DOS only rereads a drive it has seen go not-ready.
        drv_fd_swap(drive, path);
        pcs_copy(g_settings.fdd_image[drive], path, sizeof(g_settings.fdd_image[drive]));
    } else {
        drv_fd_swap(drive, NULL);   // eject
        g_settings.fdd_image[drive][0] = '\0';
    }
    ets_printf("menu: %s <- %s\n", drv_label(drive), path ? path : "(eject)");
}

// File browser for one drive; returns when an image was applied or ESC.
static void browse(int drive) {
    scan_images(drive);
    int total = s_count + 1;
    lcd_menu_clear();                             // leave the drive list cleanly (no overlap)
    if (total == 0) {
        mline(2, "  no images on SD", COL_WHITE, COL_BLACK);
        vTaskDelay(pdMS_TO_TICKS(1200));
        return;
    }
    int sel = 0, top = 0;
    draw_files(drive, sel, top);
    for (;;) {
        uint8_t nk, dn;
        if (!menu_key_pop(&nk, &dn)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (!dn) continue;
        if (nk == NK_ESC) return;
        if (key_is_up(nk))   sel = (sel > 0) ? sel - 1 : total - 1;
        if (key_is_down(nk)) sel = (sel + 1 < total) ? sel + 1 : 0;
        // A card with many images: LEFT/RIGHT go back/forward a page.
        if (key_is_left(nk)) {
            sel = (sel > VISIBLE) ? sel - VISIBLE : 0;
            top = (top > VISIBLE) ? top - VISIBLE : 0;
        }
        if (key_is_right(nk)) {
            sel = (sel + VISIBLE < total) ? sel + VISIBLE : total - 1;
            top += VISIBLE;
            if (top > total - VISIBLE) top = (total > VISIBLE) ? total - VISIBLE : 0;
        }
        if (sel < top) top = sel;
        if (sel >= top + VISIBLE) top = sel - VISIBLE + 1;
        if (nk == NK_RET) {
            if (sel == s_count) {                 // eject entry
                if (drive == 2) {
                    // Eject the running HDD: persist the now-empty HDD and reboot,
                    // so the machine comes up in N88-BASIC / a floppy instead of
                    // live-unmounting the disk the OS is booted from.
                    g_settings.hdd_image[0][0] = '\0';
                    lcd_menu_clear();
                    mline(2, "  HDD ejected - rebooting...", COL_WHITE, COL_BLACK);
                    // Same reason as the reboot out of USB Mode: the framebuffer
                    // lives in PSRAM, so without pushing the cache out the clear
                    // and the text never reach it, and the panel keeps whatever
                    // the cache happened to evict - half the old menu, left as
                    // character debris.
                    lcd_menu_flush();
                    save_settings();
                    vTaskDelay(pdMS_TO_TICKS(800));
                    // And hand the card back, so the boot that follows can
                    // initialise it from scratch rather than find it busy.
                    sd_unmount();
                    panel_tab5_blank_early();
                    esp_restart();
                }
                apply_image(drive, NULL);         // FDD: live eject
            } else {
                apply_image(drive, s_names[sel]);
            }
            vTaskDelay(pdMS_TO_TICKS(600));
            return;
        }
        draw_files(drive, sel, top);
    }
}

// nkey -> ASCII for filename entry (uppercase letters, digits, '-')
static char nkey_ascii(uint8_t nk) {
    if (nk >= 0x01 && nk <= 0x09) return (char)('1' + nk - 1);
    if (nk == 0x0a) return '0';
    if (nk == 0x0b) return '-';
    static const char letters[0x30] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                  // 0x00-0x0f
        'q','w','e','r','t','y','u','i','o','p',0,0,0,'a','s','d', // 0x10-0x1f
        'f','g','h','j','k','l',0,0,0,'z','x','c','v','b','n','m'  // 0x20-0x2f
    };
    if (nk < 0x30 && letters[nk]) return (char)(letters[nk] - 'a' + 'A');
    switch (nk) {                        // numeric keypad digits KP0..KP9
        case 0x4e: return '0'; case 0x4a: return '1'; case 0x4b: return '2';
        case 0x4c: return '3'; case 0x46: return '4'; case 0x47: return '5';
        case 0x48: return '6'; case 0x42: return '7'; case 0x43: return '8';
        case 0x44: return '9';
    }
    return 0;
}

static uint8_t wait_key(void) {
    uint8_t nk, dn;
    for (;;) {
        if (!menu_key_pop(&nk, &dn)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (dn) return nk;
    }
}

// Create a blank .NHD image, written in small chunks with a progress line.
//
// NHD (T98-Next) header, 512 bytes: "T98HDDIMAGE.R0" + NUL at 0, a comment up to
// 10Fh, then little-endian header size (110h), cylinders (114h), heads (118h),
// sectors per track (11Ah) and sector size (11Ch) - the fields media/hddmedia.c
// reads back. The data area is zeros; DOS FORMAT makes it usable. Geometry is
// 8 heads x 17 sectors x 512 bytes, 15 cylinders per MB (8*17*512*15 = 1044480).
static void put_le(uint8_t *p, uint32_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static bool create_nhd(const char *path, UINT mb, bool *cancelled) {
    const UINT32 C = (UINT32)mb * 15;
    const UINT16 H = 8, S = 17, SS = 512;
    const uint64_t total = (uint64_t)C * H * S * SS;

    uint8_t nhd[512];
    memset(nhd, 0, sizeof(nhd));
    memcpy(nhd, "T98HDDIMAGE.R0", 15);
    put_le(nhd + 0x110, sizeof(nhd), 4);
    put_le(nhd + 0x114, C, 4);
    put_le(nhd + 0x118, H, 2);
    put_le(nhd + 0x11a, S, 2);
    put_le(nhd + 0x11c, SS, 2);

    // Internal DMA-capable RAM when some is free (the SD driver writes straight
    // out of it); otherwise ordinary PSRAM, which dosio_sd bounces in 2KB pieces.
    const UINT chunk = 16 * 1024;
    uint8_t *work = (uint8_t *)heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!work) work = (uint8_t *)malloc(chunk);
    if (!work) return false;
    memset(work, 0, chunk);

    HFILE fh = hf_create((const char *)path);
    if (fh == NULL) { free(work); return false; }

    bool ok = (hf_write(fh, nhd, sizeof(nhd)) == sizeof(nhd));
    uint64_t left = total;
    int lastpct = -1;
    while (ok && left) {
        UINT n = (UINT)((left < chunk) ? left : chunk);
        if (hf_write(fh, work, n) != n) { ok = false; break; }
        left -= n;
        int pct = (int)(((total - left) * 100) / total);
        if (pct != lastpct) {
            char line[48];
            snprintf(line, sizeof(line), "  creating... %d%%  (ESC: cancel)", pct);
            mline(4, line, COL_WHITE, COL_BLACK);
            lastpct = pct;
        }
        uint8_t nk, dn;
        while (menu_key_pop(&nk, &dn))
            if (dn && nk == NK_ESC) { *cancelled = true; ok = false; }
    }
    hf_close(fh);
    free(work);
    if (!ok) hf_delete((const char *)path);      // no half-written image
    return ok;
}

// A blank 1.44MB floppy: a raw image of 80 cylinders x 2 heads x 18 sectors x 512 bytes,
// all zeros (unformatted for DOS: FORMAT it in the machine).
static bool create_blank_fd(const char *path) {
    static const UINT size = 80 * 2 * 18 * 512;
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    HFILE fh = hf_create((const char *)path);
    if (fh == NULL) return false;
    bool ok = true;
    for (UINT done = 0; ok && done < size; done += sizeof(zero))
        ok = (hf_write(fh, zero, sizeof(zero)) == sizeof(zero));
    hf_close(fh);
    if (!ok) hf_delete((const char *)path);
    return ok;
}

// Create a blank FD/HDD image on the SD card (name typed on USB keyboard).
static void newdisk_flow(void) {
    static const struct { const char *label; int mb; } types[] = {
        { "FD 1.44MB (2HD .HDM)", 0 },
        { "HDD 20MB (.NHD)", 20 },
        { "HDD 40MB (.NHD)", 40 },
        { "HDD 80MB (.NHD)", 80 },
    };
    int sel = 0;
    for (;;) {
        ui_header("New Disk");
        ui_section(UI_L, UI_R, 50, "TYPE", NULL);
        {
            UiRow rows[4];
            for (int i = 0; i < 4; i++) rows[i] = (UiRow){ types[i].label, NULL, UI_ACTION, 0, 0 };
            ui_group(UI_L, UI_R, 72, rows, 4, sel, 0);
            static const UiKey keys[3] = { { UK_UPDOWN, NULL, "Select" }, { 0, "Enter", "Next" }, { 0, "Esc", "Cancel" } };
            ui_footer(keys, 3);
        }
        uint8_t nk = wait_key();
        if (nk == NK_ESC) return;
        if (key_is_up(nk)   && sel > 0) sel--;
        if (key_is_down(nk) && sel < 3) sel++;
        if (nk == NK_RET) break;
    }
    int mb = types[sel].mb;

    // base name entry: the SD card takes long names, RET to confirm
    enum { NEWDISK_NAME_LEN = 64 };
    char name[NEWDISK_NAME_LEN + 1]; int len = 0; name[0] = 0;
    bool shift = false;
    int shown = -1;
    for (;;) {
        if (shown != len) {            // only when the name changed: a repaint is the whole screen
            shown = len;
            ui_header("New Disk");
            ui_section(UI_L, UI_R, 50, "FILE NAME", "A-Z 0-9 - _");
            char line[NEWDISK_NAME_LEN + 8];
            snprintf(line, sizeof(line), "%s_", name);
            const UiRow row = { line, NULL, UI_VALUE, 0, 0 };
            ui_group(UI_L, UI_R, 72, &row, 1, -1, 0);
            static const UiKey keys[3] = { { 0, "Enter", "Create" }, { 0, "BS", "Delete" }, { 0, "Esc", "Cancel" } };
            ui_footer(keys, 3);
        }
        uint8_t nk, dn;
        if (!menu_key_pop(&nk, &dn)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (nk == 0x70) { shift = dn; continue; }   // SHIFT: held for Shift+'-'
        if (!dn) continue;
        if (nk == NK_ESC) return;
        if (nk == NK_RET) { if (len > 0) break; continue; }
        if (nk == 0x0e) { if (len > 0) name[--len] = 0; continue; }  // Backspace
        char c = nkey_ascii(nk);
        if (nk == 0x33 || (shift && nk == 0x0b)) c = '_';  // JIS "_ろ" key, US Shift+'-'
        if (c && len < NEWDISK_NAME_LEN) { name[len++] = c; name[len] = 0; }
    }

    char path[NEWDISK_NAME_LEN + 8];
    snprintf(path, sizeof(path), "/%s.%s", name, mb ? "NHD" : "HDM");
    lcd_menu_clear();
    mline(1, "  New blank disk:", COL_WHITE, COL_BLACK);
    mline(2, path, COL_WHITE, COL_BLACK);

    // stat() rather than SD.exists(): the card is mounted on the VFS at /sd,
    // so the standard call reaches it and no Arduino layer is needed.
    struct stat st_;
    char full_[160];
    snprintf(full_, sizeof(full_), "/sd%s", path);
    if (stat(full_, &st_) == 0) {
        mline(4, "  exists! aborted", COL_WHITE, COL_BLACK);
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }
    mline(4, "  creating...", COL_WHITE, COL_BLACK);
    bool ok, cancelled = false;
    if (mb == 0) {
        ok = create_blank_fd(path);
    } else {
        ok = create_nhd(path, (UINT)mb, &cancelled);
    }
    mline(4, cancelled ? "  cancelled"
                    : ok       ? "  done (format in DOS)"
                               : "  FAILED (card full or write error)",
                  COL_WHITE, COL_BLACK);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

// Persist the 5 menu items — CPU multiple, scaler mode, and the FDD1/FDD2/HDD
// mounts — called on menu ESC exit and on RESET. main.cpp reloads these at boot
// and restores the disks. NVS commit only writes flash when a value actually
// changed, so this is wear-safe.
static void save_settings(void) {
    nvs_handle_t nh;
    if (nvs_open("pc98", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u8(nh, "multiple", (uint8_t)g_settings.clock_mul);
        nvs_set_u8(nh, "clkbase8", g_settings.base_clock == CLK_BASE8 ? 1 : 0);
        nvs_set_u8(nh, "cpumode", (uint8_t)g_cpu_mode);
        nvs_set_u8(nh, "scaler", (uint8_t)lcd_get_scale_mode());
        nvs_set_u8(nh, "backlight", (uint8_t)g_backlight_pct);
        nvs_set_u8(nh, "volume", (uint8_t)audio_get_volume());
        nvs_set_u8(nh, "line200", (uint8_t)g_line200);
        nvs_set_u8(nh, "soundfx", (uint8_t)audio_get_fx());
        nvs_set_u8(nh, "lamps", lcd_lamps_enabled() ? 1 : 0);
        nvs_set_u8(nh, "osk", (uint8_t)lcd_osk_mode());
        nvs_set_u8(nh, "mousespd", (uint8_t)touch_mouse_speed());
        extern bool g_sd_ok;
        if (g_sd_ok) {      // no card: every name reads as missing, keep what was stored
            nvs_set_str(nh, "biosfile", g_bios_file);
            nvs_set_str(nh, "fontfile", g_font_file);
            // Current mounts (empty drive => empty string; restored as empty at boot).
            nvs_set_str(nh, "fdd0", (const char *)g_settings.fdd_image[0]);
            nvs_set_str(nh, "fdd1", (const char *)g_settings.fdd_image[1]);
            nvs_set_str(nh, "hdd",  (const char *)g_settings.hdd_image[0]);
            nvs_set_str(nh, "cd",   (const char *)g_settings.cd_image);
        }
        nvs_commit(nh);
        nvs_close(nh);
    }
}

// Disk Image Reader Mode: which drive's image to hand the PC. Returns the drive
// (0 = FDD1, 1 = FDD2, 2 = HDD) or -1 on ESC, and leaves the choice in NVS for
// usb_image.cpp to read after the reboot. Only a drive with an image file in it
// can be chosen.
static int choose_image_drive(void) {
    static const int order[3] = { 2, 0, 1 };      // HDD first, as it always was
    auto usable = [](int d) {
        const char *p = (d == 2) ? (const char *)g_settings.hdd_image[0] : (const char *)g_settings.fdd_image[d];
        return p[0] != 0 && !(d < 2 && fdd_ext_is(p));
    };
    int sel = 0;
    while (sel < 2 && !usable(order[sel])) {
        sel++;
    }
    lcd_menu_clear();
    for (;;) {
        ui_header("Disk Image Reader");
        ui_section(UI_L, UI_R, 50, "OPEN WHICH DRIVE ON THE PC", NULL);
        {
            UiRow rows[3];
            for (int i = 0; i < 3; i++) {
                const int d = order[i];
                const char *p = drv_current(d);
                rows[i] = (UiRow){ drv_label(d), usable(d) ? (p[0] == '/' ? p + 1 : p) : "No image file", UI_ACTION, 0,
                                   (uint8_t)!usable(d) };
            }
            ui_group(UI_L, UI_R, 72, rows, 3, sel, 0);
            static const UiKey keys[3] = { { UK_UPDOWN, NULL, "Select" }, { 0, "Enter", "Start" }, { 0, "Esc", "Back" } };
            ui_footer(keys, 3);
        }
        const uint8_t nk = wait_key();
        if (nk == NK_ESC) {
            return -1;
        }
        if (key_is_up(nk) && sel > 0) sel--;
        if (key_is_down(nk) && sel < 2) sel++;
        if (nk == NK_RET && usable(order[sel])) {
            nvs_handle_t nh;
            if (nvs_open("pc98", NVS_READWRITE, &nh) == ESP_OK) {
                nvs_set_u8(nh, "usbimgdrv", (uint8_t)order[sel]);
                nvs_commit(nh);
                nvs_close(nh);
            }
            return order[sel];
        }
    }
}

// Layout of the slot list: the screen is the 640x400 canvas, so the list and a full-size thumbnail both fit.
#define ST_SLOTS    10
#define ST_SEC_Y    50
#define ST_Y0       72
#define ST_KEY_SEL  "Select"
static int st_div(void) { return 1; }
static int st_vis(void) { return ST_SLOTS; }

// ---- State Save: ten slots on the SD card ---------------------------------------------------------------------
//
// A slot file is /sd/PC98/STATE/SLOTnn.STA: a StHdr, the thumbnail's pixels (when the header says there is one),
// then the stream pc98_state_save() writes. The stream is only good for the firmware that wrote it, so the header
// carries the build's ELF SHA and a slot made by another build is listed but refuses to load.
// The files are written and read here, from the menu with the machine stopped, never inside machine_frame.
extern bool g_sd_ok;
extern "C" const void *lcd_last_frame(int *w, int *h, int *stride);   // lcd_*.cpp: the last frame handed over
extern "C" void lcd_ui_blit(int x, int y, int w, int h, const uint16_t *src);   // lcd_*.cpp

#define ST_DIR      "/sd/PC98/STATE"
#define ST_THUMB_W  160                  // the screen at 1/4
#define ST_THUMB_H  100
#define ST_VER      1
#define ST_IOBUF    (16 * 1024)   // aligned DMA memory: from anything else the SD driver writes one sector at a time (~0.25 MB/s)

typedef struct {
    char     magic[4];                   // "98TS"
    uint32_t ver;
    uint8_t  elf_sha[32];                // the build that wrote it
    int64_t  time;                       // seconds since the epoch; 0 = the clock was not set
    uint32_t seq;                        // saves counted up over the slots: what is shown when there is no clock
    uint32_t thumb_w, thumb_h;           // 0 = no thumbnail follows
    char     name[96];                   // FDD1's image, or the HDD's when FDD1 is empty
    char     fw_ver[32];                 // the writing build's version string, for the "other firmware" prompt
} StHdr;

typedef struct {
    uint8_t  used;                       // a file is there
    uint8_t  bad;                        // ...but not one of ours
    uint8_t  other;                      // ...written by another build
    uint8_t  thumb;                      // ...and has a thumbnail
    char     fw_ver[32];
    int64_t  time;
    uint32_t seq;
    char     name[96];
} StSlot;

static StSlot s_st[ST_SLOTS];
static uint16_t *s_th_buf;               // [0] 160x100 shown, [1] 160x100 being saved, [2] 80x50 shown on a small panel
static int s_th_for = -1;                // the slot s_th_buf[0] holds (-1 none)

#define TH_PIX (ST_THUMB_W * ST_THUMB_H)
static uint16_t *th_shown(void) { return s_th_buf; }
static uint16_t *th_new(void)   { return s_th_buf + TH_PIX; }
static uint16_t *th_small(void) { return s_th_buf + 2 * TH_PIX; }

static void st_path(char *out, size_t cap, int slot, bool tmp) {
    snprintf(out, cap, ST_DIR "/SLOT%02d.%s", slot + 1, tmp ? "TMP" : "STA");
}

static bool st_clock_ok(int64_t t) {
    return t >= 1609459200;              // 2021-01-01: anything earlier is a clock that was never set
}

// Average n x n blocks of an RGB565 picture into dw x dh.
static void st_reduce(const uint16_t *src, int stride, int dw, int dh, int n, uint16_t *dst) {
    const int cnt = n * n;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            int r = 0, g = 0, b = 0;
            for (int j = 0; j < n; j++) {
                const uint16_t *p = src + (size_t)(y * n + j) * stride + x * n;
                for (int i = 0; i < n; i++) {
                    const unsigned v = p[i];
                    r += v >> 11;
                    g += (v >> 5) & 63;
                    b += v & 31;
                }
            }
            dst[y * dw + x] = (uint16_t)(((r / cnt) << 11) | ((g / cnt) << 5) | (b / cnt));
        }
    }
}

// Read one slot's header. Reading never fails the list: a file that is not ours is shown as such.
static void st_scan_one(int i) {
    StSlot *s = &s_st[i];
    memset(s, 0, sizeof(*s));
    char path[64];
    st_path(path, sizeof(path), i, false);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    s->used = 1;
    StHdr h;
    if (fread(&h, 1, sizeof(h), f) != sizeof(h) || memcmp(h.magic, "98TS", 4) != 0 || h.ver != ST_VER) {
        s->bad = 1;
    } else {
        s->other = memcmp(h.elf_sha, esp_app_get_description()->app_elf_sha256, 32) != 0;
        s->thumb = h.thumb_w == ST_THUMB_W && h.thumb_h == ST_THUMB_H;
        s->time = h.time;
        s->seq = h.seq;
        snprintf(s->name, sizeof(s->name), "%s", h.name);
        snprintf(s->fw_ver, sizeof(s->fw_ver), "%s", h.fw_ver);
    }
    fclose(f);
}

static bool st_get(FILE *f, void *p, size_t n);

// The picture of the selected slot, into the buffers the panel draws from. Quiet failure: the box says "No image".
static void st_load_thumb(int i, int div) {
    s_th_for = -1;
    if (!s_th_buf || !s_st[i].used || s_st[i].bad || !s_st[i].thumb) return;
    char path[64];
    st_path(path, sizeof(path), i, false);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    bool ok = fseek(f, sizeof(StHdr), SEEK_SET) == 0 &&
              st_get(f, th_shown(), (size_t)TH_PIX * 2);
    fclose(f);
    if (!ok) return;
    if (div == 2) st_reduce(th_shown(), ST_THUMB_W, ST_THUMB_W / 2, ST_THUMB_H / 2, 2, th_small());
    s_th_for = i;
}

static void st_free_bufs(void) {
    heap_caps_free(s_th_buf);
    s_th_buf = NULL;
    s_th_for = -1;
}

static void st_alloc_bufs(void) {
    if (!s_th_buf) {
        s_th_buf = (uint16_t *)heap_caps_malloc((size_t)TH_PIX * 2 * 2 + (TH_PIX / 4) * 2, MALLOC_CAP_SPIRAM);
    }
}

// "2026-11-10 12:34", "11-10 12:34", or "#12" when the clock was never set.
static void st_when(const StSlot *s, bool full, char *out, size_t cap) {
    if (st_clock_ok(s->time)) {
        const time_t t = (time_t)s->time;
        struct tm tmv;
        localtime_r(&t, &tmv);
        if (full) {
            snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
        } else {
            snprintf(out, cap, "%02d-%02d %02d:%02d", tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
        }
    } else {
        snprintf(out, cap, "#%u", (unsigned)s->seq);
    }
}

static void st_draw(int sel, int top) {
    const int vis = st_vis(), div = st_div();
    const int tw = ST_THUMB_W / div, th = ST_THUMB_H / div;
    const int lx1 = UI_R - tw - 16;
    const bool wide = (UI_R - UI_L) >= 560;       // room for the image name in the row

    ui_header("State Save");
    ui_section(UI_L, UI_R, ST_SEC_Y, "SLOTS", NULL);
    UiRow rows[ST_SLOTS];
    static char lbl[ST_SLOTS][12], val[ST_SLOTS][96];
    for (int i = 0; i < vis; i++) {
        const StSlot *s = &s_st[top + i];
        char when[24];
        snprintf(lbl[i], sizeof(lbl[i]), "Slot %d", top + i + 1);
        if (!s->used) {
            snprintf(val[i], sizeof(val[i]), "Empty");
        } else if (s->bad) {
            snprintf(val[i], sizeof(val[i]), "Unreadable");
        } else {
            st_when(s, wide, when, sizeof(when));
            if (wide && s->name[0]) snprintf(val[i], sizeof(val[i]), "%s  %s", when, s->name);
            else snprintf(val[i], sizeof(val[i]), "%s", when);
        }
        rows[i] = (UiRow){ lbl[i], val[i], UI_VALUE, 0, (uint8_t)!s->used };
    }
    s_ui_vcol = 12 + 7 * 8 + 16;          // after "Slot 10"
    const int y1 = ui_group(UI_L, lx1, ST_Y0, rows, vis, sel - top, ST_SLOTS > vis ? 12 : 0);
    s_ui_vcol = 0;
    ui_scrollbar(lx1, ST_Y0, y1, top, vis, ST_SLOTS);

    // The selected slot's picture, and under it the image it was saved with.
    const int tx = UI_R - tw, ty = ST_Y0;
    const StSlot *s = &s_st[sel];
    lcd_ui_fill(tx, ty, tx + tw, ty + th, UI_GROUP, 5);
    if (s_th_for == sel) {
        lcd_ui_blit(tx, ty, tw, th, div == 2 ? th_small() : th_shown());
    } else {
        const char *m = !s->used ? "Empty" : "No image";
        lcd_ui_text(tx + (tw - (int)strlen(m) * 8) / 2, ty + th / 2 - 8, m, UI_CAP);
    }
    if (s->used && !s->bad) {
        const int cw = tw / 8;
        int ly = ty + th + 6;
        for (const char *p = s->name; *p && ly < ty + th + 6 + 36; ly += 18) {
            char ln[24];
            snprintf(ln, sizeof(ln), "%.*s", cw < 22 ? cw : 22, p);
            lcd_ui_text(tx, ly, ln, UI_DIM);
            p += strlen(ln);
        }
    }
    if (s->used && (s->bad || s->other)) {
        const int cw = tw / 8;
        lcd_ui_text(tx, ty + th + 6 + 36 + 4, s->bad ? "Unreadable" : cw >= 14 ? "Other firmware" : "Other FW", UI_ACCENT);
    }
    static const UiKey keys[3] = { { UK_UPDOWN, NULL, ST_KEY_SEL }, { 0, "Enter", "Menu" }, { 0, "Esc", "Back" } };
    ui_footer(keys, 3);
}

// A message in a box over the list. Shown at once (nothing waits for a key to flush it).
static void st_box(const char *msg) {
    const int w = (int)strlen(msg) * 8 + 32;
    const int x0 = (UI_L + UI_R) / 2 - w / 2, y0 = ST_Y0 + 4 * UI_RH;
    lcd_ui_fill(x0 - 2, y0 - 2, x0 + w + 2, y0 + UI_RH + 2, UI_ACCENT, 5);
    const UiRow r = { msg, NULL, UI_VALUE, 0, 0 };
    ui_group(x0, x0 + w, y0, &r, 1, -1, 0);
    lcd_menu_flush();
}

static void st_notice(const char *msg) {
    st_box(msg);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

// A small menu beside the selected row. Returns the row chosen, or -1 on ESC. Rows with `dim` cannot be chosen;
// the cursor stays at `lo` or below.
static int st_pick(int row_y, int lx1, const UiRow *rows, int n, int w, int start, int lo) {
    int y0 = row_y;
    const int bottom = ST_Y0 + st_vis() * UI_RH;
    if (y0 + n * UI_RH > bottom) y0 = bottom - n * UI_RH;
    if (y0 < ST_Y0) y0 = ST_Y0;
    int x1 = lx1 - 8, x0 = x1 - w;
    if (x0 < UI_L) {                     // a wide question on a small panel: over the picture rather than off the edge
        x0 = UI_L;
        x1 = x0 + w;
    }
    int sel = start;
    for (;;) {
        lcd_ui_fill(x0 - 2, y0 - 2, x1 + 2, y0 + n * UI_RH + 2, UI_ACCENT, 5);
        ui_group(x0, x1, y0, rows, n, sel, 0);
        const uint8_t nk = wait_key();
        if (nk == NK_ESC) return -1;
        if (key_is_up(nk) && sel > lo) sel--;
        if (key_is_down(nk) && sel < n - 1) sel++;
        if (nk == NK_RET && !rows[sel].dim) return sel;
    }
}

static bool st_confirm(int row_y, int lx1, const char *question, const char *yes) {
    const UiRow rows[3] = {
        { question, NULL, UI_VALUE, 0, 1 }, { yes, NULL, UI_VALUE, 0, 0 }, { "Cancel", NULL, UI_VALUE, 0, 0 },
    };
    return st_pick(row_y, lx1, rows, 3, (int)strlen(question) * 8 + 24, 2, 1) == 1;
}

// In pieces smaller than the stream buffer: newlib hands a larger write straight to the card from where it lies,
// and from PSRAM the S3's SPI card fails it (EIO); the TAB5 takes it, but a sector at a time.
static bool st_put(FILE *f, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    while (n) {
        const size_t k = n < 4096 ? n : 4096;
        if (fwrite(b, 1, k, f) != k) return false;
        b += k;
        n -= k;
    }
    return true;
}
static int st_wr(void *ctx, const void *p, size_t n) { return st_put((FILE *)ctx, p, n) ? 0 : -1; }
static bool st_get(FILE *f, void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    while (n) {
        const size_t k = n < 4096 ? n : 4096;
        if (fread(b, 1, k, f) != k) return false;
        b += k;
        n -= k;
    }
    return true;
}
static int st_rd(void *ctx, void *p, size_t n) { return st_get((FILE *)ctx, p, n) ? 0 : -1; }

// Written to a temporary name and renamed over the slot, so a card that fills up or is pulled leaves the old slot
// as it was and no half file behind.
static bool st_save(int slot) {
    mkdir("/sd/PC98", 0777);
    mkdir(ST_DIR, 0777);
    char path[64], tmp[64];
    st_path(path, sizeof(path), slot, false);
    st_path(tmp, sizeof(tmp), slot, true);

    StHdr h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "98TS", 4);
    h.ver = ST_VER;
    snprintf(h.fw_ver, sizeof(h.fw_ver), "%s", esp_app_get_description()->version);
    memcpy(h.elf_sha, esp_app_get_description()->app_elf_sha256, 32);
    const time_t now = time(NULL);
    h.time = st_clock_ok((int64_t)now) ? (int64_t)now : 0;
    for (int i = 0; i < ST_SLOTS; i++) {
        if (s_st[i].used && !s_st[i].bad && s_st[i].seq > h.seq) h.seq = s_st[i].seq;
    }
    h.seq++;
    const char *img = g_settings.fdd_image[0][0] > ' ' ? (const char *)g_settings.fdd_image[0]
                                                        : (const char *)g_settings.hdd_image[0];
    const char *base = strrchr(img, '/');
    snprintf(h.name, sizeof(h.name), "%s", base ? base + 1 : img);

    // The screen as it was when the menu opened (the menu draws into its own canvas), at 1/4.
    int fw = 0, fh = 0, stride = 0;
    const uint16_t *fb = (const uint16_t *)lcd_last_frame(&fw, &fh, &stride);
    if (s_th_buf && fb && fw / 4 == ST_THUMB_W && fh / 4 == ST_THUMB_H) {
        st_reduce(fb, stride, ST_THUMB_W, ST_THUMB_H, 4, th_new());
        h.thumb_w = ST_THUMB_W;
        h.thumb_h = ST_THUMB_H;
    }

    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    char *iobuf = (char *)heap_caps_aligned_alloc(64, ST_IOBUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (iobuf) setvbuf(f, iobuf, _IOFBF, ST_IOBUF);
    bool ok = fwrite(&h, 1, sizeof(h), f) == sizeof(h);
    if (ok && h.thumb_w) ok = st_put(f, th_new(), (size_t)TH_PIX * 2);
    if (ok) {
        const Pc98StateOut out = { st_wr, f };
        ok = pc98_state_save(&out) == 0;
    }
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    heap_caps_free(iobuf);
    if (ok) {
        remove(path);                    // FatFs will not rename over a file
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

// 0 = loaded, 1 = could not be read as a slot, 3 = the loader refused it (it resets the machine). A slot from another
// build is tried too once the user has agreed; the loader decides whether it can read it.
static int st_load(int slot) {
    char path[64];
    st_path(path, sizeof(path), slot, false);
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    StHdr h;
    if (fread(&h, 1, sizeof(h), f) != sizeof(h) || memcmp(h.magic, "98TS", 4) != 0 || h.ver != ST_VER) {
        fclose(f);
        return 1;
    }
    if (h.thumb_w && fseek(f, (long)h.thumb_w * h.thumb_h * 2, SEEK_CUR) != 0) {
        fclose(f);
        return 1;
    }
    char *iobuf = (char *)heap_caps_aligned_alloc(64, ST_IOBUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (iobuf) setvbuf(f, iobuf, _IOFBF, ST_IOBUF);
    const Pc98StateIn in = { st_rd, f };
    const int rc = pc98_state_load(&in);
    fclose(f);
    heap_caps_free(iobuf);
    if (rc != 0) return 3;
    // The menu's own record of what is mounted follows the machine, which now has what the slot had in its drives
    // (saved to NVS when the menu closes).
    for (int i = 0; i < 4; i++) {
        pcs_copy(g_settings.fdd_image[i], g_state_media.path[i], sizeof(g_settings.fdd_image[i]));
    }
    pcs_copy(g_settings.hdd_image[0], g_state_media.path[4], sizeof(g_settings.hdd_image[0]));
    pcs_copy(g_settings.hdd_image[1], g_state_media.path[5], sizeof(g_settings.hdd_image[1]));
    pcs_copy(g_settings.cd_image, g_state_media.path[6], sizeof(g_settings.cd_image));
    return 0;
}

// The State Save screen. Returns true when a state was loaded: the menu then closes and the machine runs.
static bool state_menu(void) {
    lcd_menu_clear();
    if (!g_sd_ok) {
        ui_header("State Save");
        ui_section(UI_L, UI_R, ST_SEC_Y, "No SD card.", NULL);
        lcd_menu_flush();
        vTaskDelay(pdMS_TO_TICKS(1200));
        return false;
    }
    st_alloc_bufs();
    for (int i = 0; i < ST_SLOTS; i++) st_scan_one(i);
    bool loaded = false;
    int sel = 0, top = 0, shown = -1;
    for (;;) {
        const int vis = st_vis();
        if (sel < top) top = sel;
        if (sel >= top + vis) top = sel - vis + 1;
        if (shown != sel) {
            st_load_thumb(sel, st_div());
            shown = sel;
        }
        st_draw(sel, top);
        const uint8_t nk = wait_key();
        if (nk == NK_ESC) break;
        if (key_is_up(nk))   sel = (sel > 0) ? sel - 1 : ST_SLOTS - 1;
        if (key_is_down(nk)) sel = (sel + 1 < ST_SLOTS) ? sel + 1 : 0;
        if (nk != NK_RET) continue;

        const StSlot *s = &s_st[sel];
        const int lx1 = UI_R - ST_THUMB_W / st_div() - 16;
        const int ry = ST_Y0 + (sel - top) * UI_RH;
        char q[48];
        const UiRow acts[4] = {
            { "Save", NULL, UI_VALUE, 0, 0 }, { "Load", NULL, UI_VALUE, 0, (uint8_t)!s->used },
            { "Delete", NULL, UI_VALUE, 0, (uint8_t)!s->used }, { "Cancel", NULL, UI_VALUE, 0, 0 },
        };
        const int act = st_pick(ry, lx1, acts, 4, 112, 0, 0);
        if (act == 0) {
            snprintf(q, sizeof(q), "Overwrite slot %d?", sel + 1);
            if (s->used && !st_confirm(ry, lx1, q, "Overwrite")) continue;
            st_draw(sel, top);
            st_box("Saving...");
            const bool ok = st_save(sel);
            st_scan_one(sel);
            shown = -1;
            st_draw(sel, top);
            st_notice(ok ? "Saved." : "Save failed.");
        } else if (act == 1) {
            if (s->bad) {
                st_notice("Not a state file.");
            } else if (s->other && (snprintf(q, sizeof(q), "Other firmware v%.10s. Load?", s->fw_ver),
                                    !st_confirm(ry, lx1, q, "Load"))) {
                continue;
            } else {
                st_draw(sel, top);
                st_box("Loading...");
                st_free_bufs();          // the loader may want the memory
                const int rc = st_load(sel);
                if (rc == 0) {
                    loaded = true;
                    break;
                }
                st_alloc_bufs();
                shown = -1;
                st_draw(sel, top);
                st_notice(rc == 3 ? (s->other ? "This firmware cannot read it." : "Load failed. Machine reset.") : "Not a state file.");
            }
        } else if (act == 2) {
            snprintf(q, sizeof(q), "Delete slot %d?", sel + 1);
            if (!st_confirm(ry, lx1, q, "Delete")) continue;
            char path[64];
            st_path(path, sizeof(path), sel, false);
            remove(path);
            st_scan_one(sel);
            shown = -1;
        }
    }
    st_free_bufs();
    return loaded;
}

extern volatile int g_menu_active, g_menu_req;
extern "C" void menu_disk_run(void) {
    int sel = 0;
    // Drop what the game left in the key queue and any arrow still "held" from an earlier session, so only
    // keys pressed from now on drive the menu.
    { uint8_t k, d; while (usb_kbd_pop(&k, &d)) {} }
    s_held_nk = 0xff;
    g_menu_active = 1;
    lcd_menu_set_bg(UI_PAGE);
    lcd_menu_clear();
    draw_drives(sel);
    for (;;) {
        uint8_t nk, dn;
        if (!menu_key_pop(&nk, &dn)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (!dn) continue;
        if (nk == NK_ESC) break;
        if (key_is_up(nk))   sel = (sel > 0) ? sel - 1 : 21;
        if (key_is_down(nk)) sel = (sel < 21) ? sel + 1 : 0;
        // Left/right adjust the rows that hold a value rather than doing
        // something; everywhere else they are ignored.
        {
            const int dir = key_is_right(nk) ? 1 : (key_is_left(nk) ? -1 : 0);
            if (dir) {
                if (sel == 11) {
                    g_backlight_pct = level_step(g_backlight_pct, dir, false);
                    tab5_backlight_set(g_backlight_pct);
                } else if (sel == 12) {
                    audio_set_volume(level_step(audio_get_volume(), dir, false, 0));
                } else if (sel == 13) {
                    line200_apply(wrap_step(g_line200, dir, 3));
                    line200_refresh();
                } else if (sel == 15) {
                    lcd_lamps_enable(dir > 0);
                } else if (sel == 16) {
                    lcd_set_osk(osk_step(dir));
                    lcd_menu_clear();
                } else if (sel == 14) {
                    audio_set_fx(sound_step(audio_get_fx(),dir,true));
                } else if (sel == 5) {
                    const UINT mult = clock_step(g_settings.clock_mul, dir, false);
                    g_settings.clock_mul = mult;       // display follows immediately
                    g_speed_req = (int)mult;      // emulator loop applies it
                } else if (sel == 6) {
                    g_settings.base_clock = dir > 0 ? CLK_BASE5 : CLK_BASE8;
                } else if (sel == 7) {
                    g_cpu_mode = cpu_step(g_cpu_mode, dir, true);
                } else if (sel == 10) {
                    touch_mouse_set_speed(wrap_step(touch_mouse_speed(), dir, 3));
                }
            }
        }
        if (nk == NK_RET) {
            if (sel == 4) {                     // create blank disk image
                newdisk_flow();
                lcd_menu_clear();   // the submenu owned the screen
            } else if (sel == 5) {              // CPU clock: next, wrapping
                const UINT mult = clock_step(g_settings.clock_mul, 1, true);
                g_settings.clock_mul = mult;         // display follows immediately
                g_speed_req = (int)mult;        // emulator loop applies it
            } else if (sel == 6) {              // oscillator family: applies on reset
                g_settings.base_clock = g_settings.base_clock == CLK_BASE8 ? CLK_BASE5 : CLK_BASE8;
            } else if (sel == 7) {              // CPU: next, wrapping
                g_cpu_mode = cpu_step(g_cpu_mode, 1, true);
            } else if (sel == 8) {              // BIOS ROM: pick a file from the SD
                browse_rom(0);
                lcd_menu_clear();   // the submenu owned the screen
            } else if (sel == 9) {              // FONT ROM: pick a file from the SD
                browse_rom(1);
                lcd_menu_clear();   // the submenu owned the screen
            } else if (sel == 10) {             // Touch mouse speed: low, medium, high
                touch_mouse_set_speed(wrap_step(touch_mouse_speed(), 1, 3));
            } else if (sel == 11) {              // Backlight: next, wrapping
                g_backlight_pct = level_step(g_backlight_pct, 1, true);
                tab5_backlight_set(g_backlight_pct);
            } else if (sel == 12) {              // Volume: next, wrapping
                audio_set_volume(level_step(audio_get_volume(), 1, true, 0));
            } else if (sel == 13) {              // 200-line graphics: next, wrapping
                line200_apply((g_line200 + 1) % 3);
                line200_refresh();
            } else if (sel == 14) {              // Sound: next, wrapping
                audio_set_fx(sound_step(audio_get_fx(),1,true));
            } else if (sel == 15) {              // Drive lamps: on/off
                lcd_lamps_enable(!lcd_lamps_enabled());
            } else if (sel == 16) {              // Screen keyboard: off, landscape, landscape-game, portrait
                lcd_set_osk(osk_step(1));
                lcd_menu_clear();
            } else if (sel == 17) {              // Screenshot
                if (!screenshot_save(s_last_shot, sizeof(s_last_shot))) {
                    snprintf(s_last_shot, sizeof(s_last_shot), "FAILED");
                }
            } else if (sel == 18) {              // State Save: the slot screen; a load closes the menu
                if (state_menu()) break;
                lcd_menu_clear();   // the submenu owned the screen
            } else if (sel == 19 || sel == 20) {   // USB Mode: arm the flag, reboot
                const int m = (sel == 19) ? USB_MODE_SD : USB_MODE_IMAGE;
                if (m == USB_MODE_IMAGE && choose_image_drive() < 0) {
                    lcd_menu_clear();   // back to the menu
                    draw_drives(sel);
                    continue;
                }
                save_settings();
                usb_msc_request(m);
                lcd_menu_clear();
                mline(0, (m == USB_MODE_IMAGE) ? MODE_INDENT "Disk Image Reader Mode"
                                                       : MODE_INDENT "SD Card Reader Mode",
                              COL_YELLOW, COL_BLACK);
                mline(2, MODE_INDENT "Restarting...", COL_WHITE, COL_BLACK);
                // Drawing normally reaches the panel from menu_key_pop(), and
                // this is the one screen that never waits for a key - it
                // reboots. Without this the clear and the text stay in cache,
                // only whatever the cache happened to evict gets to PSRAM, and
                // what is left on the panel is half the old menu.
                lcd_menu_flush();
                vTaskDelay(pdMS_TO_TICKS(800));
                // Hand the card back before the reboot. The mode this restarts
                // into initialises the card from scratch, and one left busy by
                // the emulator will not answer - which is what made entering
                // these modes work only some of the time.
                sd_unmount();
                panel_tab5_blank_early();
                esp_restart();
            } else if (sel == 21) {             // RESET: persist the settings, reboot
                save_settings();
                sd_unmount();                   // same reason as USB Mode above
                panel_tab5_blank_early();
                esp_restart();
            } else {
                browse(sel);                    // sel 0/1/2/3 = FDD1/FDD2/HDD/CD-ROM
                lcd_menu_clear();   // the submenu owned the screen
            }
        }
        draw_drives(sel);
    }
    g_menu_active = 0;
    g_menu_req = 0;                         // an F12/Pause pressed while the menu was up must not reopen it
    save_settings();                        // ESC exit: persist multiple + scaler
}

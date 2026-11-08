// Display backend for the M5Stack Tab5: 2-lane MIPI-DSI, 720(H) x 1280(V)
// native portrait, RGB565. The panel controller is ILI9881C on board revision
// 1 and ST7123 on revision 2 — the BSP probes for it, so nothing here depends
// on which one is fitted.
//
// Same extern "C" interface as the ESP32-S3 backends (lcd_rgb.cpp /
// lcd_st7789.cpp) so main.cpp and menu_disk.cpp need no changes. What differs:
//
//   - The panel is driven by esp_lcd's DPI driver, which — like the S3's RGB
//     driver — hands out a frame buffer that hardware scans out continuously.
//     So this is a blit into memory, not a transfer, and the panel keeps
//     showing whatever was left there. That is what makes the per-tile diff
//     below safe.
//   - The panel is portrait and the PC-98 screen is landscape, so the blit
//     rotates 90 degrees: PC-98 x runs along the panel's 1280 axis, PC-98 y
//     along its 720 axis.
//   - Two placements, switchable at runtime (see board_pins.h): 1.8x filling
//     the height, or 1:1 centred.

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "driver/ppa.h"

// The panel itself is brought up in panel_tab5.cpp, which keeps the BSP
// headers to itself — they pull in esp_vfs_fat.h, whose ff.h typedefs TCHAR as
// char while another header of the core once typedef'd it differently, and
// this file needs the core, for the CG ROM.
extern "C" bool panel_tab5_init(void);
extern "C" uint16_t *panel_tab5_framebuffer(void);

extern "C" {
#include <compiler.h>  // the core's types
#include <memmap.h>
#include <video.h>
extern UINT8 mem[];     // the machine's memory (bus.c)
}

#include "board_pins.h"

extern "C" int ets_printf(const char *fmt, ...);

#define PC98_W BRD_PC98_W
#define PC98_H BRD_PC98_H
#define PAN_W  BRD_LCD_H_RES  // 720
#define PAN_H  BRD_LCD_V_RES  // 1280
#define SRC_W  PC98_W

// Which way up the picture sits on the glass, as one switch.
//
// Three things have to agree or the display comes apart: the PPA's rotation of
// the emulated screen, the s_map_* tables the menu text is painted through, and
// the same tables again for turning a finger position back into a PC-98
// coordinate. Flipping both axes of the tables is a 180-degree rotation, and
// swapping the PPA between 90 and 270 is the same 180 degrees, so they are tied
// together here rather than left as three independent constants to get wrong.
//
// 1 puts the picture the right way up for the way the case is marked.
#define ROTATE_180 1

#if ROTATE_180
#define FLIP_V 0
#define FLIP_H 1
#define PPA_ROTATION PPA_SRM_ROTATION_ANGLE_90
#else
#define FLIP_V 1
#define FLIP_H 0
#define PPA_ROTATION PPA_SRM_ROTATION_ANGLE_270
#endif

// 16, not the 32 the RGB fork uses: 1280 divides by 32 but 720 does NOT
// (22.5), so a 32 tile silently covered 704 of the 720 columns. 16 divides
// both exactly (45 x 80 tiles).
//
// The tile is what keeps the strided source reads inside the cache: a whole
// tile is drawn from at most 16 source rows, and those stay resident for the
// tile's 16 output rows. Walking the source the other way misses on every
// pixel — that cost the RGB fork a 7.8x slowdown before it was tiled.
#define TILE 16
#define TILES_X (PAN_W / TILE)
#define TILES_Y (PAN_H / TILE)

static uint16_t *s_fb = nullptr;  // the DPI frame buffer (PSRAM)
static bool s_lamp_force = true;  // the access lamps need painting again

// panel axis -> PC-98 axis, precomputed for the active scale mode.
// -1 means "outside the PC-98 screen", i.e. letterbox.
static int16_t s_map_y[PAN_W];  // panel_x -> PC-98 y
static int16_t s_map_x[PAN_H];  // panel_y -> PC-98 x

static int s_scale_mode = BRD_SCALE_DEFAULT;
static bool s_force_full = true;

// Per-tile hash of what was last written, so an unchanged tile costs no PSRAM
// writes at all. On a static screen that removes almost the whole blit.

// Jobs for the core-0 blit task. Core 1 posts, core 0 consumes; s_pend counts posted and
// unfinished jobs and is changed only with atomics (both cores write it).
typedef struct { void (*fn)(const void *); const void *arg; const uint8_t *src; unsigned seq; } BlitJob;
static QueueHandle_t s_blit_q = nullptr;
static volatile unsigned s_post_seq, s_done_seq;   // composite jobs posted / finished (core 1 / core 0)
static const uint8_t *volatile s_blit_src = nullptr;
static volatile int s_blit_busy = 0;
static volatile int s_pend = 0;   // atomic only   // blit-task jobs posted and not yet finished (the running one counts)

// ---- scale mapping --------------------------------------------------------
static void build_maps(void) {
    for (int i = 0; i < PAN_W; i++) {
        s_map_y[i] = -1;
    }
    for (int i = 0; i < PAN_H; i++) {
        s_map_x[i] = -1;
    }

    if (s_scale_mode == BRD_SCALE_DOT) {
        // 1:1, centred on both axes.
        const int xoff = (PAN_W - PC98_H) / 2;  // (720-400)/2 = 160
        const int yoff = (PAN_H - PC98_W) / 2;  // (1280-640)/2 = 320
        for (int py = 0; py < PC98_H; py++) {
            s_map_y[xoff + py] = (int16_t)(FLIP_V ? (PC98_H - 1 - py) : py);
        }
        for (int px = 0; px < PC98_W; px++) {
            s_map_x[yoff + px] = (int16_t)(FLIP_H ? (PC98_W - 1 - px) : px);
        }
    } else {
        // 1.8x: 400 -> 720 exactly (the whole short axis), 640 -> 1152.
        const int outw = PC98_H * BRD_FIT_NUM / BRD_FIT_DEN;  // 720
        const int outh = PC98_W * BRD_FIT_NUM / BRD_FIT_DEN;  // 1152
        const int xoff = (PAN_W - outw) / 2;                  // 0
        const int yoff = (PAN_H - outh) / 2;                  // 64
        for (int i = 0; i < outw; i++) {
            int py = i * BRD_FIT_DEN / BRD_FIT_NUM;
            if (py >= PC98_H) {
                py = PC98_H - 1;
            }
            s_map_y[xoff + i] = (int16_t)(FLIP_V ? (PC98_H - 1 - py) : py);
        }
        for (int i = 0; i < outh; i++) {
            int px = i * BRD_FIT_DEN / BRD_FIT_NUM;
            if (px >= PC98_W) {
                px = PC98_W - 1;
            }
            s_map_x[yoff + i] = (int16_t)(FLIP_H ? (PC98_W - 1 - px) : px);
        }
    }
    s_force_full = true;
}

// ---- blit -----------------------------------------------------------------
// The panel's DMA reads the frame buffer straight out of PSRAM while the CPU
// writes to it through the L2 cache, so anything written has to be flushed
// before the scanout can see it. Without this the panel is lit, the driver is
// fetching, the emulator is running — and the screen stays black, because the
// DMA keeps reading the zeroed memory the cache is hiding. IDF's own DPI
// driver does exactly this in esp_lcd_panel_dpi.c whenever it touches a frame
// buffer. Synced a tile row-band at a time (32 panel rows, contiguous) rather
// than the whole 1.8MB, so an unchanged screen costs nothing.
static inline void fb_sync_all(void) {
    esp_cache_msync(s_fb, (size_t)PAN_W * PAN_H * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static inline void fb_sync_band(int y0) {
    void *p = s_fb + (size_t)y0 * PAN_W;
    const size_t n = (size_t)TILE * PAN_W * sizeof(uint16_t);
    esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// The rotate-and-scale is done by the PPA, the ESP32-P4's 2D pixel engine.
//
// The CPU version that used to live here cost 62ms for a full frame - more than
// the emulated CPU and the screen composite put together, and enough that the
// emulator throttled its own frame rate waiting for it (main.cpp drops to
// rendering every 4th frame when it falls behind). It is 921,600 output pixels
// gathered through a rotation, so there was not much left to win by tuning it.
//
// The PPA does exactly this operation in hardware: 90 degrees counter-clockwise
// puts PC-98 x along the panel's long axis and mirrors y, which is precisely the
// mapping the tables above describe, and it does it as a DMA pass rather than a
// million strided loads.
//
// What it costs is the scale factor. The PPA expresses it as an integer plus a
// 1/16 fraction, so the 1.8x that filled the panel's short axis exactly is not
// available; 1.75 is the closest below it and leaves a 10-pixel border on that
// axis. board_pins.h carries the same ratio so the menu text and the touch
// mapping stay in step.
static ppa_client_handle_t s_ppa = nullptr;

static void do_blit(const uint8_t *fb) {
    if (!s_ppa) {
        return;
    }
    const int num = (s_scale_mode == BRD_SCALE_DOT) ? 1 : BRD_FIT_NUM;
    const int den = (s_scale_mode == BRD_SCALE_DOT) ? 1 : BRD_FIT_DEN;
    const int outw = PC98_H * num / den;   // along the panel's short axis
    const int outh = PC98_W * num / den;   // along the panel's long axis

    ppa_srm_oper_config_t op = {};
    op.in.buffer = (void *)fb;
    op.in.pic_w = SRC_W;
    op.in.pic_h = 480;                     // the surface the renderer draws into
    op.in.block_w = PC98_W;
    op.in.block_h = PC98_H;
    op.in.block_offset_x = 0;
    op.in.block_offset_y = 0;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    op.out.buffer = s_fb;
    op.out.buffer_size = (size_t)PAN_W * PAN_H * sizeof(uint16_t);
    op.out.pic_w = PAN_W;
    op.out.pic_h = PAN_H;
    op.out.block_offset_x = (PAN_W - outw) / 2;
    op.out.block_offset_y = (PAN_H - outh) / 2;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    op.rotation_angle = PPA_ROTATION;
    op.scale_x = (float)num / (float)den;
    op.scale_y = (float)num / (float)den;
    op.mode = PPA_TRANS_MODE_BLOCKING;

    ppa_do_scale_rotate_mirror(s_ppa, &op);
    s_force_full = false;
}

// The PPA only writes the block; the letterbox around it keeps whatever was
// there. Painted black once, when the geometry changes.
static void clear_borders(void) {
    if (s_fb) {
        memset(s_fb, 0, (size_t)PAN_W * PAN_H * sizeof(uint16_t));
        fb_sync_all();
        s_lamp_force = true;
    }
}

static volatile uint8_t s_job_posted;
static void blit_task(void *arg) {
    (void)arg;
    for (;;) {
        BlitJob j;
        xQueueReceive(s_blit_q, &j, portMAX_DELAY);
        if (j.fn) { j.fn(j.arg); s_done_seq = j.seq; }
        if (j.src) {
            do_blit(j.src);
        }
        s_blit_busy = (__atomic_sub_fetch(&s_pend, 1, __ATOMIC_SEQ_CST) > 0);
    }
}

// Bring-up diagnostic: the panel showed noise at power-on (so the frame buffer
// is on screen) and then black (so our clear and the cache sync both work),
// which leaves only "is anything being blitted, and is it non-black?".
// Set to 0 once the picture is up.
#define LCD_BLIT_TRACE 0

extern "C" void lcd_blit(const uint8_t *fb) {
#if LCD_BLIT_TRACE
    static int calls = 0, dropped = 0;
    if (s_fb && fb && s_blit_busy) {
        dropped++;
    }
    if (s_fb && fb && !s_blit_busy && (calls++ % 60) == 0) {
        // Is the emulator's own framebuffer actually non-black?
        const uint16_t *p = (const uint16_t *)fb;
        int nz = 0;
        for (int y = 0; y < PC98_H && nz < 4; y += 8) {
            for (int x = 0; x < PC98_W; x += 8) {
                if (p[(size_t)y * SRC_W + x]) {
                    nz++;
                    break;
                }
            }
        }
        // The boot screen is "text VRAM rendered with the CGROM", so an all
        // black frame buffer is either an empty text VRAM (the BIOS wrote
        // nothing) or an empty font (FONT.ROM never loaded). Report both.
        int tvram = 0;
        for (int i = 0; i < 80 * 25 * 2; i += 2) {
            const uint8_t ch = mem[0xA0000 + i];
            if (ch > 0x20 && ch < 0x7f) {
                tvram++;
            }
        }
        int glyph = 0;
        for (int i = 0; i < 16; i++) {
            if (cgrom[0x80000 + 'A' * 16 + i]) {
                glyph++;
            }
        }
        // And does anything actually reach the panel's own buffer?
        int fbnz = 0;
        for (int i = 0; i < PAN_W * PAN_H; i += 977) {
            if (s_fb[i]) {
                fbnz++;
            }
        }
        ets_printf("lcd_blit: n=%d src_nb=%d tvram=%d fontA=%d fb_nb=%d\n", calls, nz, tvram, glyph, fbnz);
    }
#endif
    if (!s_fb || !fb || s_blit_busy) {
        return;  // still drawing the previous frame; drop this one
    }
    s_blit_src = fb;
    s_blit_busy = 1;
    __atomic_add_fetch(&s_pend, 1, __ATOMIC_SEQ_CST);
    BlitJob j = { nullptr, nullptr, fb, 0 };
    if (xQueueSend(s_blit_q, &j, 0) != pdTRUE) {
        s_blit_busy = (__atomic_sub_fetch(&s_pend, 1, __ATOMIC_SEQ_CST) > 0);
    }
}

extern "C" bool lcd_blit_busy(void) {
    // s_pend (atomic) is the truth: s_blit_busy is a plain store that core 1 (post) and core 0 (finish) can write
    // out of order, leaving 0 while a posted job is still waiting - the menu would then draw and the job's
    // blit would land on top of it.
    return __atomic_load_n(&s_pend, __ATOMIC_SEQ_CST) != 0 || s_blit_busy != 0;
}

// The frame the emulator handed over last, for screenshot.cpp. While the menu
// is up this is still the screen that was showing when it was opened: the menu
// draws into the panel buffer, not into this one, and the machine is paused.
// The geometry returned is the PC-98 screen's own, before any scaling.
extern "C" const void *lcd_last_frame(int *w, int *h, int *stride) {
    if (w) {
        *w = PC98_W;
    }
    if (h) {
        *h = PC98_H;
    }
    if (stride) {
        *stride = SRC_W;   // the renderer draws into a 640x480 surface
    }
    return (const void *)s_blit_src;
}

// ---- scale mode API -------------------------------------------------------
extern "C" int lcd_get_scale_mode(void) {
    return s_scale_mode;
}

extern "C" int lcd_scale_mode_count(void) {
    return BRD_SCALE_COUNT;
}

extern "C" const char *lcd_scale_mode_name(int m) {
    return (m == BRD_SCALE_DOT) ? "1:1 dot" : "1.8x fit";
}

extern "C" void lcd_set_scale_mode(int m) {
    if (m < 0 || m >= BRD_SCALE_COUNT || m == s_scale_mode) {
        return;
    }
    s_scale_mode = m;
    build_maps();
    if (s_fb) {
        memset(s_fb, 0, (size_t)PAN_W * PAN_H * sizeof(uint16_t));
        fb_sync_all();
    }
}

// ---- init -----------------------------------------------------------------
extern "C" bool lcd_init(void) {
    if (!panel_tab5_init()) {
        return false;
    }
    // The DPI driver owns the frame buffer and scans it out continuously, so
    // write into it directly rather than pushing bitmaps.
    s_fb = panel_tab5_framebuffer();
    if (!s_fb) {
        return false;
    }
    memset(s_fb, 0, (size_t)PAN_W * PAN_H * sizeof(uint16_t));
    fb_sync_all();

    s_blit_q = xQueueCreate(2, sizeof(BlitJob));
    if (!s_blit_q) {
        ets_printf("lcd: blit semaphore failed\n");
        return false;
    }

    build_maps();
    {
        ppa_client_config_t pc = {};
        pc.oper_type = PPA_OPERATION_SRM;
        pc.max_pending_trans_num = 1;
        if (ppa_register_client(&pc, &s_ppa) != ESP_OK) {
            ets_printf("lcd: ppa_register_client failed\n");
            return false;
        }
    }
    clear_borders();
    xTaskCreatePinnedToCore(blit_task, "lcdblit", 4096, nullptr, 4, nullptr, 0);

    // Backlight only — NOT bsp_display_brightness_init(). bsp_display_new()
    // has already run it (along with bsp_feature_enable(BSP_FEATURE_LCD)), and
    // configuring the same LEDC channel twice fails with
    //     ledc: GPIO 22 is not usable, maybe conflict with others
    // leaving the channel dead and the panel dark — the emulator runs, the
    // chime plays, and the screen shows nothing.
    // The panel model is not fixed: the BSP probes the board revision and picks
    // ILI9881C or ST7123 accordingly (it logs which). Only the geometry is ours
    // to care about, so do not name a controller here.
    ets_printf("lcd: MIPI-DSI %dx%d ready (PC-98 %dx%d, %s)\n", PAN_W, PAN_H, PC98_W, PC98_H, (s_scale_mode == BRD_SCALE_DOT) ? "1:1" : "1.75x fit");
    return true;
}

// ---- access lamps -----------------------------------------------------------
// FDD1, FDD2, CD-ROM and HDD: small rectangles in the margin under the
// picture, red for the floppies, yellow for the CD-ROM and green for the
// hard disk. The core reports each access through host_fdd_access /
// host_cd_access / host_hdd_access (host.h); a lamp stays lit for a
// moment after the last one, so a single sector read is still seen to flash.
#define LAMP_HOLD_US 80000
#define LAMPS 4
static volatile int64_t s_lamp_at[LAMPS];   // FDD1, FDD2, CD-ROM, HDD
static int  s_lamp_drawn = -1;          // lamps last painted, -1 = nothing there

extern "C" void lamp_fdd(REG8 drv) {
    if (drv < 2) {
        s_lamp_at[drv] = esp_timer_get_time();
    }
}

extern "C" void lamp_cd(void) {
    s_lamp_at[2] = esp_timer_get_time();
}

extern "C" void lamp_hdd(REG8 drv) {
    (void)drv;
    s_lamp_at[3] = esp_timer_get_time();
}

// A rectangle in screen coordinates as the picture is seen - x 0..1279 left to
// right, y 0..719 top to bottom - through the same turn the blit applies.
static void lamp_rect(int lx0, int ly0, int w, int h, uint16_t c) {
    for (int lx = lx0; lx < lx0 + w; lx++) {
        const int panel_y = FLIP_H ? (PAN_H - 1 - lx) : lx;
        uint16_t *row = s_fb + (size_t)panel_y * PAN_W;
        for (int ly = ly0; ly < ly0 + h; ly++) {
            row[FLIP_V ? (PAN_W - 1 - ly) : ly] = c;
        }
    }
}

#define LAMP_W   40
#define LAMP_H   6
#define LAMP_GAP 8

static bool s_lamps_on = true;          // the menu's switch

extern "C" void lcd_lamps_enable(bool on) {
    s_lamps_on = on;
    s_lamp_force = true;
}

extern "C" bool lcd_lamps_enabled(void) {
    return s_lamps_on;
}

// Once a frame, from the emulator loop: repaint the lamps that changed. They
// sit in a row under the picture, FDD1 FDD2 CD-ROM HDD from the left, the row's right
// end under the picture's right edge - wherever the scale mode puts it.
extern "C" void lcd_lamps_tick(void) {
    if (!s_fb) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    int on = 0;
    for (int i = 0; i < LAMPS; i++) {
        if (s_lamp_at[i] && now - s_lamp_at[i] < LAMP_HOLD_US) {
            on |= 1 << i;
        }
    }
    if (!s_lamps_on) {
        on = -2;                        // drawn as nothing at all
    }
    if (on == s_lamp_drawn && !s_lamp_force) {
        return;
    }
    const int num = (s_scale_mode == BRD_SCALE_DOT) ? 1 : BRD_FIT_NUM;
    const int den = (s_scale_mode == BRD_SCALE_DOT) ? 1 : BRD_FIT_DEN;
    const int pic_w = PC98_W * num / den;               // along the screen's width
    const int pic_h = PC98_H * num / den;
    const int right = (PAN_H + pic_w) / 2;              // just past the picture
    const int below = (PAN_W + pic_h) / 2;
    const int lx0 = right - (LAMPS * LAMP_W + (LAMPS - 1) * LAMP_GAP);
    int ly0 = below + 2;
    if (ly0 + LAMP_H > PAN_W) {
        ly0 = PAN_W - LAMP_H;
    }
    static const uint16_t lit[LAMPS] = {0xF800, 0xF800, 0xFFE0, 0x07E0};   // red, red, yellow, green
    static const uint16_t dark[LAMPS] = {0x3000, 0x3000, 0x3180, 0x0180};
    for (int i = 0; i < LAMPS; i++) {
        const uint16_t c = (on < 0) ? 0x0000 : (on & (1 << i)) ? lit[i] : dark[i];
        lamp_rect(lx0 + i * (LAMP_W + LAMP_GAP), ly0, LAMP_W, LAMP_H, c);
    }
    // The lamps' panel rows, written back for the scanout.
    const int span = LAMPS * LAMP_W + (LAMPS - 1) * LAMP_GAP;
    const int y0 = FLIP_H ? (PAN_H - lx0 - span) : lx0;
    esp_cache_msync(s_fb + (size_t)y0 * PAN_W, (size_t)span * PAN_W * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    s_lamp_drawn = on;
    s_lamp_force = false;
}

// ---- disk-menu text UI ----------------------------------------------------
// Glyphs come from the CGROM the emulator loaded from FONT.ROM (8x16 ANK cells
// at 0x80000) - no second font.
//
// The menu is laid out in PC-98 coordinates (80 columns of 8 pixels, 16-pixel
// rows) and mapped onto the panel through the same rotation and scale as the
// emulator image, so it lands on the picture rather than somewhere else on the
// glass — and the 1.8x mode duplicates its pixels exactly the way the blit
// does, with no second scaling path to get subtly wrong.
#define FONT_ANK16 0x80000
#define MENU_CH_W 8
#define MENU_CH_H 16
#define MENU_COLS (PC98_W / MENU_CH_W)  // 80
#define MENU_ROWS (PC98_H / MENU_CH_H)  // 25

// The menu is drawn into a 640x400 canvas in PC-98 coordinates and copied onto the panel by lcd_menu_flush()
// through the same rotation and scale tables as the emulator image, so boxes, rules and glyphs can sit on any
// pixel, and a whole screen reaches the glass in one pass with one cache writeback.
static uint16_t *s_ui = nullptr;     // PC98_W x PC98_H, RGB565
static bool s_menu_dirty = false;
static uint16_t s_menu_bg = 0;       // the page colour lcd_menu_clear() paints

static uint16_t *ui_canvas(void) {
    if (!s_ui) s_ui = (uint16_t *)heap_caps_malloc((size_t)PC98_W * PC98_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    return s_ui;
}

extern "C" void lcd_menu_set_bg(uint16_t c) { s_menu_bg = c; }

// Fill [x0,x1) x [y0,y1) on the canvas; r > 0 rounds the four corners.
extern "C" void lcd_ui_fill(int x0, int y0, int x1, int y1, uint16_t c, int r) {
    uint16_t *u = ui_canvas();
    if (!u) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PC98_W) x1 = PC98_W;
    if (y1 > PC98_H) y1 = PC98_H;
    for (int y = y0; y < y1; y++) {
        const int dy = (y - y0 < y1 - 1 - y) ? y - y0 : y1 - 1 - y;
        for (int x = x0; x < x1; x++) {
            if (r) {
                const int dx = (x - x0 < x1 - 1 - x) ? x - x0 : x1 - 1 - x;
                if (dx < r && dy < r && (r - dx - 1) * (r - dx - 1) + (r - dy - 1) * (r - dy - 1) > r * r) continue;
            }
            u[y * PC98_W + x] = c;
        }
    }
    s_menu_dirty = true;
}

static inline void ui_px(int x, int y, uint16_t c) {
    if (s_ui && (unsigned)x < PC98_W && (unsigned)y < PC98_H) s_ui[y * PC98_W + x] = c;
}

// The glyphs of a string: an 8x16 ANK cell per byte, or a 16x16 kanji as two cells for a Shift-JIS pair (the SD
// card's names come through FatFs as CP932), from the CGROM the emulator loaded: kanji left half at
// (JIS 2nd byte << 12) + ((JIS 1st byte - 0x20) << 4), right half 0x800 above it. A lead byte with no trail is a
// blank. Returns the number of cells.
static int ui_cells(const char *s, const uint8_t **cell, int max) {
    const uint8_t *blank = cgrom + FONT_ANK16 + (unsigned)' ' * MENU_CH_H;
    int col = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p && col < max; ) {
        const unsigned c = *p;
        if ((c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc)) {
            const unsigned t = p[1];
            if (t < 0x40 || t == 0x7f || t > 0xfc || col + 1 >= max) {
                cell[col++] = blank;
                p++;
                continue;
            }
            unsigned j1 = ((c >= 0xe0 ? c - 0x40 : c) - 0x81) * 2 + 0x21;
            unsigned j2;
            if (t >= 0x9f) {
                j1++;
                j2 = t - 0x7e;
            } else {
                j2 = t - (t >= 0x80 ? 0x20 : 0x1f);
            }
            if (j1 > 0x7e) {
                cell[col++] = blank;
                cell[col++] = blank;
            } else {
                const uint8_t *k = cgrom + (j2 << 12) + ((j1 - 0x20) << 4);
                cell[col++] = k;
                cell[col++] = k + 0x800;
            }
            p += 2;
            continue;
        }
        cell[col++] = cgrom + FONT_ANK16 + c * MENU_CH_H;
        p++;
    }
    return col;
}

// Text at a pixel position (transparent background). Returns the x after the last cell.
extern "C" int lcd_ui_text(int x, int y, const char *s, uint16_t c) {
    const uint8_t *cell[MENU_COLS];
    const int n = ui_cells(s ? s : "", cell, MENU_COLS);
    if (!ui_canvas()) return x;
    for (int i = 0; i < n; i++) {
        for (int r = 0; r < MENU_CH_H; r++) {
            const uint8_t b = cell[i][r];
            if (!b) continue;
            for (int k = 0; k < 8; k++)
                if (b & (0x80 >> k)) ui_px(x + i * 8 + k, y + r, c);
        }
    }
    s_menu_dirty = true;
    return x + n * 8;
}

// Small marks: a chevron 4 wide and 7 tall (dir 1 points right, -1 left), a triangle 7 wide and 4 tall
// (dir -1 points up, 1 down), a dot of radius 2 centred on (x, y).
extern "C" void lcd_ui_chev(int x, int y, uint16_t c, int dir) {
    if (!ui_canvas()) return;
    for (int i = 0; i < 4; i++) {
        const int xx = dir > 0 ? x + i : x + 3 - i;
        ui_px(xx, y + i, c);
        ui_px(xx, y + 6 - i, c);
    }
    s_menu_dirty = true;
}

extern "C" void lcd_ui_tri(int x, int y, uint16_t c, int dir) {
    if (!ui_canvas()) return;
    for (int i = 0; i < 4; i++)
        for (int k = -i; k <= i; k++) ui_px(x + 3 + k, dir < 0 ? y + i : y + 3 - i, c);
    s_menu_dirty = true;
}

extern "C" void lcd_ui_dot(int x, int y, uint16_t c) {
    if (!ui_canvas()) return;
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            if (dx * dx + dy * dy <= 5) ui_px(x + dx, y + dy, c);
    s_menu_dirty = true;
}

extern "C" void lcd_menu_clear(void) {
    lcd_ui_fill(0, 0, PC98_W, PC98_H, s_menu_bg, 0);
    s_lamp_force = true;             // the panel is painted over: put the lamps back after
    s_force_full = true;             // the panel no longer matches the per-tile hashes
}

// Panel pixel -> PC-98 pixel, through whichever scale mode is active. The
// touch panel needs exactly the transform the blit uses and nothing else, so it
// reads the same two tables rather than reconstructing the rotation.
extern "C" bool lcd_panel_to_pc98(int panel_x, int panel_y, int *out_x, int *out_y) {
    if (panel_x < 0 || panel_x >= PAN_W || panel_y < 0 || panel_y >= PAN_H) {
        return false;
    }
    const int px = s_map_x[panel_y];
    const int py = s_map_y[panel_x];
    if (px < 0 || py < 0) {
        return false;  // letterbox
    }
    if (out_x) {
        *out_x = px;
    }
    if (out_y) {
        *out_y = py;
    }
    return true;
}

// Copy the canvas onto the panel (the letterbox in the page colour) and write it back for the scanout.
extern "C" void lcd_menu_flush(void) {
    if (!s_menu_dirty || !s_fb || !s_ui) return;
    const int64_t t0 = esp_timer_get_time();
    for (int panel_y = 0; panel_y < PAN_H; panel_y++) {
        const int px = s_map_x[panel_y];
        uint16_t *drow = s_fb + (size_t)panel_y * PAN_W;
        if (px < 0) {
            for (int panel_x = 0; panel_x < PAN_W; panel_x++) drow[panel_x] = s_menu_bg;
            continue;
        }
        const uint16_t *col = s_ui + px;
        for (int panel_x = 0; panel_x < PAN_W; panel_x++) {
            const int py = s_map_y[panel_x];
            drow[panel_x] = py < 0 ? s_menu_bg : col[py * PC98_W];
        }
    }
    fb_sync_all();
    s_menu_dirty = false;
    s_force_full = true;
}

// One text row of the 80 x 25 grid. Columns from `split` on are drawn in fg2 (a label and its value in two tones);
// columns c0..c1-1 sit on `bar` instead of bg (the selected row), and a nonzero `stripe` marks the bar's left edge.
extern "C" void lcd_menu_line_ex(int row, const char *s, uint16_t fg, uint16_t fg2, int split,
                                 uint16_t bg, uint16_t bar, int c0, int c1, uint16_t stripe) {
    if (!s || row < 0 || row >= MENU_ROWS || !ui_canvas()) return;
    const int y = row * MENU_CH_H;
    lcd_ui_fill(0, y, PC98_W, y + MENU_CH_H, bg, 0);
    if (c1 > c0) lcd_ui_fill(c0 * MENU_CH_W, y, c1 * MENU_CH_W, y + MENU_CH_H, bar, 0);
    if (stripe) lcd_ui_fill(c0 * MENU_CH_W, y, c0 * MENU_CH_W + 3, y + MENU_CH_H, stripe, 0);
    const uint8_t *cell[MENU_COLS];
    const int n = ui_cells(s, cell, MENU_COLS);
    for (int i = 0; i < n; i++) {
        const uint16_t f = i >= split ? fg2 : fg;
        for (int r = 0; r < MENU_CH_H; r++) {
            const uint8_t b = cell[i][r];
            for (int k = 0; k < 8; k++)
                if (b & (0x80 >> k)) ui_px(i * 8 + k, y + r, f);
        }
    }
    s_menu_dirty = true;
}

extern "C" void lcd_menu_line(int row, const char *s, uint16_t fg, uint16_t bg) {
    lcd_menu_line_ex(row, s, fg, fg, MENU_COLS, bg, bg, 0, 0, 0);
}

// A horizontal rule one PC-98 pixel high at line y, from x0 to x1-1.
extern "C" void lcd_menu_rule(int y, int x0, int x1, uint16_t color) {
    lcd_ui_fill(x0, y, x1, y + 1, color, 0);
}

// Blank a range of text rows.
extern "C" void lcd_menu_blank_rows(int first, int last) {
    for (int row = first; row <= last; row++) {
        lcd_menu_line(row, "", 0, s_menu_bg);
    }
}

// Stage 11: the screen composite (RGB565 from the index planes, pc98core/video/render.c) runs on this core right before the blit,
// so core 1 only hands over the job and goes on emulating. Returns nonzero when the job was taken.
extern "C" uint8_t *pc98_framebuffer(void);
extern "C" int lcd_post_composite(void (*fn)(const void *), const void *arg) {
    uint8_t *fb = pc98_framebuffer();
    if (!s_fb || !fb) {
        return 0;
    }
    while (s_pend >= 2) {
        vTaskDelay(1);
    }
    BlitJob j = { fn, arg, fb, ++s_post_seq };
    s_job_posted = 1;
    s_blit_src = fb;
    s_blit_busy = 1;
    __atomic_add_fetch(&s_pend, 1, __ATOMIC_SEQ_CST);
    xQueueSend(s_blit_q, &j, portMAX_DELAY);   // at most 2 pending, the queue holds 2
    return 1;
}
extern "C" bool lcd_comp_busy(void) {
    return s_done_seq != s_post_seq;
}
// 1 once per posted composite job: the blit of that frame is already on its way (core 0), main must not queue another.
extern "C" int lcd_take_job_flag(void) {
    int v = s_job_posted;
    s_job_posted = 0;
    return v;
}
extern "C" int lcd_pending(void) {
    return s_pend;
}


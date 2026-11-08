#include <errno.h>
// PC-98 emulator for the M5Stack Tab5 (ESP32-P4): start-up, the emulation task and the host loop.
#include <sys/stat.h>
#include <dirent.h>
#include <esp_system.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

extern "C" int ets_printf(const char *fmt, ...);
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "board_pins.h"                 // BRD_I2S_* (power-on anti-pop drives them low)
#include <nvs.h>
#include <nvs_flash.h>

extern "C" {
#include <compiler.h>
#include <pc98.h>
#include <kbd.h>
#include <hostfile.h>
#include <devices.h>
#include <video.h>
#include <sound.h>
#include <events.h>
#include <cpulink.h>
              // g_cpu.clocks_done/BASECLOCK/REMCLOCK (emulated cycle counter)
#include "util/pcstr.h"
#include "media/drives.h"
#include "dev/sasi.h"
#include "dev/ide.h"
uint8_t *pc98_framebuffer(void);   // from platform_stubs.cpp
bool fdd_ext_is(const char *name);
bool fdd_ext_mount(int drv, const char *path);
void fdd_ext_started(void);
void fdd_ext_tick(void);
bool pc98_framebuffer_init(void);      // pre-allocate the framebuffer (PSRAM) before machine_init
extern int g_pc98_draw_enabled;    // g_mach.c: screen-render gate (off during boot)
bool lcd_init(void);               // lcd_st7789.cpp: ST7789 on HSPI
void lcd_blit(const uint8_t *fb);  // blit framebuffer -> panel (async, core 0)
bool lcd_blit_busy(void);            // lcd_st7789.cpp: core-0 blit still reading fb
void lcd_set_scale_mode(int m);      // lcd_st7789.cpp: 0=MAX 1=AVG 2=MID 3=1:1
int  lcd_scale_mode_count(void);     // lcd_st7789.cpp: valid modes are 0..count-1
// Which ROM files the machine boots with, as chosen in the menu from whatever
// BIOS*.ROM / FONT*.ROM are in the SD root - so a ROM dumped from real
// hardware and a compatible one built for this port can sit side by side.
// Empty means "the default name". The machine reads both at machine_init, so a
// change only takes effect on the next boot; the menu says as much.
extern "C" char g_bios_file[40] = "";
extern "C" char g_font_file[40] = "";

void usb_kbd_init(void);           // usb_kbd.cpp: USB HID keyboard host
// No bt_hid here: the P4 has no radio of its own. Bluetooth would go
// through the companion ESP32-C6 over esp-hosted — a later phase.
int  usb_msc_boot_flag_take(void); // usb_msc.cpp: read+clear the USB Mode request (0 = none)
void usb_msc_report_last(void);    // usb_msc.cpp: how far the last USB Mode attempt got
void panel_tab5_blank_early(void);  // panel_tab5.cpp: kill the backlight and the panel rail
void usb_msc_restore_phy_map(void);  // usb_msc.cpp: undo USB Mode's PHY swap
void tab5_backlight_set(int percent);   // panel_tab5.cpp
bool touch_mouse_init(void);            // touch_mouse.cpp: touch panel as the mouse
void bt_hid_init(void);                 // bt_hid.cpp: BLE HID host via the ESP32-C6
void rtc_tab5_sync(void);               // rtc_tab5.cpp: the RX8130CE clock
void audio_set_volume(int percent);     // audio_codec.cpp

// Backlight percentage, shared with the menu row that changes it. Restored
// from NVS at boot, applied once the panel is up.
extern "C" int g_backlight_pct = 100;

// How the lines a 200-line screen leaves out are drawn, from the menu:
//   0  black, as the machine shows them (scanlines)
//   1  each graphics line drawn twice
//   2  each missing line halfway between the graphics line above and below
// Text has 400 lines of its own and is drawn at them in all three
// (video_set_line200, video/compose.c).
extern "C" void lcd_lamps_tick(void);    // lcd_mipi.cpp
extern "C" void lcd_lamps_enable(bool on);
extern "C" int g_line200 = 0;
extern "C" void audio_set_fx(int mode);   // audio_codec.cpp

extern "C" void line200_apply(int m) {
    g_line200 = (m >= 0 && m <= 2) ? m : 0;
    video_set_line200(g_line200);
}

// After a change while the machine is up: the whole screen again.
extern "C" void line200_refresh(void) {
    video_redraw_all();
}
// CPU chosen in the menu: 0 = V30, 1 = 80286, 2 = 80286 + 386 instructions. Read
// once at boot - the menu shows a change as pending until RESET.
extern "C" int g_cpu_mode = 2;
extern "C" int g_cpu_mode_boot = 2;
extern "C" void hid_mouse_delta(int dx, int dy, uint8_t buttons); // usb_kbd.cpp


extern "C" void hid_kbd_report(int src, const uint8_t *d, int len);
extern volatile int g_speed_req;
extern volatile int g_menu_req;
extern "C" void sd_unmount(void);
void usb_msc_run(int mode);        // usb_msc.cpp: USB mass storage (never returns)
int  usb_kbd_pop(uint8_t *nkey, uint8_t *down);  // drain one key event
extern volatile int g_menu_req;      // usb_kbd.cpp: Pause/Break -> disk swap menu
extern volatile int g_speed_req;     // menu_disk.cpp: CPU clock row -> new multiple
extern "C" int lcd_post_composite(void (*fn)(const void *), const void *arg);
extern "C" bool lcd_comp_busy(void);
extern "C" int lcd_pending(void);
extern "C" int lcd_take_job_flag(void);
extern "C" int (*g_comp_async)(void (*fn)(const void *), const void *arg);
extern "C" void hid_kbd_report(int src, const uint8_t *d, int len);
void menu_disk_run(void);            // menu_disk.cpp: modal disk UI (pauses emulation)
extern uint8_t mem[];              // core main RAM; text VRAM at 0xA0000
bool audio_init(int rate, int maxframes);        // audio_i2s.cpp: I2S DAC output
void audio_write_s32(const int32_t *pcm, const int32_t *dry, int frames);
}

// ---- FM sound (milestone ④, PC-9801-26K / YM2203) ----
// Default OFF: the emulator runs well below real time (~8-27% RT), so FM audio
// would be pitch-shifted / underrunning, and FM synthesis further slows the
// already CPU-bound emulation. The whole path (26K board + I2S out on 38/39/40)
// is implemented and boots when enabled — flip this to 1 to try it. Audio output
// correctness is unverified (no way to listen; DAC hardware on S3 unconfirmed).
#define ENABLE_FM_SOUND   1
#define FM_RATE           22050
#define FM_DELAY_MS       20
static int g_fm_frames = 0;        // stereo frames per snd_take block
// Test aids over the serial line: T 1 / T 0 runs the machine flat out (no real-time wait), E prints the emulated
// time since the machine started (ms), so a test can take its pictures at emulated moments.

#if ENABLE_FM_SOUND
// The FM / SSG / beeper synthesis runs on core 0 (sound/mixer.c, asynchronous mode): the machine on core 1 queues
// the chips' register changes with their emulated clock and, once a frame, publishes how far it has got and how
// many samples the emulated time owes the codec; this task renders up to there, applying the changes in order, and
// hands the blocks to the codec ring. Core 1 keeps the time the synthesis took for the CPU.
static TaskHandle_t      s_snd_task;
static SemaphoreHandle_t s_snd_mux;
static volatile int32_t  s_snd_owed;            // samples owed (rate-matched drain; may go negative)

static void snd_lock_fn(int lock) {
    if (lock) xSemaphoreTakeRecursive(s_snd_mux, portMAX_DELAY);
    else xSemaphoreGiveRecursive(s_snd_mux);
}

static void snd_kick_fn(void) {                 // the change queue is full: let the task drain it
    xTaskNotifyGive(s_snd_task);
    vTaskDelay(1);
}

static void snd_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        snd_lock_fn(1);
        snd_service();
        // ...and also whenever the chips have rendered more than a block ahead of the count (their 22050 Hz is a
        // little faster than it): the owed count goes negative and evens it out (see the synchronous drain below).
        for (int guard = 0;
             (__atomic_load_n(&s_snd_owed, __ATOMIC_ACQUIRE) >= g_fm_frames || snd_pending() >= (UINT)(2 * g_fm_frames)) && guard < 8;
             guard++) {
            const SINT32 *pcm = snd_take();
            if (!pcm) break;
            audio_write_s32((const int32_t *)pcm, (const int32_t *)snd_take_dry(), g_fm_frames);
            snd_release(pcm);
            __atomic_sub_fetch(&s_snd_owed, g_fm_frames, __ATOMIC_ACQ_REL);
        }
        snd_lock_fn(0);
    }
}

static void snd_task_start(void) {
    s_snd_mux = xSemaphoreCreateRecursiveMutex();
    if (!s_snd_mux) return;
    if (xTaskCreatePinnedToCore(snd_task, "sound", 4096, nullptr, 5, &s_snd_task, 0) != pdPASS) return;
    g_snd_kick = snd_kick_fn;
    g_snd_lock = snd_lock_fn;
    snd_lock_fn(1);
    snd_set_async(1);
    snd_lock_fn(0);
}
#endif



// ---- real-time pacing (throttle) ----
// The CPU core can run FASTER than a real VM21 (BIOS phase hit ~10x), which
// makes VSYNC-timed software run too fast and FM audio play at the wrong tempo.
// Pace the loop so emulated time advances at real time: after each frame, sleep
// until wall-clock catches up to the emulated cycles executed (cycles/realclock).
// Heavy frames that can't keep up simply run flat-out (no negative sleep); a debt
// clamp stops them from banking "fast time" credit afterwards.
#define ENABLE_REALTIME   1

// ---- USB HID host (native USB-OTG in host mode) ----
// The native USB port is EITHER a host (keyboard/mouse) OR the USB-Serial-JTAG
// console — the S3 has one USB PHY and cannot do both. The board's only other
// serial pins, UART0 43/44, are now the external I2S DAC's BCLK/LRC, so USB is
// the only console left. Since bt_hid.cpp provides a BLE keyboard and mouse,
// USB host is off and the console is back: CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG.
// Flip this to 1 to trade the console back for USB HID input.
#define ENABLE_USB_HOST   1

// Output via ets_printf: writes straight to the UART ROM routine, bypassing the
// stdio/VFS layer (which crashed after the emulator ran). No %f support → integers.
extern "C" int ets_printf(const char *fmt, ...);
#define printf ets_printf

// Boot-phase text renderer. The BIOS_IO_EMULATION BIOS writes the text VRAM
// directly without programming the GDC, so the renderer composites nothing
// until DOS/a game sets a video mode — the memory count and the EPSON [SHIFT]
// message never reach the LCD. While g_frames_composed is still 0 (no real composite
// has ever happened), render the 80x25 text VRAM into the framebuffer with the
// CGROM font (8x16 ANK cells) so the boot sequence shows like on real hardware.
static void render_boot_text(uint8_t *fb) {
   uint16_t *d = (uint16_t *)fb;
    for (int cy = 0; cy < 25; cy++) {
        for (int cx = 0; cx < 80; cx++) {
            uint8_t code = mem[0xA0000 + (cy * 80 + cx) * 2];
            // ANK 8x16 lives at 0x80000 in the CG ROM image (cgrom.c); offset 0
            // is the kanji area, whose ASCII range the loader zeroes - reading there
            // drew blank cells.
            const uint8_t *glyph = cgrom + 0x80000 + (unsigned)code * 16;
            for (int r = 0; r < 16; r++) {
                uint8_t bits = glyph[r];
                uint16_t *px = d + (cy * 16 + r) * 640 + cx * 8;
                for (int b = 0; b < 8; b++)
                    px[b] = (bits & (0x80 >> b)) ? 0xFFFF : 0x0000;
            }
        }
    }
}

// SD wiring — Anemoia config.h (ESP32-S3, FSPI)
static const int SD_CS = 4, SD_SCK = 5, SD_MISO = 6, SD_MOSI = 7;

static void banner(const char *s) { printf("\n==== %s ====\n", s); }

// Disk image paths on the SD card.
#define IMG_FDD1  "/FD.NFD"
#define IMG_FDD2  "/FD2.NFD"
#define IMG_HDD   "/HDD.NHD"

// Boot media comes from NVS (the disk menu persists FDD1/FDD2/HDD on ESC/RESET).
// With no saved disk data, nothing is mounted and the BIOS drops to N88-BASIC
// (BIOS.ROM). Otherwise every saved image that exists on the SD is mounted, and
// the PC-98 BIOS boots the first of FDD1 -> FDD2 -> HDD that carries bootable
// media (BIOS scans the FDDs before the HDD).

// True if a file exists on the SD (via hostfile.h, same paths as mounts).
static bool sd_file_exists(const char *path) {
    HFILE h = hf_open((const char *)path);
    if (h == NULL) return false;
    hf_close(h);
    return true;
}

static void emu_task(void *arg);
extern "C" bool sd_mount(void);
// The card answered at boot. Without it the menu must not store the ROM and disk names: they read as missing
// and would be saved as empty, so the next boot with the card back would lose them.
bool g_sd_ok = false;


       // sd_tab5.cpp: SDMMC via the BSP, mounted at /sd

// Emulator task stack lives in internal .bss (placed at link time), not the
// heap: moving the hot CPU data (szpflag_w etc.) into internal DRAM fragmented
// the heap so a 64KB contiguous heap block was no longer available. A static
// stack sidesteps heap fragmentation entirely.
// 36KB, not 56KB. This is static .bss in internal RAM, the pool the BLE
// controller, the I2S buffers, the SD driver and the panel's bounce buffers all
// compete for. Measured with uxTaskGetStackHighWaterMark after a full DOS boot
// (the deepest disk and BIOS paths): 22,844 bytes actually used - the same
// figure the ST7789 build reported. 36KB keeps
// a 14KB margin and hands 20KB back to the heap.
#define EMU_STACK_WORDS (36 * 1024 / sizeof(StackType_t))
static StackType_t  emu_stack[EMU_STACK_WORDS];

// USB Mode runs instead of the emulator and leaves only by rebooting, so
// this stack is 36KB of internal RAM that is reserved and idle for as long
// as that mode lasts. It is the largest such block in the firmware, and USB
// Mode was running out of exactly this kind of memory, so it may borrow it.
// Nothing else may: the emulator task is created with it the moment the
// ordinary boot path reaches the end.
extern "C" void *emu_stack_borrow(size_t *len) {
    if (len) {
        *len = sizeof(emu_stack);
    }
    return emu_stack;
}
static StaticTask_t emu_tcb;

// Nothing is printed on the serial console: a message the PC is not reading can hold up the core that writes it.
static void console_null_putc(char c) { (void)c; }

extern "C" void app_main(void) {
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_rom_install_channel_putc(1, console_null_putc);
    esp_rom_install_channel_putc(2, console_null_putc);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
    // No I2S pins to hold down at reset here: the speaker sits behind a codec
    // whose enable is on an I/O expander, and the BSP leaves it off until
    // bsp_audio_codec_speaker_init(). The S3 boards had a bare amplifier
    // wired to three GPIOs that squawked while they floated.

    // --- Bluetooth LE HID host (BLE keyboard/mouse) — brought up here, first
    // thing, because it is the only consumer that CANNOT fall back to PSRAM: the
    // BLE controller needs one large contiguous block of internal DMA memory.
    // Waiting even until emu_task left just 34KB contiguous (of 114KB free) —
    // enough allocations had already been sprinkled through the internal heap to
    // break it up — and the controller answers a failed allocation by crashing
    // in its own cleanup path rather than returning an error. So: before
    // initArduino(), before the RGB panel's bounce buffers, before SDMMC.
    // NVS first, since the BLE bonding keys are stored there.
    {
        esp_err_t nerr = nvs_flash_init();
        if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }
    }
    // --- USB Mode (SD card reader) --- chosen from the disk menu, which sets an
    // NVS flag and reboots. Checked here, before anything else claims memory or
    // the USB PHY: USB Mode runs no emulator, no Bluetooth and no arduino layer.
    // The flag is cleared as it is read, so a replug/RESET returns to normal.
    // Before anything else, and before the USB Mode check: if the last run was
    // USB Mode and the board was reset rather than power-cycled, the FSLS PHY
    // mapping is still swapped and there is no console. Undoing it here is what
    // makes RESET a way out of USB Mode. usb_msc_run() sets it again for itself.
    usb_msc_restore_phy_map();
    panel_tab5_blank_early();

    const int usb_mode = usb_msc_boot_flag_take();
    if (usb_mode) {
        usb_msc_run(usb_mode);           // never returns
    }


    vTaskDelay(pdMS_TO_TICKS(1500));
    // Run the emulator on a dedicated task (internal static stack, core 1).
    xTaskCreateStaticPinnedToCore(emu_task, "emu", EMU_STACK_WORDS, nullptr, 5,
                                  emu_stack, &emu_tcb, 1);
    vTaskDelete(nullptr);
}

static void emu_task(void *arg) {
    (void)arg;
    banner("pc98tab5");
    printf("PSRAM: total=%u free=%u\n", (unsigned)esp_psram_get_size(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("internal heap free=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // --- LCD (ST7701S RGB) — MUST init before SD: the ST7701S 3-wire init uses
    // GPIO 1/2, the same pins as the SDMMC CLK/CMD. LCD_Init() finishes that SPI
    // init and hands 1/2 to the RGB interface, freeing them for the SD bus. ---
    // --- Bluetooth LE HID host, through the companion ESP32-C6 ---
    // FIRST, before the display, the card and the USB host. All three take
    // internal DMA-capable memory and never give it back, and the BLE stack
    // cannot use PSRAM for any of what it needs. Started last it got 109KB with
    // a 53KB largest block and could not even create its memory pool; started
    // here it has the whole pool to choose from. Nothing above this point has
    // allocated anything but the frame buffer.
    bt_hid_init();

    printf("LCD init: %s\n", lcd_init() ? "OK" : "FAIL");

    // --- SD (SDMMC 1-bit via BSP: CLK2/CMD1/D0=42/D3=EXIO4, mounted at /sd) ---
    g_sd_ok = sd_mount();

    // If the last boot went into USB Mode and came back, say how far it got and
    // write it to the card. After sd_mount() because of the file, and the file
    // is the point: reading this over the serial port resets the chip, and the
    // reset clears the RTC memory the record lives in.
    usb_msc_report_last();

    // --- USB HID keyboard/mouse, on the USB-A port ---
    // The P4 has
    // two USB peripherals, each with its own PHY - usb_host_install() takes the USB 2.0 HS one
    // (UTMI), and the console keeps the other. So it is simply on here.
#if ENABLE_USB_HOST
    usb_kbd_init();
#endif

    // --- The real-time clock ---
    // The BSP leaves the RX8130CE alone, so nothing had told the system what
    // time it is and every file written to the card was stamped 1980. Must
    // come after sd_mount(), because this is also what reads SETTIME.TXT.
    rtc_tab5_sync();


    // --- Touch panel as the PC-98 mouse ---
    // After the panel: bsp_touch_new() talks to a controller that sits on the
    // display module's rail, which panel_tab5_init() power-cycles.
    touch_mouse_init();

    // --- machine configuration ---
    machine_defaults();
    pcs_copy(g_settings.model, "VX", sizeof(g_settings.model));   // PC-9801VX (80286: XMS/HIMEM capable)
    // The VX has an EGC, and the default here (2) left it out. Software that
    // copies VRAM with the EGC then drew solid blocks through the plain GRCG instead.
    g_settings.gfx_chip = 3;
    // Emulated V30 @ ~4.92MHz = base 2.4576MHz x2. x4 (authentic VM21 9.83MHz)
    // left the CPU core at ~17ms/frame with ZERO graphics — borderline for
    // real time, so games dipped below it and FM audio (22050 samples/s tied
    // to emulated time) crackled from chronic supply deficit. x3 (~12ms) was
    // enough at rest but marginal under load; x2 (~8ms) gives a wide margin.
    // Measured on this board with the perf log, idle at the DOS prompt, as
    // real-time headroom left per frame: x4 8.3ms, x5 6.5ms, x6 4.6ms,
    // x8 2.5ms, x10 zero. So x10 is where the P4 stops keeping up even with
    // nothing on screen, and x5 keeps roughly a third of the frame in hand for
    // whatever the software is actually doing. The default is x3; the menu
    // still goes up to x10.
    g_settings.clock_mul = 3;
    // Extended (XMS) memory. 13MB is the most a PC-9801VX could take, and _MALLOC lands it in PSRAM -
    // the P4 has 32MB and about 24MB of it free at this point, so the old 1MB
    // (chosen when the S3's 8MB had to cover the frame buffers too) has no
    // reason to stay.
    g_settings.ext_mb = 13;
    // Key repeat: the keyboard unit re-sends the newest key held from
    // keyunit_tick(), which machine_frame() calls once a frame - but only when
    // this is on, and nothing here ever turned it on, so
    // a held key was sent once and never again. The delay must stay larger
    // than the interval: keyunit_tick() waits (delay - interval) before the
    // first repeat, and both are unsigned 16-bit.
    g_settings.key_repeat   = 1;
    g_settings.key_repeat_delay_ms    = 500;   // ms held before it starts repeating
    g_settings.key_repeat_rate_ms = 60;    // ms between repeats after that
#if ENABLE_FM_SOUND
    // PC-9801-26K (YM2203: 3 FM + SSG). machine_init opens the mixer with these.
    g_settings.sound_board  = SOUND_26K;
    g_settings.sample_rate = FM_RATE;
    g_settings.audio_block_ms   = FM_DELAY_MS;
    // Headroom: 3 FM + 3 SSG channels summed at full volume can exceed int16 and
    // clip (buzz on loud/bass passages). 70% master leaves room for the mix.
    g_settings.volume_all = 70;
    g_settings.volume_fm    = 100;
    g_settings.volume_ssg   = 100;
    g_fm_frames      = (FM_RATE * FM_DELAY_MS) / 1000;
#else
    g_settings.sound_board = SOUND_NONE;                          // FM off (see ENABLE_FM_SOUND)
#endif
    pcs_copy(g_settings.rom_dir, "/", sizeof(g_settings.rom_dir));
    {   // The ROM names chosen in the menu. Read before machine_init, which
        // is where the machine actually opens them.
        nvs_handle_t nh;
        if (nvs_open("pc98", NVS_READONLY, &nh) == ESP_OK) {
            size_t n = sizeof(g_bios_file);
            if (nvs_get_str(nh, "biosfile", g_bios_file, &n) != ESP_OK) g_bios_file[0] = 0;
            n = sizeof(g_font_file);
            if (nvs_get_str(nh, "fontfile", g_font_file, &n) != ESP_OK) g_font_file[0] = 0;
            nvs_close(nh);
        }
    }
    // A ROM deleted from the card since it was chosen must not wedge the
    // machine on a name that cannot be opened.
    if (g_bios_file[0] && !sd_file_exists(g_bios_file)) g_bios_file[0] = 0;
    if (g_font_file[0] && !sd_file_exists(g_font_file)) g_font_file[0] = 0;
    // Same rule as the BIOS: the menu's choice wins, otherwise the font built
    // into the firmware - not a FONT.ROM that happens to be on the card.
    pcs_copy(g_settings.font_file, g_font_file[0] ? g_font_file : ":builtin/FONT.ROM",
                sizeof(g_settings.font_file));
    printf("font file: %s\n", g_settings.font_file);
    // Real /bios.rom or the built-in BIOS is decided below, once we know whether
    // any disk is mounted — see the have_media block.

    // Equip 2 floppy drives (VM21 has two). fdc.equip is loaded from this in the
    // FDC reset (dev/fdc765.c); a drive whose equip bit is clear does not exist
    // for the machine — so drive 1 (FDD2) needs bit 1 set here.
    g_settings.fdd_fitted |= 0x03;

    // 1.44MB floppy support. fdc_reset() copies this into fdc.support144, and
    // without it setfdcmode() rejects every 1.44MB access (bios1b.c: "if ((rpm)
    // && (!fdc.support144)) return FAILURE") - the BIOS then cannot read the
    // disk's first sector and simply stops after the memory count, with no error
    // and no BASIC banner. A 1.2MB disk is unaffected either way, so this is
    // always on: the emulated VX gains the 1.44MB drive interface (port 0x4be)
    // that later real machines had.
    g_settings.fdd_144 = 1;

    // --- HDD (SASI/IDE fixed drive 0). hddbind() inside machine_reset() opens
    //     sasihdd[0], so the image must be set BEFORE then. The path is chosen
    //     from NVS below (after hf_init, so file existence can be checked);
    //     empty here means "no HDD". hddmedia.c reads NHD/HDI/THD/HDN/HDS. ---
    g_settings.hdd_image[0][0] = '\0';        // no HDD unless NVS names one that exists

    // --- persisted disk state (NVS "pc98"): CPU multiple, LCD scaler, and the
    //     FDD1/FDD2/HDD image paths (written by the disk menu on ESC/RESET). Read
    //     the paths into buffers here; the actual mounts happen after hf_init
    //     (so file existence can be checked). If NO disk keys are stored,
    //     have_nvs_disks stays false and nothing is mounted -> N88-BASIC. ---
    char saved_hdd [sizeof(g_settings.hdd_image[0])] = {0};
    char saved_fdd0[sizeof(g_settings.fdd_image[0])] = {0};
    char saved_fdd1[sizeof(g_settings.fdd_image[1])] = {0};
    bool have_nvs_disks = false;
    {
        esp_err_t nerr = nvs_flash_init();
        if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nerr = nvs_flash_init();
        }
        nvs_handle_t nh;
        if (nerr == ESP_OK && nvs_open("pc98", NVS_READONLY, &nh) == ESP_OK) {
            uint8_t v;
            // Rounded to the 10% the menu now steps in: earlier builds had a
            // 10/25/50/75/100 ladder and those values are still in NVS.
            if (nvs_get_u8(nh, "backlight", &v) == ESP_OK && v >= 5 && v <= 100) {
                g_backlight_pct = ((v + 5) / 10) * 10;
                if (g_backlight_pct < 10) {
                    g_backlight_pct = 10;
                }
                tab5_backlight_set(g_backlight_pct);
            }
            if (nvs_get_u8(nh, "volume", &v) == ESP_OK && v <= 100) {
                audio_set_volume(((v + 5) / 10) * 10);
            }
            if (nvs_get_u8(nh, "cpumode", &v) == ESP_OK && v <= 2)
                g_cpu_mode = v;
            if (nvs_get_u8(nh, "clkbase8", &v) == ESP_OK && v <= 1)
                g_settings.base_clock = v ? CLK_BASE8 : CLK_BASE5;
            if (nvs_get_u8(nh, "multiple", &v) == ESP_OK && v >= 1 && v <= CPU_MULT_MAX)
                g_settings.clock_mul = v;
            if (nvs_get_u8(nh, "scaler", &v) == ESP_OK && v < lcd_scale_mode_count())
                lcd_set_scale_mode(v);
            if (nvs_get_u8(nh, "line200", &v) == ESP_OK && v <= 2)
                line200_apply(v);
            if (nvs_get_u8(nh, "soundfx", &v) == ESP_OK && v <= 4)
                audio_set_fx(v);
            if (nvs_get_u8(nh, "lamps", &v) == ESP_OK)
                lcd_lamps_enable(v != 0);
            size_t len;
            len = sizeof(saved_hdd);
            if (nvs_get_str(nh, "hdd",  saved_hdd,  &len) == ESP_OK) have_nvs_disks = true;
            len = sizeof(saved_fdd0);
            if (nvs_get_str(nh, "fdd0", saved_fdd0, &len) == ESP_OK) have_nvs_disks = true;
            len = sizeof(saved_fdd1);
            if (nvs_get_str(nh, "fdd1", saved_fdd1, &len) == ESP_OK) have_nvs_disks = true;
            nvs_close(nh);
        }
    }

    // The V30 is DIP switch 3-8, read by machine_reset. The 386 is the 286
    // core with its 386 instructions switched on.
    if (g_cpu_mode == 0) {
        g_settings.dip_switch[2] |= 0x80;
    } else {
        g_settings.dip_switch[2] &= (UINT8)~0x80;
    }
    g_cpu.x386.enabled = (g_cpu_mode == 2);
    g_cpu_mode_boot = g_cpu_mode;
    ets_printf("cpu: mode %d (0 V30, 1 286, 2 286+386), 386 extensions %s\n",
               g_cpu_mode, g_cpu.x386.enabled ? "on" : "off");

    hf_init();
    keyunit_init();

    // HDD mount decision (before machine_reset()'s hddbind opens sasihdd[0]):
    // mount only when NVS names an HDD image that actually exists on the SD.
    //
    // There used to be an "else mount /HDD.NHD anyway" fallback here, from when
    // this board had no menu to pick a disk with. It made ejecting the HDD
    // impossible: the eject writes an empty HDD path to NVS and reboots, and the
    // fallback mounted the file straight back - so a bootable floppy could never
    // get its turn. The menu draws text now, so the disk set is the user's.
    if (have_nvs_disks && saved_hdd[0] && sd_file_exists(saved_hdd))
        pcs_copy(g_settings.hdd_image[0], (const char *)saved_hdd,
                     NELEMENTS(g_settings.hdd_image[0]));
    // First run only: a board whose NVS has never been written has no disk set
    // at all, and with nothing mounted the built-in BIOS stops at a screen that
    // draws nothing — so a brand new board looks broken. Mount /HDD.NHD if it
    // is there, just this once.
    //
    // This is NOT the fallback that was removed above. That one keyed on the
    // saved HDD path being EMPTY, which is exactly what ejecting writes, so it
    // remounted the disk the user had just ejected. This keys on the NVS entry
    // being ABSENT: an eject stores an empty string, the key then exists, and
    // this never fires again.
    else if (!have_nvs_disks && sd_file_exists("/HDD.NHD")) {
        pcs_copy(g_settings.hdd_image[0], (const char *)"/HDD.NHD",
                     NELEMENTS(g_settings.hdd_image[0]));
        printf("disks: first run, defaulting to /HDD.NHD\n");
    }
    // Which BIOS to run. With a disk mounted we want the real /bios.rom, since
    // that is what boots real media. With nothing mounted the real ROM falls
    // through to its N88-BASIC banner screen, so we deliberately keep the
    // built-in BIOS there instead: it stops at its own "insert the system disk"
    // prompt, which is the correct thing to show and carries no NEC copyright
    // notice. FDD mounts happen after machine_reset(), so test the saved paths
    // rather than g_settings.fdd_image — but this must be settled before machine_init(),
    // which is where bios_initialize() reads the flag.
    const bool have_media = g_settings.hdd_image[0][0] ||
                            (have_nvs_disks && saved_fdd0[0] &&
                             sd_file_exists(saved_fdd0)) ||
                            (have_nvs_disks && saved_fdd1[0] &&
                             sd_file_exists(saved_fdd1));
    printf("BIOS: media=%d  /bios.rom on SD=%d\n",
           (int)have_media, (int)sd_file_exists("/bios.rom"));
    printf("framebuffer alloc: %s\n", pc98_framebuffer_init() ? "OK (640x480 PSRAM)" : "FAIL");
    banner("machine_init");
    printf("free before machine_init: internal=%u psram=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    machine_init();
    machine_reset();
    // Which file was taken is on the "biosrom:" line from bios.c. This says
    // only whether a real ROM image is in memory at all, as against the
    // built-in BIOS - naming a file here read as though the one on the
    // card had been used, which is the opposite of what happens.
    printf("BIOS ROM: %s\n", g_mach.rom_bios ? "real ROM image in memory" : "emulated (dummy BASIC)");

    // --- mount floppies (after machine_reset). Load NOW (setup context) via
    //     readyfddex instead of the delayed setfdd: the delayed load runs inside
    //     machine_frame where fopen()/SD crash. Opening here keeps the handle open so
    //     the FDC streams sectors via read()/lseek during emulation.
    //     Mount each saved floppy whose file exists; with no NVS disk data both
    //     drives stay empty and the BIOS drops to N88-BASIC. ---
    if (have_nvs_disks) {
        if (saved_fdd0[0] && sd_file_exists(saved_fdd0) && fdd_ext_is(saved_fdd0)) {
            fdd_ext_mount(0, saved_fdd0);
        } else if (saved_fdd0[0] && sd_file_exists(saved_fdd0)) {
            if (drv_fd_mount(0, saved_fdd0) == 0) pcs_copy(g_settings.fdd_image[0], saved_fdd0, sizeof(g_settings.fdd_image[0]));
        }
        if (saved_fdd1[0] && sd_file_exists(saved_fdd1) && fdd_ext_is(saved_fdd1)) {
            fdd_ext_mount(1, saved_fdd1);
        } else if (saved_fdd1[0] && sd_file_exists(saved_fdd1)) {
            if (drv_fd_mount(1, saved_fdd1) == 0) pcs_copy(g_settings.fdd_image[1], saved_fdd1, sizeof(g_settings.fdd_image[1]));
        }
    }
    machine_sync_switches();
    printf("disks: nvs=%d fdd1=[%s] fdd2=[%s] hdd=[%s]\n", (int)have_nvs_disks,
           g_settings.fdd_image[0], g_settings.fdd_image[1], g_settings.hdd_image[0]);
    printf("fddequip=%02X  drv_fd_ready(0)=%d  drv_fd_ready(1)=%d\n",
           g_settings.fdd_fitted, drv_fd_ready(0), drv_fd_ready(1));

    // Internal DMA RAM is the scarce resource on this board (BLE controller, I2S
    // buffers, SD transfers). The SD driver allocates a small aligned buffer per
    // transfer for anything FATFS reads through its own window, so a near-zero
    // figure here means disk reads are about to start failing with ESP_ERR_NO_MEM.
    printf("internal DMA heap: free=%u largest=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    banner("running");
    fdd_ext_started();

    printf("free after reset: internal=%u psram=%u largest_psram=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    // HDD status: the image named by sasihdd[0] was opened at reset (dev/sasi_bind.c logs its geometry).
    // An empty sasihdd[0] means "no HDD" was requested (BASIC / floppy boot), not an error.
    if (sasi_disk(0) || ide_disk(0)) {
        printf("HDD: open OK [%s] on %s\n", g_settings.hdd_image[0], sasi_disk(0) ? "SASI" : "IDE");
    } else if (g_settings.hdd_image[0][0]) {
        printf("HDD: [%s] NOT open — check the image on SD\n", g_settings.hdd_image[0]);
    } else {
        printf("HDD: none mounted (boot to N88-BASIC or floppy)\n");
    }

#if ENABLE_FM_SOUND
    printf("FM: SOUND_SW=%02X rate=%d frames/blk=%d  audio_init=%s\n",
           g_settings.sound_board, FM_RATE, g_fm_frames,
           audio_init(FM_RATE, g_fm_frames) ? "OK" : "FAIL");
    snd_task_start();
#endif

    // --- run the machine continuously; render + push to the LCD once booted ---
    uint8_t *fb = pc98_framebuffer();
    int total = 0;
    // Rendering (text, graphics, composite) is the biggest per-frame cost
    // after the PSRAM-bound CPU, and the LCD only changes when we blit. So run
    // most frames with draw=FALSE (machine_frame skips drawscreen) and only render
    // + blit 1 in RENDER_EVERY frames. Big speedup with a still-usable ~emu/N LCD.
    // video_frame() self-skips when nothing changed (dirty tracking: only ~13
    // composites per 300 idle frames), so we let it run every frame — it's ~free
    // when static — and only pay the 25ms LCD blit on frames that actually drew.
    const int RENDER_EVERY = 2;   // drawscreen every 2nd frame: frees core 1 so emu hits real time (audio needs 22050 samples/s)
    // Emulated-time bookkeeping, shared by the FM drain and the real-time throttle:
    // dcyc = emulated CPU cycles executed per frame; both audio sample count and the
    // wall-clock target derive from it (via g_mach.cpu_clock).
    uint32_t rt_hz = g_mach.cpu_clock ? g_mach.cpu_clock : 9984000; // re-read each frame (F11 changes it)
    uint32_t rt_prevcyc = (uint32_t)(g_cpu.clocks_done + g_cpu.slice_len - g_cpu.slice_left);
    int64_t  rt_t0 = esp_timer_get_time();     // wall baseline
    int64_t  rt_emu_us = 0;                     // accumulated emulated time (us)
    int64_t  snd_owed = 0;                      // FM: audio samples owed (rate-matched drain)
    printf("realtime pacing ON: realclock=%u Hz\n", (unsigned)rt_hz);
    g_comp_async = lcd_post_composite;      // stage 11: the composite runs on core 0
    for (;;) {
        // Disk swap menu (Pause/Break): modal — machine_frame pauses inside.
        if (g_menu_req) {
            g_menu_req = 0;
            // In-flight composite/blit jobs (core 0) must finish before the menu draws, or they land on top of it.
            for (int w = 0; (lcd_blit_busy() || lcd_comp_busy()) && w < 2000; w++) vTaskDelay(1);
            menu_disk_run();
            if (fb) lcd_blit(fb);   // menu drew straight to the LCD: restore the emulated screen
            // Time stopped while the menu was open: rebase the pacing clocks so
            // the throttle/audio drain don't try to "catch up" the menu time.
            rt_t0 = esp_timer_get_time();
            rt_emu_us = 0;
            rt_prevcyc = (uint32_t)(g_cpu.clocks_done + g_cpu.slice_len - g_cpu.slice_left);
        }

        fdd_ext_tick();

        // Disk-menu CPU row: change the emulated clock at runtime.
        // Every clocked subsystem (sound, keyboard, mouse, GDC, events) follows.
        if (g_speed_req) {
            UINT oldm = g_mach.clock_mul;
            UINT newm = (UINT)g_speed_req;
            g_speed_req = 0;
            if (newm < 1) newm = 1;
            if (newm > CPU_MULT_MAX) newm = CPU_MULT_MAX;
            g_mach.clock_mul = g_settings.clock_mul = newm;
            g_mach.cpu_clock = g_mach.base_clock * newm;
            ev_rescale(oldm, newm);
            snd_lock(1);
            snd_clock_changed();
            snd_lock(0);
            kbd_changeclock();
            mouse_changeclock();
            gdc_timing();
            rt_emu_us = esp_timer_get_time() - rt_t0;   // rate changed: don't bank false debt
            printf("cpu multiple -> %u (%u Hz)\n", (unsigned)newm, (unsigned)g_mach.cpu_clock);
        }
        rt_hz = g_mach.cpu_clock ? g_mach.cpu_clock : 9984000;


        // Feed queued USB-keyboard events into the emulated keyboard.
        uint8_t nk, dn;
        while (usb_kbd_pop(&nk, &dn)) {
            keyunit_key(nk, dn);
        }

        // Regular-cadence render scheduling. Bursty catch-up renders look like
        // stutter even at the same average fps, so the LCD cadence itself is
        // load-adaptive: light load -> every 2nd frame (~28fps), heavy load ->
        // every 4th frame (~14fps, but a STEADY 14). Rendering is only skipped
        // outright when emulated time is >100ms behind wall clock — audio is
        // tied to emulated time, and that guard keeps the FM stream fed.
        int64_t debt_us = rt_emu_us - (esp_timer_get_time() - rt_t0);   // <0 = behind
        // Display is decoupled from emulation pacing: when the composite runs on core 0 (g_comp_async), a frame is rendered
        // whenever RENDER_EVERY guest frames have passed since the last one and core 0 has finished the previous job (composite
        // and blit); a busy core 0 only postpones the render to the next frame, it never blocks core 1 and the debt does not
        // thin the cadence. Only a debt that eats into the audio queue (80-150 ms) skips renders: more than 50 ms behind.
        // (The old 250 ms never came: the debt is forgiven at 200 ms below.)
        // Without the core-0 path (sync composite) the old load-adaptive cadence applies.
        static int since_render = 0;
        const int RENDER_MIN = 2;      // every 2nd guest frame, steadily: a render that slips to the 3rd frame shows one picture late and the next one twice
        int every = RENDER_EVERY, render;
        since_render++;
        if (g_comp_async) {
            int c0_free = !lcd_comp_busy() && lcd_pending() < 2;   // composite done and at most the previous blit still running
            // On the 2nd frame core 0 is normally a millisecond or two short of done (composite + PPA is ~34 ms against 33.3):
            // wait that out instead of slipping a frame, but never long enough to cost the machine real time.
            if (!c0_free && g_pc98_draw_enabled && since_render == RENDER_MIN && debt_us > -10000) {
                for (int w = 0; w < 8 && !c0_free; w++) { vTaskDelay(1); c0_free = !lcd_comp_busy() && lcd_pending() < 2; }
            }
            // Falling behind: a steady third frame (20 fps) from 20 ms, a sixth (10 fps) from 35 ms, leave core 1 the time
            // the renders took, so the machine catches up without the screen stopping; only past 50 ms are renders skipped.
            const int gap = (debt_us < -35000) ? 6 : (debt_us < -20000) ? 3 : RENDER_MIN;
            render = g_pc98_draw_enabled && since_render >= gap && c0_free && (debt_us > -50000);
            if (render) since_render = 0;
        } else {
            every = (debt_us > -30000) ? RENDER_EVERY : 4;
            render = g_pc98_draw_enabled && ((total % every) == 0) && (debt_us > -100000);
        }
        // Tear-free: don't let this frame's drawscreen rewrite fb while the
        // core-0 blit is still reading it. A full non-render frame has passed
        // since the enqueue, so the blit is normally long done (zero wait).
        if (render) { while (!g_comp_async && lcd_blit_busy()) vTaskDelay(1); }
        UINT dc0 = g_frames_composed;
        machine_frame(render ? TRUE : FALSE);
        // Only push to the LCD when drawscreen actually re-composited the frame
        // (g_frames_composed advanced) — skips the 25ms blit while the screen is static.
        if (render && fb) {
            if (g_frames_composed != dc0) {
                if (!lcd_take_job_flag()) lcd_blit(fb);
            } else if (g_frames_composed == 0 && (total % 8) == 0) {
                render_boot_text(fb);   // boot phase only: GDC not programmed yet
                lcd_blit(fb);
            }
        }

        total++;
        lcd_lamps_tick();                // the drive lamps beside the picture

        // Emulated CPU cycles executed this frame (unsigned delta is wrap-safe;
        // clamp resets/garbage to ~one frame). Shared by FM drain + throttle.
        uint32_t cyc  = (uint32_t)(g_cpu.clocks_done + g_cpu.slice_len - g_cpu.slice_left);
        uint32_t dcyc = cyc - rt_prevcyc;
        rt_prevcyc = cyc;
        if (dcyc == 0 || dcyc > rt_hz / 20) dcyc = rt_hz / 60;  // sane: <1/20s

#if ENABLE_FM_SOUND
        // Rate-matched drain: emit audio in proportion to emulated time (dcyc), so
        // tempo is correct at any frame rate. (A fixed block/frame ran ~12% fast.)
        {
            if (snd_is_async()) {       // the sound task renders and drains (snd_task)
                __atomic_add_fetch(&s_snd_owed, (int32_t)((int64_t)dcyc * FM_RATE / rt_hz), __ATOMIC_ACQ_REL);
                xTaskNotifyGive(s_snd_task);
                snd_owed = 0;
            }
            snd_owed += (int64_t)dcyc * FM_RATE / rt_hz;
            // ...and also whenever the chips have rendered more than a block
            // ahead of that. They run at their own idea of 22050Hz, a little
            // faster than this count; the difference piled up in the mixer
            // until it overflowed, and the samples it threw away were
            // a click each time. snd_owed goes negative and evens it out.
            for (int guard = 0;
                 !snd_is_async() && (snd_owed >= g_fm_frames || snd_pending() >= (UINT)(2 * g_fm_frames)) && guard < 8;
                 guard++) {
                const SINT32 *pcm = snd_take();
                if (!pcm) break;
                // The beep comes separately, so the FM sound effects skip it.
                audio_write_s32((const int32_t *)pcm, (const int32_t *)snd_take_dry(), g_fm_frames);
                snd_release(pcm);
                snd_owed -= g_fm_frames;
            }
        }
#endif

#if ENABLE_REALTIME
        rt_emu_us += (int64_t)dcyc * 1000000 / rt_hz;
        {
            int64_t now   = esp_timer_get_time() - rt_t0;
            int64_t ahead = rt_emu_us - now;
            if (ahead > 1000) {                        // ahead of real time -> wait it out
                vTaskDelay(pdMS_TO_TICKS(ahead / 1000));
            } else {
                if (ahead < -200000) rt_emu_us = now;   // >200ms behind -> forgive debt
                if ((total & 31) == 0) vTaskDelay(1);   // yield so IDLE/WDT runs when not sleeping
            }
        }
#else
        if ((total & 7) == 0) vTaskDelay(1);   // feed watchdog / let IDLE run
#endif
    }
}

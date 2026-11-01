// ESP32 platform layer for the core — everything except the files (hostfile_sd.cpp).
// The screen is a PSRAM framebuffer (640x400 RGB565) that the LCD path scales.
// Sound / MIDI / serial / input / font are minimal stubs for the first boot.
// extern "C" symbols are resolved by name at link, so stub signatures only need
// to be self-consistent + match the core's call ABI (pointer handles == void*).

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"

extern "C" int ets_printf(const char *fmt, ...);
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <sys/time.h>
#include <time.h>
#include <cstddef>
#include <cstdlib>
#include <new>

// Large C++ allocations (new[]) go to PSRAM.
// Internal DRAM is tiny; route large allocations to PSRAM so they don't throw
// bad_alloc. heap_caps_free handles both PSRAM and internal pointers.
static inline void *app_cpp_alloc(std::size_t n) {
    void *p = nullptr;
    if (n >= 4096) p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(n);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p;
}
static void app_alloc_fail(std::size_t n) {
    ets_printf("[NEW FAIL] size=%u  free_psram=%u free_internal=%u free_8bit=%u\n",
               (unsigned)n, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}
// NOTE: returns NULL on failure instead of throwing/aborting; callers check the pointer.
void *operator new(std::size_t n)   { void *p = app_cpp_alloc(n); if (!p) app_alloc_fail(n); return p; }
void *operator new[](std::size_t n) { void *p = app_cpp_alloc(n); if (!p) app_alloc_fail(n); return p; }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept   { return app_cpp_alloc(n); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return app_cpp_alloc(n); }
void operator delete(void *p) noexcept   { heap_caps_free(p); }
void operator delete[](void *p) noexcept { heap_caps_free(p); }
void operator delete(void *p, std::size_t) noexcept   { heap_caps_free(p); }
void operator delete[](void *p, std::size_t) noexcept { heap_caps_free(p); }

extern "C" {
#include <compiler.h>
#include <host.h>
}

extern "C" {

// ================= the frame buffer (640x480 RGB565 in PSRAM) =======
static uint8_t *s_fb = nullptr;
#define PC98_W 640
#define PC98_H 480   /* >= any PC-98 mode we render (400/480), so sdraw never overflows */

// Allocate the framebuffer up-front while PSRAM is still plentiful (call before
// machine_init). Lazy alloc during drawscreen could fail once the heap is tight
// → NULL surface ptr → crash in sdraw. Returns true on success.
bool pc98_framebuffer_init(void) {
    if (!s_fb)
        // 128-byte aligned: this is the PPA's input picture, and the PPA
        // driver requires external buffers aligned to the L2 cache line both in
        // address and in size (640*480*2 is already a multiple of 128).
        s_fb = (uint8_t *)heap_caps_aligned_alloc(128, (size_t)PC98_W * PC98_H * 2,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_fb) memset(s_fb, 0, (size_t)PC98_W * PC98_H * 2);
    return s_fb != nullptr;
}

UINT16 *host_frame(int *width, int *height, int *stride) {
    if (!s_fb) pc98_framebuffer_init();
    *width = PC98_W;
    *height = PC98_H;
    *stride = PC98_W * 2;           // bytes per row
    return (UINT16 *)s_fb;
}
uint8_t *pc98_framebuffer(void) { return s_fb; }   // for the LCD blitter later

// ================= time =================
int host_date(HostDate *d) {
    time_t now = time(nullptr);
    struct tm *lt = localtime(&now);
    if (!lt) return 0;
    d->year = (UINT16)(lt->tm_year + 1900); d->month = (UINT8)(lt->tm_mon + 1); d->day = (UINT8)lt->tm_mday;
    d->hour = (UINT8)lt->tm_hour;           d->minute = (UINT8)lt->tm_min;      d->second = (UINT8)lt->tm_sec;
    return 1;
}


// ================= time / lamps =================
UINT32 host_ms(void) { return (UINT32)(esp_timer_get_time() / 1000); }
void lamp_fdd(REG8 drv);   // lcd_mipi.cpp: the drive lamps
void lamp_hdd(REG8 drv);
void host_fdd_access(UINT8 drv) { lamp_fdd(drv); }
void host_hdd_access(UINT8 drv) { lamp_hdd(drv); }

// ================= sound =================



// ================= input =================
// Real USB mouse only (HID boot protocol; state comes from usb_kbd.cpp's
// g_umouse_*). Buttons are ACTIVE-LOW on the uPD8255 port (bit SET=released,
// CLEAR=pressed); idle = LEFT(0x80)|RIGHT(0x20) = 0xA0. (The old stub returned
// 0 = both buttons stuck down -> auto-click.)
extern volatile int16_t g_umouse_dx, g_umouse_dy;  // usb_kbd.cpp: real USB mouse
extern volatile uint8_t g_umouse_btn;
extern volatile int     g_umouse_present;
UINT8 host_mouse(SINT16 *dx, SINT16 *dy) {
    if (!g_umouse_present) {           // no mouse attached: neutral state
        *dx = 0;
        *dy = 0;
        return 0xA0;                   // both buttons released (active low)
    }
    *dx = (SINT16)g_umouse_dx;
    *dy = (SINT16)g_umouse_dy;
    g_umouse_dx = 0;
    g_umouse_dy = 0;
    return g_umouse_btn;
}
// Joystick bits are ACTIVE-LOW (the sound board ANDs host_joypad() in; 0x00 would mean
// every direction+button held -> games ignore the keyboard). 0xFF = nothing pressed.
UINT8 host_joypad(void) { return 0xff; }

// ================= misc =================

} // extern "C"

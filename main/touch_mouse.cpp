// Touch panel as the PC-98 mouse.
//
// The PC-98 mouse is relative and the panel is absolute, so this behaves like a
// trackpad rather than a pointing stick: dragging one finger moves the cursor
// by the same amount the finger moved, and the emulated cursor's own position
// is never something this code has to know. A quick tap is a left click; a
// quick tap with two fingers is a right click.
//
// The panel is rotated 90 degrees relative to the PC-98 screen and mirrored on
// one axis, and which axis depends on the scale mode, so the finger position is
// converted through the very same lookup tables the blit uses (see
// lcd_panel_to_pc98 in lcd_mipi.cpp). Getting that wrong shows up as a mouse
// that moves sideways, and deriving it here a second time would be a second
// place to get it wrong.
//
// This file keeps the BSP headers to itself, like panel_tab5.cpp: bsp/*.h pulls
// in esp_vfs_fat.h, whose TCHAR clashed with the core's. Nothing here needs
// the core - the mouse goes through the same hid_mouse_* funnel as a USB or BLE
// mouse (hid_input.h), which is plain C.

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_touch.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"

#include "hid_input.h"
#include "osk.h"

extern "C" int ets_printf(const char *fmt, ...);

// lcd_mipi.cpp: panel pixel -> PC-98 pixel, through the active scale mapping.
// Returns false for a point outside the emulated screen (the letterbox).
extern "C" bool lcd_panel_to_pc98(int panel_x, int panel_y, int *out_x, int *out_y);
extern "C" bool lcd_osk_enabled(void);

// A finger moves the cursor this many PC-98 pixels per PC-98 pixel of travel, in quarters, chosen in the menu:
// 1.0, 1.5 or 2.0. The fractions left over are carried, so a slow drag still moves the cursor.
static const int s_gain4[3] = { 4, 6, 8 };
static volatile int s_speed = 1;
static int s_rem_x, s_rem_y;

extern "C" int touch_mouse_speed(void) { return s_speed; }
extern "C" void touch_mouse_set_speed(int s) { if (s >= 0 && s <= 2) s_speed = s; }

static int scaled(int d, int *rem) {
    const int v = d * s_gain4[s_speed] + *rem;
    const int out = v / 4;           // toward zero; the remainder keeps the sign
    *rem = v - out * 4;
    return out;
}

// A tap is a contact that ends quickly without going anywhere.
#define TAP_MAX_MS   250
#define TAP_MAX_MOVE 12   // PC-98 pixels
#define CLICK_HOLD_MS 40

#define POLL_MS 15        // ~66Hz; the emulated mouse is polled far slower

static esp_lcd_touch_handle_t s_tp = nullptr;

static void click(uint8_t mask) {
    hid_mouse_delta(0, 0, mask);
    vTaskDelay(pdMS_TO_TICKS(CLICK_HOLD_MS));
    hid_mouse_delta(0, 0, 0);
}

static void touch_task(void *arg) {
    (void)arg;

    bool down = false;          // a contact is in progress
    int  prev_x = 0, prev_y = 0;
    int  travel = 0;            // total movement of this contact, PC-98 pixels
    int  fingers_max = 0;       // most fingers seen during this contact
    TickType_t t_down = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        uint16_t tx[5] = {0}, ty[5] = {0};
        uint8_t cnt = 0;
        esp_lcd_touch_read_data(s_tp);
        bool pressed = esp_lcd_touch_get_coordinates(s_tp, tx, ty, nullptr, &cnt, 5);
        if (!pressed) {
            cnt = 0;
        }
        // The on-screen keyboard takes the points that land on it; the rest steer the mouse.
        const unsigned on_kbd = lcd_osk_enabled() ? osk_touch(tx, ty, cnt) : 0;
        if (on_kbd) {
            uint8_t k = 0;
            for (uint8_t i = 0; i < cnt; i++) {
                if (!(on_kbd & (1u << i))) {
                    tx[k] = tx[i];
                    ty[k] = ty[i];
                    k++;
                }
            }
            cnt = k;
            pressed = cnt > 0;
        }

        if (pressed && cnt > 0) {
            int px = 0, py = 0;
            if (!lcd_panel_to_pc98(tx[0], ty[0], &px, &py)) {
                continue;  // in the letterbox; ignore rather than jump
            }
            if (!down) {
                down = true;
                travel = 0;
                s_rem_x = s_rem_y = 0;
                fingers_max = cnt;
                t_down = xTaskGetTickCount();
            } else {
                if (cnt > fingers_max) {
                    fingers_max = cnt;
                }
                // A second finger landing moves the reported first contact
                // around; only one finger is allowed to steer.
                if (cnt == 1) {
                    const int dx = px - prev_x;
                    const int dy = py - prev_y;
                    travel += abs(dx) + abs(dy);
                    if (dx || dy) {
                        const int mx = scaled(dx, &s_rem_x), my = scaled(dy, &s_rem_y);
                        if (mx || my) hid_mouse_delta(mx, my, 0);
                    }
                }
            }
            prev_x = px;
            prev_y = py;
        } else if (down) {
            down = false;
            const uint32_t held = (xTaskGetTickCount() - t_down) * portTICK_PERIOD_MS;
            if (held <= TAP_MAX_MS && travel <= TAP_MAX_MOVE) {
                click(fingers_max >= 2 ? 0x02 : 0x01);  // two fingers = right
            }
        }
    }
}

extern "C" bool touch_mouse_init(void) {
    // The touch controller is on the display module and shares its rail, which
    // panel_tab5_init() has already brought up and settled.
    if (bsp_touch_new(nullptr, &s_tp) != ESP_OK || !s_tp) {
        ets_printf("touch: bsp_touch_new failed\n");
        return false;
    }
    hid_map_init();
    hid_mouse_attach();
    xTaskCreatePinnedToCore(touch_task, "touch", 3072, nullptr, 4, nullptr, 0);
    ets_printf("touch: panel mouse ready (tap=left, 2-finger tap=right)\n");
    return true;
}

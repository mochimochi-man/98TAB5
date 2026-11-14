// The on-screen keyboard (osk.cpp), shown under the picture in portrait or in landscape.
#pragma once

#include <stdint.h>

#define OSK_ROWH  67    // a row of keys
#define OSK_ROWS  11    // rows of keys, counting the tiers of the block at the bottom
#define OSK_TOP   (1280 - OSK_ROWS * OSK_ROWH)   // the keyboard, at the bottom of the panel
#define OSK_PIC_H 450   // the picture: 640x400 at 9/8, across the panel,
#define OSK_PIC_Y ((OSK_TOP - OSK_PIC_H) / 2)   // centred in the space above the keyboard

// Landscape (1280x720 as the case is held): the picture 640x400 at 19/16 (the PPA's step nearest 1.2), the
// function keys down its right and STOP ... SHIFT down its left, and under it five rows: the main keys, then
// RETURN, the cursor keys and keypad.
#define OSKL_ROWH  47
#define OSKL_ROWS  5
#define OSKL_TOP   (720 - OSKL_ROWS * OSKL_ROWH)
#define OSKL_NUM   19
#define OSKL_DEN   16
#define OSKL_PIC_W (640 * OSKL_NUM / OSKL_DEN)   // 760
#define OSKL_PIC_H (400 * OSKL_NUM / OSKL_DEN)   // 475
#define OSKL_PIC_X ((1280 - OSKL_PIC_W) / 2)
#define OSKL_PIC_Y ((OSKL_TOP - OSKL_PIC_H) / 2)

#define OSK_OFF       0
#define OSK_PORTRAIT  1
#define OSK_LANDSCAPE 2
#define OSK_GAME      3   // landscape, with only the keys games use (osk.cpp, layout_game)
#define OSK_MODES     4

#ifdef __cplusplus
extern "C" {
#endif

// Paint the whole keyboard into the panel's frame buffer (720 wide, native portrait), laid out for `mode`.
void osk_paint(uint16_t *fb, int mode);

// The touch points, in panel coordinates. Returns a bit per point that fell on the keyboard.
unsigned osk_touch(const uint16_t *tx, const uint16_t *ty, int n);

// Landscape (either keyboard): the gap under INS / DEL that holds the drive lamps, in the keyboard's landscape
// coordinates. False in portrait, or before the keyboard is laid out.
bool osk_lamp_slot(int *x0, int *y0, int *x1, int *y1);

#ifdef __cplusplus
}
#endif

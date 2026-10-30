#pragma once
// Drawing helpers for launcher apps (implemented in main.c). The panel is
// 135x240; the top BAR_H pixels belong to the status bar (battery, USB, BLE).
#include <stdint.h>

#define LCD_W          135
#define LCD_H          240
#define BAR_H          20

#define COL_BLACK      0x0000
#define COL_WHITE      0xFFFF
#define COL_RED        0xF800
#define COL_GREEN      0x07E0
#define COL_BLUE       0x001F
#define COL_YELLOW     0xFFE0
#define COL_CYAN       0x07FF
#define COL_MAGENTA    0xF81F
#define COL_ORANGE     0xFD20
#define COL_DIM_RED    0x4000
#define COL_DIM_CYAN   0x0210
#define COL_DARK_GRAY  0x0841
#define COL_LIGHT_GRAY 0xBDF7

// Hold the lock while drawing from an app's tick; the status bar and the BLE
// pairing screen draw from other tasks.
void ui_lock(void);
void ui_unlock(void);
void ui_fill_rect(int x0, int y0, int x1, int y1, uint16_t rgb565);   // x1, y1 exclusive
void ui_line(int x0, int y0, int x1, int y1, int thickness, uint16_t rgb565);
void ui_circle(int cx, int cy, int r, uint16_t rgb565);
void ui_clear(uint16_t rgb565);
int  ui_text_width(const char *s, int scale);
// 5x7 font scaled by `scale`: A-Z subset (see k_font_chars in main.c), 0-9, % and -.
void ui_text(int x, int y, const char *s, int scale, uint16_t rgb565);

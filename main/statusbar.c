// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

// ---- Status bar (top 20 px): battery, charging, Bluetooth link ---------------
int           g_batt_pct    = -1;
volatile TickType_t g_batt_good_tick = 0;   // when a trustworthy reading last arrived (0 = none since boot)       // -1 = unknown
bool          g_batt_ext    = false;    // running from USB power
static bool          g_bar_ble     = false;
static int           g_bar_shown_pct = -2;
static bool          g_bar_shown_ext = false;
static bool          g_bar_shown_ble = false;

int g_fault_reason = 0;                 // esp_reset_reason() if the last reset was abnormal
uint32_t g_fault_crumb = 0;
char g_fault_who[20] = "";
volatile int g_batt_mv = 0;
volatile int g_batt_dbg = 0;            // 0 = last battery read fine; else error code or 10000+mV

// Paints the bar. The caller must hold g_pres_lcd_mu (or be the only drawer).
static void statusbar_paint(void)
{
    g_bar_dirty = false;
    g_bar_shown_pct = g_batt_pct; g_bar_shown_ext = g_batt_ext; g_bar_shown_ble = g_bar_ble;
    lcd_fill_rect(0, 0, LCD_W, BAR_H, COL_BLACK);
    uint16_t col = (g_batt_pct >= 0 && g_batt_pct <= BATT_LOW_PCT) ? COL_RED
                 : (g_batt_ext ? COL_GREEN : COL_LIGHT_GRAY);
    // battery icon: outline, nub, fill proportional to charge
    lcd_fill_rect(4, 4, 28, 15, col);
    lcd_fill_rect(6, 6, 26, 13, COL_BLACK);
    lcd_fill_rect(28, 7, 30, 12, col);
    if (g_batt_pct >= 0) {
        int w = (20 * g_batt_pct) / 100;
        if (w < 1) w = 1;
        lcd_fill_rect(6, 6, 6 + w, 13, col);
        char s[8];
        // "~" in front: the chip has not answered sensibly for a while, so this is the last good value
        snprintf(s, sizeof(s), battery_is_estimate() ? "~%d%%" : "%d%%", g_batt_pct);
        lcd_draw_text(36, 3, s, 2, col);
        int tw = lcd_text_width(s, 2);
        if (g_batt_ext) {                       // lightning bolt
            int bx = 36 + tw + 6;
            lcd_draw_thick_line(bx + 4, 3, bx, 10, 1, COL_YELLOW);
            lcd_draw_thick_line(bx, 10, bx + 5, 10, 1, COL_YELLOW);
            lcd_draw_thick_line(bx + 5, 10, bx + 1, 17, 1, COL_YELLOW);
        }
    } else {
        zh_text(36, 4, "電量未知", 0x7BEF, COL_BLACK, 1);   // no trustworthy reading yet (typical on battery power)
    }
    if (g_fault_reason) {                       // the stick reset itself: open DIAG
        lcd_draw_text(94, 6, "!", 2, COL_RED);
    }
    // Bluetooth link dot
    int r = 5, cx = LCD_W - 12, cy = BAR_H / 2;
    for (int dy = -r; dy <= r; dy++) {
        int hw = 0;
        while ((hw + 1) * (hw + 1) + dy * dy <= r * r) hw++;
        lcd_fill_rect(cx - hw, cy + dy, cx + hw + 1, cy + dy + 1,
                      g_bar_ble ? COL_CYAN : COL_DARK_GRAY);
    }
}

// Cheap hook for draw routines that already hold the mutex.
void statusbar_paint_if_dirty(void)
{
    if (g_bar_dirty) statusbar_paint();
}

// Called from the display task: refreshes the readings and repaints on change.
void statusbar_tick(void)
{
    static TickType_t last_sample = 0;
    TickType_t now = xTaskGetTickCount();
    if (last_sample == 0 || (now - last_sample) >= pdMS_TO_TICKS(5000)) {
        last_sample = now ? now : 1;
        battery_sample();
    }
    g_bar_ble = ble_presenter_connected();
    if (g_passkey_hold_until != 0) return;      // keep the pairing code unobstructed
    // Also repaint every few seconds: anything that draws over the bar by accident heals itself.
    static TickType_t last_paint = 0;
    bool stale = (now - last_paint) >= pdMS_TO_TICKS(3000);
    if (stale || g_bar_dirty || g_batt_pct != g_bar_shown_pct || g_batt_ext != g_bar_shown_ext ||
        g_bar_ble != g_bar_shown_ble) {
        last_paint = now;
        xSemaphoreTake(g_pres_lcd_mu, portMAX_DELAY);
        statusbar_paint();
        xSemaphoreGive(g_pres_lcd_mu);
    }
}

// 28x28 menu icons built from rectangles and lines, drawn in colour `c`.
void lcd_fill_circle(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int hw = 0;
        while ((hw + 1) * (hw + 1) + dy * dy <= r * r) hw++;
        lcd_fill_rect(cx - hw, cy + dy, cx + hw + 1, cy + dy + 1, c);
    }
}

void lcd_icon_mic(int x, int y, uint16_t c)
{
    lcd_fill_rect(x + 10, y + 2, x + 18, y + 16, c);          // capsule
    lcd_fill_rect(x + 11, y + 1, x + 17, y + 2, c);
    lcd_fill_rect(x + 11, y + 16, x + 17, y + 17, c);
    lcd_draw_thick_line(x + 6, y + 11, x + 6, y + 17, 2, c);    // cradle
    lcd_draw_thick_line(x + 6, y + 17, x + 10, y + 21, 2, c);
    lcd_draw_thick_line(x + 10, y + 21, x + 18, y + 21, 2, c);
    lcd_draw_thick_line(x + 18, y + 21, x + 22, y + 17, 2, c);
    lcd_draw_thick_line(x + 22, y + 17, x + 22, y + 11, 2, c);
    lcd_draw_thick_line(x + 14, y + 21, x + 14, y + 26, 2, c);  // stem + base
    lcd_draw_thick_line(x + 9, y + 26, x + 19, y + 26, 2, c);
}

void lcd_icon_bluetooth(int x, int y, uint16_t c)
{
    static const int8_t pts[][2] = {{6, 19}, {22, 9}, {14, 2}, {14, 26}, {22, 19}, {6, 9}};
    for (int i = 0; i + 1 < 6; i++) {
        lcd_draw_thick_line(x + pts[i][0], y + pts[i][1],
                            x + pts[i + 1][0], y + pts[i + 1][1], 2, c);
    }
}

void lcd_icon_record(int x, int y, uint16_t c, uint16_t bg)
{
    lcd_fill_circle(x + 14, y + 14, 13, c);                    // ring
    lcd_fill_circle(x + 14, y + 14, 10, bg);
    lcd_fill_circle(x + 14, y + 14, 6, c);                     // dot
}

void lcd_icon_slide(int x, int y, uint16_t c)
{
    lcd_draw_thick_line(x + 2, y + 14, x + 22, y + 14, 4, c);   // arrow
    lcd_draw_thick_line(x + 14, y + 5, x + 24, y + 14, 3, c);
    lcd_draw_thick_line(x + 14, y + 23, x + 24, y + 14, 3, c);
}


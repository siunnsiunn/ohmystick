// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

// ---- DIAG: what the stick knows about itself ------------------------------------
void icon_diag(int x, int y, uint16_t fg, uint16_t bg)      // warning triangle with "!"
{
    (void)bg;
    lcd_draw_thick_line(x + 14, y + 3, x + 2, y + 25, 2, fg);
    lcd_draw_thick_line(x + 14, y + 3, x + 26, y + 25, 2, fg);
    lcd_draw_thick_line(x + 2, y + 25, x + 26, y + 25, 2, fg);
    lcd_fill_rect(x + 13, y + 10, x + 16, y + 18, fg);
    lcd_fill_rect(x + 13, y + 20, x + 16, y + 23, fg);
}

#define DIAG_LINES 14
static char g_diag_shown[DIAG_LINES][48];

// Redraws one 9 px line only if its text changed.
static void diag_line(int i, const char *s, uint16_t col)
{
    if (strcmp(g_diag_shown[i], s) == 0) return;
    snprintf(g_diag_shown[i], sizeof(g_diag_shown[i]), "%s", s);
    int y = BAR_H + 6 + i * 11;
    xSemaphoreTake(g_pres_lcd_mu, portMAX_DELAY);
    lcd_fill_rect(0, y, LCD_W, y + 9, COL_BLACK);
    lcd_draw_text(4, y, s, 1, col);
    xSemaphoreGive(g_pres_lcd_mu);
}

static uint32_t diag_tick_details(bool first)
{
    static const char *ui_name[] = { "NO LINK", "READY", "REC", "BUSY", "DONE", "ERR", "REC OFF", "SYNC", "FULL" };
    static bool was_held = false;
    if (g_passkey_hold_until != 0) { was_held = true; return 200; }   // the pairing code owns the screen
    if (was_held) { was_held = false; lcd_clear_locked(); first = true; }
    if (first) memset(g_diag_shown, 0, sizeof(g_diag_shown));
    char s[48], d[24], s2[32];
    int ui = (int)btmic_ui_state();
    snprintf(s, sizeof(s), "LINK %s", ui >= 0 && ui < 9 ? ui_name[ui] : "?");
    diag_line(0, s, COL_CYAN);
    snprintf(s, sizeof(s), "PENDING %d", btmic_pending() % 100);
    diag_line(1, s, COL_YELLOW);
    btmic_debug(d, sizeof(d));
    snprintf(s, sizeof(s), "UPLOAD %s", d);
    diag_line(2, s, COL_LIGHT_GRAY);
    if (g_batt_pct >= 0) snprintf(s, sizeof(s), "BATT %d%% %dMV %s", g_batt_pct, g_batt_mv, g_batt_ext ? "USB" : "BAT");
    else snprintf(s, sizeof(s), "BATT UNKNOWN");
    diag_line(3, s, COL_LIGHT_GRAY);
    if (g_batt_dbg >= 10000) snprintf(s, sizeof(s), "READ ODD V%d KEEP OLD", g_batt_dbg - 10000);
    else if (g_batt_dbg) snprintf(s, sizeof(s), "READ FAIL E%d", g_batt_dbg % 1000);
    else snprintf(s, sizeof(s), "READ OK");
    diag_line(4, s, g_batt_dbg >= 10000 ? COL_YELLOW : (g_batt_dbg ? COL_RED : COL_GREEN));
    snprintf(s, sizeof(s), "HEAP %uK MIN %uK", (unsigned)(esp_get_free_heap_size() / 1024),
             (unsigned)(esp_get_minimum_free_heap_size() / 1024));
    diag_line(5, s, COL_LIGHT_GRAY);
    snprintf(s, sizeof(s), "UP %u S", (unsigned)(xTaskGetTickCount() / configTICK_RATE_HZ));
    diag_line(6, s, COL_LIGHT_GRAY);
    snprintf(s, sizeof(s), "LCD WAIT %u MAX %u MS", (unsigned)g_lcd_contended, (unsigned)g_lcd_wait_max_ms);
    diag_line(7, s, COL_LIGHT_GRAY);
    snprintf(s, sizeof(s), "QUIET MAX D%d B%d R%d S", g_hb_stuck_max[0], g_hb_stuck_max[1], g_hb_stuck_max[2]);
    diag_line(10, s, g_hb_stuck_max[0] >= 4 || g_hb_stuck_max[1] >= 4 || g_hb_stuck_max[2] >= 5 ? COL_YELLOW : COL_LIGHT_GRAY);
    vox_pair_info(s, sizeof(s), s2, sizeof(s2));
    diag_line(11, s, strstr(s, "OPEN") ? COL_YELLOW : COL_LIGHT_GRAY);
    diag_line(12, s2, COL_LIGHT_GRAY);
    if (g_fault_reason) {
        snprintf(s, sizeof(s), "LAST RESET R%d", g_fault_reason % 100);
        diag_line(8, s, COL_RED);
        snprintf(s, sizeof(s), "C%05u %s", (unsigned)(g_fault_crumb % 100000), g_fault_who);
        diag_line(9, s, COL_RED);
        diag_line(13, "FRONT BACK  HOLD PAIR", COL_LIGHT_GRAY);
    } else {
        diag_line(8, "LAST RESET NONE", COL_GREEN);
        diag_line(13, "FRONT BACK  HOLD PAIR", COL_LIGHT_GRAY);
    }
    return 1000;
}

// ---- plain-language summary (the default DIAG page) ---------------------------
// Five rows, each a small label over a big value, and one hint that says what to do.
static bool g_diag_details = false;
static char g_diag_sum[10][64];

// One row: a grey label and a coloured value, repainted only when the text changes.
static void diag_sum_row(int i, int y, const char *label, const char *value, uint16_t col, int scale)
{
    char key[64];
    snprintf(key, sizeof(key), "%s|%s|%u", label, value, (unsigned)col);
    if (strcmp(g_diag_sum[i], key) == 0) return;
    snprintf(g_diag_sum[i], sizeof(g_diag_sum[i]), "%s", key);
    xSemaphoreTake(g_pres_lcd_mu, portMAX_DELAY);
    lcd_fill_rect(0, y, LCD_W, y + 12 * scale + 2, COL_BLACK);
    int x = 4;
    if (label[0]) x = zh_text(x, y, label, COL_LIGHT_GRAY, COL_BLACK, scale) + 6;
    zh_text(x, y, value, col, COL_BLACK, scale);
    xSemaphoreGive(g_pres_lcd_mu);
}

static uint32_t diag_tick_summary(bool first)
{
    if (first) memset(g_diag_sum, 0, sizeof(g_diag_sum));
    char v[40];
    bool linked = btmic_ui_state() != BTMIC_UI_NOLINK;
    int pending = btmic_pending();
    int left = 0, pair = vox_pair_status(&left);
    bool batt_known = g_batt_pct >= 0;
    bool batt_low = batt_known && g_batt_pct <= BATT_LOW_PCT;

    const char *verdict; uint16_t vcol;
    if (g_fault_reason || batt_low) { verdict = "有問題"; vcol = COL_RED; }
    else if (!linked || pending > 0 || pair == 2 || !batt_known) { verdict = "請留意"; vcol = COL_YELLOW; }
    else { verdict = "狀態良好"; vcol = COL_GREEN; }
    diag_sum_row(0, BAR_H + 6, "", verdict, vcol, 2);

    int y = BAR_H + 44;
    diag_sum_row(1, y, "電腦連線", linked ? "已連上" : "沒連上", linked ? COL_GREEN : COL_YELLOW, 1);
    y += 24;
    if (pending > 0) snprintf(v, sizeof(v), "%d 筆", pending % 100); else snprintf(v, sizeof(v), "無");
    diag_sum_row(2, y, "待傳筆記", v, pending > 0 ? COL_YELLOW : COL_GREEN, 1);
    y += 24;
    if (batt_known) snprintf(v, sizeof(v), "%d%% %s", g_batt_pct, g_batt_ext ? "充電中" : "電池");
    else snprintf(v, sizeof(v), "量測中");
    diag_sum_row(3, y, "電量", v, !batt_known ? COL_YELLOW : (batt_low ? COL_RED : COL_GREEN), 1);
    y += 24;
    if (pair == 1) snprintf(v, sizeof(v), "開放 %d 秒", left);
    else if (pair == 2) snprintf(v, sizeof(v), "尚未配對");
    else snprintf(v, sizeof(v), "已鎖定");
    diag_sum_row(4, y, "新裝置", v, pair == 0 ? COL_GREEN : COL_YELLOW, 1);
    y += 24;
    diag_sum_row(5, y, "本機", g_fault_reason ? "曾自動重開" : "穩定", g_fault_reason ? COL_RED : COL_GREEN, 1);

    const char *h1, *h2 = "";
    char code[40];
    if (g_fault_reason) {                       // the reason code, so one photo of this page is enough
        if (g_fault_reason == 50) snprintf(code, sizeof(code), "R50 C%05u %s", (unsigned)(g_fault_crumb % 100000), g_fault_who);
        else                      snprintf(code, sizeof(code), "R%d %s", g_fault_reason % 100, g_fault_who);
        h1 = "重開機過 前鍵確認"; h2 = code;
    }
    else if (pair == 2)      { h1 = "請到 Mac 藍牙設定"; h2 = "完成配對"; }
    else if (!linked)        { h1 = "請開啟 Mac 的 btmic"; h2 = "或喚醒 Mac"; }
    else if (pending > 0)    { h1 = "請進入 NOTE 畫面"; h2 = "傳送已存的筆記"; }
    else if (batt_low)       { h1 = "電量偏低 請充電"; }
    else                     { h1 = "目前不用處理"; }
    diag_sum_row(6, y + 32, "", h1, COL_WHITE, 1);
    diag_sum_row(7, y + 48, "", h2, COL_WHITE, 1);
    diag_sum_row(8, LCD_H - 16, "", "前鍵 詳細  長按 配對", COL_LIGHT_GRAY, 1);
    return 1000;
}

uint32_t diag_tick(bool first)
{
    static bool was_details = false, was_held = false;
    if (g_passkey_hold_until != 0) { was_held = true; return 200; }  // the pairing code owns the screen
    if (was_held) { was_held = false; lcd_clear_locked(); first = true; }
    if (first) was_details = g_diag_details = false;
    if (g_diag_details != was_details) {                              // page switched: start from a clean screen
        was_details = g_diag_details;
        lcd_clear_locked();
        first = true;
    }
    return g_diag_details ? diag_tick_details(first) : diag_tick_summary(first);
}

void diag_front_up(uint32_t held_ms, bool is_long)   // short: summary <-> details; held 2 s: let a new computer pair for 60 s
{
    (void)is_long;
    if (held_ms >= 2000) { vox_pair_open(); return; }
    // The reset notice stays until it has been looked at on the details page and that page is left.
    if (g_diag_details) { g_fault_reason = 0; g_batt_dbg = 0; g_bar_dirty = true; }
    g_diag_details = !g_diag_details;
}

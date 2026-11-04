// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

// Launcher: a two-column grid, every app on one page (up to eight), the selected
// app's description in a strip at the bottom.
// prev >= 0: only the previously selected cell, the new one and the description strip
// change, so repaint just those (switching is then instant). prev < 0: whole screen.
void lcd_draw_menu(int sel, int prev)
{
    if (!g_lcd) return;
    int n = launcher_count();
    const int cols = 2, gap = 3, margin = 3;
    int rows = (n + cols - 1) / cols;
    const int top = BAR_H + 3;
    const int strip = 20;                                   // bottom strip for the description
    int cell_w = (LCD_W - 2 * margin - gap * (cols - 1)) / cols;
    int cell_h = (LCD_H - top - strip - gap * (rows - 1)) / rows;
    bool partial = prev >= 0 && prev < n && prev != sel;
    xSemaphoreTake(g_pres_lcd_mu, portMAX_DELAY);
    if (partial) lcd_fill_rect(0, LCD_H - strip, LCD_W, LCD_H, COL_BLACK);
    else         lcd_clear(COL_BLACK);
    for (int i = 0; i < n; i++) {
        if (partial && i != sel && i != prev) continue;
        const app_t *a = launcher_app(i);
        int x = margin + (i % cols) * (cell_w + gap);
        int y = top + (i / cols) * (cell_h + gap);
        bool on = (i == sel);
        uint16_t col = a->color;
        if (partial) lcd_fill_rect(x, y, x + cell_w, y + cell_h, COL_BLACK);
        if (on) {
            lcd_fill_rect(x, y, x + cell_w, y + cell_h, col);
        } else {
            lcd_fill_rect(x, y, x + cell_w, y + 2, col);
            lcd_fill_rect(x, y + cell_h - 2, x + cell_w, y + cell_h, col);
            lcd_fill_rect(x, y, x + 2, y + cell_h, col);
            lcd_fill_rect(x + cell_w - 2, y, x + cell_w, y + cell_h, col);
        }
        uint16_t fg = on ? COL_BLACK : col;
        uint16_t bg = on ? col : COL_BLACK;
        int icon_y = y + (cell_h - 28 - 16) / 2;
        if (a->icon) a->icon(x + (cell_w - 28) / 2, icon_y, fg, bg);
        const char *nm = a->name_zh ? a->name_zh : a->name;
        int nx = x + (cell_w - zh_text_width(nm, 1)) / 2;
        zh_text(nx < x ? x : nx, icon_y + 28 + 4, nm, fg, bg, 1);
    }
    const app_t *s = launcher_app(sel);
    if (s && s->desc_zh) zh_center(LCD_H - 16, s->desc_zh, s->color, 1);
    statusbar_paint_if_dirty();
    xSemaphoreGive(g_pres_lcd_mu);
}

// Words for the BLE bring-up state, shared by the SLIDE screen and the voice screens.
static void ble_status_words(const char **word, uint16_t *col, int *scale, char *code, size_t n)
{
    int st = ble_presenter_state();
    code[0] = 0;
    if (ble_presenter_connected())       { *word = "已連線"; *col = COL_CYAN; *scale = 3; }   // a stale error code must not hide a working link
    else if (st == 98)                 { *word = "配對失敗"; *col = COL_RED; *scale = 2; snprintf(code, n, "到狀態頁長按配對"); }
    else if (st >= 90)                 { *word = "藍牙錯誤"; *col = COL_RED; *scale = 2; snprintf(code, n, "代碼 %d", st); }
    else if (st == 8)                  { *word = "輸入配對碼"; *col = COL_YELLOW; *scale = 1; }
    else                               { *word = "等待連線"; *col = COL_ORANGE; *scale = 2; }
}

// Presenter/mic screens only repaint the middle band, so wipe the whole panel
// when a mode is entered (otherwise the menu colours stay at the top/bottom).
void lcd_clear_locked(void)
{
    if (!g_lcd) return;
    xSemaphoreTake(g_pres_lcd_mu, portMAX_DELAY);
    lcd_clear(COL_BLACK);
    statusbar_paint_if_dirty();
    xSemaphoreGive(g_pres_lcd_mu);
}

void ui_lock(void)   { xSemaphoreTake(g_pres_lcd_mu, portMAX_DELAY); }
void ui_unlock(void) { xSemaphoreGive(g_pres_lcd_mu); }
void ui_fill_rect(int x0, int y0, int x1, int y1, uint16_t c) { lcd_fill_rect(x0, y0, x1, y1, c); }
void ui_line(int x0, int y0, int x1, int y1, int th, uint16_t c) { lcd_draw_thick_line(x0, y0, x1, y1, th, c); }
void ui_circle(int cx, int cy, int r, uint16_t c) { lcd_fill_circle(cx, cy, r, c); }
void ui_clear(uint16_t c) { lcd_clear(c); }
int  ui_text_width(const char *s, int scale) { return lcd_text_width(s, scale); }
void ui_text(int x, int y, const char *s, int scale, uint16_t c) { lcd_draw_text(x, y, s, scale, c); }

// Voice screens (WHISPER, NOTE): one big word for the state (see btmic_ui_t), plus how many
// recordings are waiting on the stick.
void lcd_draw_btrec(int ui, bool note)
{
    // state -> word, colour, and a one-line hint (live dictation / stored notes)
    static const struct { const char *word; uint16_t col; const char *live; const char *stored; } k[] = {
        { "未連線", COL_ORANGE, "請開啟 Mac 的 btmic", "按住前鍵錄音" },
        { "可錄音", COL_CYAN,   "按住前鍵說話",         "按住前鍵錄音" },
        { "錄音中", COL_RED,    "放開後轉成文字",       "放開即存檔" },
        { "轉寫中", COL_YELLOW, "請稍候",               "電腦轉寫中" },
        { "完成",   COL_GREEN,  "",                     "已存入收件夾" },
        { "失敗",   COL_RED,    "沒聽到內容 請再說一次", "請稍後再試" },
        { "錄音中", COL_RED,    "",                     "放開即存檔" },
        { "傳送中", COL_CYAN,   "",                     "正在傳到電腦" },
        { "已滿",   COL_RED,    "",                     "請連線傳送筆記" },
    };
    if (!g_lcd) return;
    if (ui < 0 || ui >= (int)(sizeof(k) / sizeof(k[0]))) ui = 0;
    int pending = btmic_pending();
    const char *word = k[ui].word;
    uint16_t wcol = k[ui].col;
    if (note && (ui == BTMIC_UI_NOLINK || ui == BTMIC_UI_READY)) {   // notes record with or without the Mac
        word = "可錄音";
        wcol = ui == BTMIC_UI_READY ? COL_CYAN : COL_ORANGE;
    }
    const char *hint = note ? k[ui].stored : k[ui].live;
    xSemaphoreTake(g_pres_lcd_mu, portMAX_DELAY);
    lcd_clear(COL_BLACK);
    zh_center(34, note ? "語音筆記" : "即時聽寫", COL_LIGHT_GRAY, 2);
    zh_center((LCD_H - 36) / 2, word, wcol, 3);
    if (note && pending > 0 && ui != BTMIC_UI_REC_OFF && ui != BTMIC_UI_REC) {
        char s[24];
        snprintf(s, sizeof(s), "待傳 %d 筆", pending % 100);
        zh_center(160, s, COL_YELLOW, 2);
    }
    zh_center(210, hint, COL_LIGHT_GRAY, 1);
    statusbar_paint_if_dirty();
    xSemaphoreGive(g_pres_lcd_mu);
}

void lcd_draw_wifimic(int ui)
{
    static const struct { const char *word; uint16_t col; const char *hint; } k[] = {
        [WIFIMIC_UI_OFF]     = { "啟動中", COL_ORANGE, "" },
        [WIFIMIC_UI_SETUP]   = { "設定",   COL_YELLOW, "手機連上後開 192.168.4.1" },
        [WIFIMIC_UI_SAVED]   = { "已儲存", COL_GREEN,  "重新開機中" },
        [WIFIMIC_UI_WIFI]    = { "連線中", COL_ORANGE, "Wi-Fi" },
        [WIFIMIC_UI_TAILNET] = { "連線中", COL_ORANGE, "Tailscale" },
        [WIFIMIC_UI_PEER]    = { "未連線", COL_ORANGE, "請開啟電腦的 winvoice" },
        [WIFIMIC_UI_READY]   = { "可錄音", COL_CYAN,   "按住前鍵說話" },
        [WIFIMIC_UI_REC]     = { "錄音中", COL_RED,    "放開後轉成文字" },
        [WIFIMIC_UI_BUSY]    = { "轉寫中", COL_YELLOW, "請稍候" },
        [WIFIMIC_UI_DONE]    = { "完成",   COL_GREEN,  "" },
        [WIFIMIC_UI_ERR]     = { "失敗",   COL_RED,    "沒聽到內容 請再說一次" },
    };
    if (!g_lcd) return;
    if (ui < 0 || ui >= (int)(sizeof(k) / sizeof(k[0]))) ui = 0;
    char a[40], b[40];
    wifimic_detail(a, sizeof(a), b, sizeof(b));
    xSemaphoreTake(g_pres_lcd_mu, portMAX_DELAY);
    lcd_clear(COL_BLACK);
    zh_center(34, "Wi-Fi聽寫", COL_LIGHT_GRAY, 2);
    zh_center(ui == WIFIMIC_UI_SETUP ? 66 : (LCD_H - 36) / 2, k[ui].word, k[ui].col, 3);
    if (ui == WIFIMIC_UI_SETUP) {                   // hotspot name and password, large enough to type
        zh_center(116, a, COL_WHITE, 1);
        zh_center(136, b, COL_WHITE, 2);
        zh_center(170, "手機連這個 Wi-Fi", COL_LIGHT_GRAY, 1);
    } else {
        zh_center(160, a, COL_LIGHT_GRAY, 1);
        zh_center(176, b, COL_LIGHT_GRAY, 1);
    }
    zh_center(196, k[ui].hint, COL_LIGHT_GRAY, 1);
    if (ui == WIFIMIC_UI_WIFI || ui == WIFIMIC_UI_TAILNET || ui == WIFIMIC_UI_PEER)
        zh_center(214, "長按前鍵3秒 重新設定", COL_LIGHT_GRAY, 1);
    statusbar_paint_if_dirty();
    xSemaphoreGive(g_pres_lcd_mu);
}

void lcd_draw_presenter(void)
{
    if (!g_lcd) return;
    const char *word; uint16_t col; int scale; char code[16];
    ble_status_words(&word, &col, &scale, code, sizeof(code));
    if (scale > 2) scale = 2;                       // the arrow takes the middle of the screen
    xSemaphoreTake(g_pres_lcd_mu, portMAX_DELAY);
    lcd_clear(COL_BLACK);
    zh_center(34, "簡報翻頁", COL_LIGHT_GRAY, 2);
    const int cx = LCD_W / 2;
    lcd_draw_thick_line(cx - 36, 99, cx + 30, 99, 8, col);       // the arrow
    lcd_draw_thick_line(cx + 34, 99, cx + 6, 70, 8, col);
    lcd_draw_thick_line(cx + 34, 99, cx + 6, 128, 8, col);
    zh_center(146, word, col, scale);
    if (code[0]) zh_center(176, code, COL_RED, 1);
    zh_center(192, "前鍵 下一頁", COL_LIGHT_GRAY, 1);
    zh_center(208, "側鍵 上一頁", COL_LIGHT_GRAY, 1);
    statusbar_paint_if_dirty();
    xSemaphoreGive(g_pres_lcd_mu);
}

// 7-segment digits for the BLE pairing passkey (random per pairing).
static void lcd_draw_digit(int x, int y, int d, uint16_t col)
{
    static const uint8_t seg[10] = {0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};
    const int w = 16, h = 30, k = 3;
    uint8_t s = seg[d];
    if (s & 0x01) lcd_fill_rect(x, y, x + w, y + k, col);
    if (s & 0x02) lcd_fill_rect(x + w - k, y, x + w, y + h / 2, col);
    if (s & 0x04) lcd_fill_rect(x + w - k, y + h / 2, x + w, y + h, col);
    if (s & 0x08) lcd_fill_rect(x, y + h - k, x + w, y + h, col);
    if (s & 0x10) lcd_fill_rect(x, y + h / 2, x + k, y + h, col);
    if (s & 0x20) lcd_fill_rect(x, y, x + k, y + h / 2, col);
    if (s & 0x40) lcd_fill_rect(x, y + h / 2 - 1, x + w, y + h / 2 + 2, col);
}

void ble_presenter_show_passkey(uint32_t passkey)
{
    if (!g_lcd) return;
    // Hold flag first so the LCD task stops repainting before we draw.
    g_passkey_hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(60000);
    xSemaphoreTake(g_pres_lcd_mu, portMAX_DELAY);
    lcd_fill_rect(0, 34, LCD_W, 184, COL_BLACK);
    zh_center(50, "藍牙配對", COL_LIGHT_GRAY, 2);
    zh_center(140, "請在電腦輸入配對碼", COL_LIGHT_GRAY, 1);
    for (int i = 5; i >= 0; i--) {
        lcd_draw_digit(4 + i * 21, 95, passkey % 10, COL_YELLOW);
        passkey /= 10;
    }
    xSemaphoreGive(g_pres_lcd_mu);
}


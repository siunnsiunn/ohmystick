// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

// =========================================================================
// TIMER — an hourglass. Side button picks the length, front button starts / pauses,
// holding the front button leaves (launcher.h). Sand runs from the top bulb into the
// bottom one; there is no sound (the speaker amplifier is off), so the end is shown by
// the screen: "時間到" blinking until a button is pressed.
// =========================================================================
#define HG_CX        (LCD_W / 2)
#define HG_TOP       34                 // top cap
#define HG_NECK      102
#define HG_BOT       170                // bottom cap
#define HG_HALF      64                 // rows of each bulb (between the caps and the neck)
#define HG_WALL      3
#define HG_SAND      COL_YELLOW
#define HG_GLASS     COL_LIGHT_GRAY

typedef enum { T_IDLE, T_RUN, T_PAUSE, T_DONE } tstate_t;
static const uint16_t k_minutes[] = { 1, 3, 5, 10, 15, 30 };
static volatile tstate_t s_state = T_IDLE;
static volatile int      s_preset = 2;                   // 5 min
static volatile uint32_t s_left_ms = 5 * 60000;          // while idle / paused: what remains
static TickType_t        s_end;                          // while running: when it ends
static volatile bool     s_dirty = true;                 // a button changed the state: redraw the hints

static uint32_t preset_ms(void) { return (uint32_t)k_minutes[s_preset] * 60000; }

static uint32_t remaining_ms(void)
{
    if (s_state != T_RUN) return s_left_ms;
    int32_t d = (int32_t)(s_end - xTaskGetTickCount());
    return d > 0 ? pdTICKS_TO_MS(d) : 0;
}

// ---- hourglass geometry ---------------------------------------------------
static int inner_half(int y)                              // half width of the glass interior at row y
{
    int d = y > HG_NECK ? y - HG_NECK : HG_NECK - y;
    return 2 + d * 29 / HG_HALF;
}

// 0 empty, 1 sand, 2 falling stream
static int row_state(int y, int n_top, int n_bot, bool flowing)
{
    int pile_top = HG_BOT - 4 - n_bot;                    // first sand row of the bottom pile
    if (y < HG_NECK) return y >= HG_NECK - n_top ? 1 : 0;
    if (n_bot > 0 && y >= pile_top) return 1;
    if (flowing && n_top > 0 && y < pile_top) return 2;
    return 0;
}

static void draw_row(int y, int st)
{
    int wi = inner_half(y), wo = wi + HG_WALL;
    lcd_fill_rect(HG_CX - wo, y, HG_CX - wi, y + 1, HG_GLASS);
    lcd_fill_rect(HG_CX + wi + 1, y, HG_CX + wo + 1, y + 1, HG_GLASS);
    if (st == 1) {
        lcd_fill_rect(HG_CX - wi, y, HG_CX + wi + 1, y + 1, HG_SAND);
    } else if (st == 2) {
        lcd_fill_rect(HG_CX - wi, y, HG_CX - 1, y + 1, COL_BLACK);
        lcd_fill_rect(HG_CX - 1, y, HG_CX + 2, y + 1, HG_SAND);
        lcd_fill_rect(HG_CX + 2, y, HG_CX + wi + 1, y + 1, COL_BLACK);
    } else {
        lcd_fill_rect(HG_CX - wi, y, HG_CX + wi + 1, y + 1, COL_BLACK);
    }
}

static int8_t s_shown[LCD_H];                             // what each row shows now (-1 = not drawn yet)
static char   s_time_key[16];

static void frame_reset(void)
{
    for (int y = 0; y < LCD_H; y++) s_shown[y] = -1;
    s_time_key[0] = 0;
    s_dirty = true;
}

static void draw_caps(void)
{
    lcd_fill_rect(HG_CX - 38, HG_TOP, HG_CX + 39, HG_TOP + HG_WALL, COL_GREEN);
    lcd_fill_rect(HG_CX - 38, HG_BOT - HG_WALL + 1, HG_CX + 39, HG_BOT + 1, COL_GREEN);
}

static void draw_hints(void)
{
    const char *a = "", *b = "";
    switch (s_state) {
        case T_IDLE:  a = "前鍵開始 側鍵換時間"; break;
        case T_RUN:   a = "前鍵暫停 側鍵取消";   break;
        case T_PAUSE: a = "前鍵繼續 側鍵取消";   break;
        case T_DONE:  a = "按任一鍵重新開始";     break;
    }
    b = "按住前鍵 回選單";
    lcd_fill_rect(0, 206, LCD_W, 236, COL_BLACK);
    zh_center(208, a, COL_LIGHT_GRAY, 1);
    zh_center(222, b, COL_LIGHT_GRAY, 1);
}

static uint32_t s_flips_seen;
static void timer_front_up(uint32_t held, bool is_long);
static void timer_reset_and_start(void);

// Turned over like a real hourglass: idle or paused starts; running reverses the sand
// (what has run becomes what is left); finished starts again.
static void timer_flip(void)
{
    switch (s_state) {
        case T_IDLE:
        case T_PAUSE: timer_front_up(0, false); break;
        case T_RUN: {
            uint32_t total = preset_ms(), left = remaining_ms();
            s_left_ms = total - (left > total ? total : left);
            s_end = xTaskGetTickCount() + pdMS_TO_TICKS(s_left_ms);
            if (s_left_ms == 0) { s_state = T_DONE; }
            s_dirty = true;
            break;
        }
        case T_DONE:  timer_reset_and_start(); break;
    }
}

static uint32_t timer_tick(bool first)
{
    if (first) { frame_reset(); s_flips_seen = g_imu_flips; }
    if (g_imu_flips != s_flips_seen) { s_flips_seen = g_imu_flips; timer_flip(); }
    uint32_t left = remaining_ms();
    if (s_state == T_RUN && left == 0) { s_state = T_DONE; s_left_ms = 0; s_dirty = true; beep_play(3); }
    uint32_t total = preset_ms();
    int n_top = (int)(((uint64_t)(s_state == T_DONE ? 0 : left) * HG_HALF + total / 2) / total);
    if (n_top > HG_HALF) n_top = HG_HALF;
    int n_bot = HG_HALF - n_top;
    if (s_state == T_IDLE) { n_top = HG_HALF; n_bot = 0; }
    bool flowing = s_state == T_RUN;

    xSemaphoreTake(g_pres_lcd_mu, portMAX_DELAY);
    if (first) draw_caps();
    for (int y = HG_TOP + HG_WALL; y < HG_BOT - HG_WALL + 1; y++) {
        int st = row_state(y, n_top, n_bot, flowing);
        if (st != s_shown[y]) { draw_row(y, st); s_shown[y] = (int8_t)st; }
    }
    char key[16];
    if (s_state == T_DONE) snprintf(key, sizeof(key), "DONE%d", (int)((xTaskGetTickCount() / pdMS_TO_TICKS(500)) & 1));
    else { uint32_t s = (left + 999) / 1000; snprintf(key, sizeof(key), "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60)); }
    if (strcmp(key, s_time_key) != 0) {
        strcpy(s_time_key, key);
        lcd_fill_rect(0, 178, LCD_W, 206, COL_BLACK);
        if (s_state == T_DONE) zh_center(180, "時間到", (key[4] == '1') ? COL_RED : COL_YELLOW, 2);
        else {
            int w = lcd_text_width(key, 3);
            lcd_draw_text((LCD_W - w) / 2, 180, key, 3, s_state == T_PAUSE ? COL_YELLOW : COL_WHITE);
        }
    }
    if (s_dirty) { s_dirty = false; draw_hints(); }
    xSemaphoreGive(g_pres_lcd_mu);
    return s_state == T_DONE ? 250 : 100;
}

// ---- buttons --------------------------------------------------------------
static void timer_reset(void)
{
    s_state = T_IDLE;
    s_left_ms = preset_ms();
    s_dirty = true;
    s_time_key[0] = 0;
}

static void timer_reset_and_start(void)
{
    timer_reset();
    timer_front_up(0, false);
}

static void timer_front_up(uint32_t held, bool is_long)
{
    (void)held; (void)is_long;
    switch (s_state) {
        case T_IDLE:  s_end = xTaskGetTickCount() + pdMS_TO_TICKS(s_left_ms); s_state = T_RUN; break;
        case T_RUN:   s_left_ms = remaining_ms(); s_state = T_PAUSE; break;
        case T_PAUSE: s_end = xTaskGetTickCount() + pdMS_TO_TICKS(s_left_ms); s_state = T_RUN; break;
        case T_DONE:  timer_reset(); break;
    }
    s_dirty = true;
}

static void timer_side_down(void)
{
    if (s_state == T_IDLE) {
        s_preset = (s_preset + 1) % (int)(sizeof(k_minutes) / sizeof(k_minutes[0]));
        s_left_ms = preset_ms();
        s_time_key[0] = 0;
    } else {
        timer_reset();                                       // cancel
    }
}

static void timer_enter(void) { timer_reset(); }
static void timer_leave(void) { s_state = T_IDLE; }

static void timer_icon(int x, int y, uint16_t fg, uint16_t bg)     // a small hourglass
{
    (void)bg;
    lcd_fill_rect(x + 3, y + 2, x + 25, y + 5, fg);
    lcd_fill_rect(x + 3, y + 23, x + 25, y + 26, fg);
    for (int r = 0; r < 9; r++) {
        int half = 9 - r;
        lcd_fill_rect(x + 14 - half - 1, y + 5 + r, x + 14 - half + 1, y + 6 + r, fg);
        lcd_fill_rect(x + 14 + half, y + 5 + r, x + 14 + half + 2, y + 6 + r, fg);
        lcd_fill_rect(x + 14 - half - 1, y + 22 - r, x + 14 - half + 1, y + 23 - r, fg);
        lcd_fill_rect(x + 14 + half, y + 22 - r, x + 14 + half + 2, y + 23 - r, fg);
        if (r >= 5) lcd_fill_rect(x + 14 - half + 1, y + 5 + r, x + 14 + half, y + 6 + r, fg);   // sand in the top bulb
        if (r <= 4) lcd_fill_rect(x + 14 - half + 1, y + 22 - r, x + 14 + half, y + 23 - r, fg); // sand in the bottom bulb
    }
}

const app_t app_timer = {
    .name = "TIMER", .name_zh = "沙漏", .desc_zh = "倒數計時", .color = COL_GREEN, .icon = timer_icon,
    .flags = APP_NO_SLEEP | APP_HOLD_EXITS,
    .enter = timer_enter, .leave = timer_leave,
    .tick = timer_tick, .front_up = timer_front_up, .side_down = timer_side_down,
};

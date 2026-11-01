// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

// =========================================================================
// LCD — ST7789P3 over SPI3, used as a low-power status indicator.
//
// Boot draws a tiny colour badge; runtime draws a small mic icon and audio
// meter on a black background. This is enough to tell the stick is alive
// without running the StickS3 LCD at full-brightness flood-fill all day.
//
// Stage palette (also returned by lcd_status() for grep-ability):
//   white   = boot reached app_main
//   yellow  = I2S RX up
//   orange  = ES8311 codec configured
//   cyan    = UAC + HID composite running, advertising as voxstick
//   green   = mic streaming + buttons live (ready)
//   red     = something panicked
// =========================================================================
esp_lcd_panel_handle_t g_lcd = NULL;
// Static line buffer is enough for a single full-width row — colors fit in a
// uint16_t and we paint by repeating one row 240 times.
#define LCD_BLK_ROWS 16                          // rows per SPI transfer: 16x fewer transactions than row-by-row
static uint16_t g_lcd_line[2][LCD_W * LCD_BLK_ROWS];   // two buffers, alternated per call
static int g_lcd_line_sel = 0;
static esp_lcd_panel_io_handle_t g_lcd_io = NULL;
RTC_NOINIT_ATTR volatile uint16_t vox_where_lcd;      // last LCD draw: kind*1000 + y (1 fill, 2 fill by another task, 3 text)
static StaticSemaphore_t g_lcd_spi_mu_buf;
static SemaphoreHandle_t g_lcd_spi_mu = NULL;     // leaf lock around one rectangle: SPI and buffers never shared
volatile uint32_t g_lcd_contended = 0;     // times a drawer had to wait for another one
volatile uint32_t g_lcd_wait_max_ms = 0;

static inline uint16_t lcd_swap_rgb565(uint16_t rgb565)
{
    return (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
}

static esp_err_t lcd_backlight_init(void)
{
    gpio_reset_pin(LCD_BL_PIN);
    gpio_set_direction(LCD_BL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL_PIN, 0);

    ledc_timer_config_t timer_cfg = {
        .speed_mode       = LCD_BL_LEDC_MODE,
        .duty_resolution  = LCD_BL_DUTY_RES,
        .timer_num        = LCD_BL_LEDC_TIMER,
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "lcd_bl_timer");

    ledc_channel_config_t channel_cfg = {
        .gpio_num   = LCD_BL_PIN,
        .speed_mode = LCD_BL_LEDC_MODE,
        .channel    = LCD_BL_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LCD_BL_LEDC_TIMER,
        .duty       = LCD_BL_DUTY_READY,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_cfg), TAG, "lcd_bl_channel");

    ESP_LOGI(TAG, "lcd backlight GPIO%d pwm duty=%u/%u",
             LCD_BL_PIN, (unsigned)LCD_BL_DUTY_READY, (unsigned)LCD_BL_DUTY_MAX);
    return ESP_OK;
}

esp_err_t lcd_init(void)
{
    // Keep the backlight off while the panel is being initialized; LEDC PWM
    // starts it at a dim duty cycle once the panel is ready.
    gpio_reset_pin(LCD_BL_PIN);
    gpio_set_direction(LCD_BL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL_PIN, 0);

    spi_bus_config_t buscfg = {
        .sclk_io_num     = LCD_SCK_PIN,
        .mosi_io_num     = LCD_MOSI_PIN,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_W * LCD_H * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize");

    g_lcd_spi_mu = xSemaphoreCreateMutexStatic(&g_lcd_spi_mu_buf);   // before any task can draw
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num         = LCD_DC_PIN,
        .cs_gpio_num         = LCD_CS_PIN,
        .pclk_hz             = 40 * 1000 * 1000,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .spi_mode            = 0,
        .trans_queue_depth   = 10,
        .flags = {
            // M5GFX configures the StickS3 LCD bus as 3-wire SPI. Keep DC as
            // a separate GPIO, but make the SPI device half-duplex single-I/O
            // so the bus timing matches the known-good M5Stack path.
            .sio_mode = 1,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_cfg, &io_handle),
                        TAG, "panel_io_spi");
    g_lcd_io = io_handle;

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST_PIN,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &g_lcd),
                        TAG, "new_panel_st7789");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(g_lcd), TAG, "panel_reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(g_lcd),  TAG, "panel_init");
    // ST7789 needs colour inversion on for a normal-looking image.
    esp_lcd_panel_invert_color(g_lcd, true);
    // 135-wide panels are mounted with X offset 52 / Y offset 40 against the
    // 240x320 framebuffer ST7789 hardware actually has.
    esp_lcd_panel_set_gap(g_lcd, LCD_X_OFFSET, LCD_Y_OFFSET);
    esp_lcd_panel_disp_on_off(g_lcd, true);
    ESP_RETURN_ON_ERROR(lcd_backlight_init(), TAG, "lcd_backlight");
    ESP_LOGI(TAG, "lcd init ok (ST7789 %dx%d gap=%d,%d)",
             LCD_W, LCD_H, LCD_X_OFFSET, LCD_Y_OFFSET);
    return ESP_OK;
}

// Linked in with -Wl,--wrap=esp_panic_handler: remember where a crash happened so the
// DIAG page can show it after the automatic restart (the console is not reachable).
extern void __real_esp_panic_handler(panic_info_t *info);
void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    vox_panic_pc = (uint32_t)info->addr;
    vox_panic_exc = (uint32_t)info->exception;
    vox_panic_magic = 0xFACE;
    __real_esp_panic_handler(info);
}

static void lcd_spi_lock(void)
{
    if (!g_lcd_spi_mu) return;                  // not initialised: nothing can be drawn yet
    if (xSemaphoreTake(g_lcd_spi_mu, 0) != pdTRUE) {
        TickType_t t0 = xTaskGetTickCount();
        g_lcd_contended++;
        xSemaphoreTake(g_lcd_spi_mu, portMAX_DELAY);
        uint32_t ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - t0);
        if (ms > g_lcd_wait_max_ms) g_lcd_wait_max_ms = ms;
    }
}

// A command sent with polling waits until every queued colour transfer is done, so the
// buffers can be reused as soon as the lock is released.
static void lcd_flush(void)
{
    if (g_lcd_io) esp_lcd_panel_io_tx_param(g_lcd_io, 0x00, NULL, 0);   // ST7789 NOP
}

void lcd_fill_rect(int x0, int y0, int x1, int y1, uint16_t rgb565)
{
    if (!g_lcd) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LCD_W) x1 = LCD_W;
    if (y1 > LCD_H) y1 = LCD_H;
    if (x0 >= x1 || y0 >= y1) return;

    lcd_spi_lock();
    uint16_t px = lcd_swap_rgb565(rgb565);
    int width = x1 - x0;
    uint16_t *line = g_lcd_line[g_lcd_line_sel ^= 1];
    int rows = y1 - y0 < LCD_BLK_ROWS ? y1 - y0 : LCD_BLK_ROWS;
    for (int i = 0; i < width * rows; i++) line[i] = px;
    bool mine = (g_disp_task != NULL && xTaskGetCurrentTaskHandle() == g_disp_task);
    if (mine) vox_where_disp = 5;                // inside an LCD transfer: a stall here means the SPI draw hung
    for (int y = y0; y < y1; y += LCD_BLK_ROWS) {
        int n = y1 - y < LCD_BLK_ROWS ? y1 - y : LCD_BLK_ROWS;
        vox_where_lcd = (uint16_t)((mine ? 1000 : 2000) + y);
        esp_lcd_panel_draw_bitmap(g_lcd, x0, y, x1, y + n, line);
    }
    lcd_flush();
    if (mine) vox_where_disp = 4;
    xSemaphoreGive(g_lcd_spi_mu);
}

volatile bool g_bar_dirty = true;         // status bar needs repainting
void lcd_clear(uint16_t rgb565)
{
    lcd_fill_rect(0, 0, LCD_W, LCD_H, rgb565);
    g_bar_dirty = true;
}

// Low-power boot/status marker: black screen plus a tiny colour badge.
void lcd_status(uint16_t rgb565)
{
    if (!g_lcd) return;
    lcd_clear(COL_BLACK);
    lcd_fill_rect((LCD_W - 38) / 2, LCD_H - 20,
                  (LCD_W + 38) / 2, LCD_H - 12, rgb565);
}

void lcd_draw_thick_line(int x0, int y0, int x1, int y1,
                                int thickness, uint16_t rgb565)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
        ? (dx < 0 ? -dx : dx)
        : (dy < 0 ? -dy : dy);
    if (steps == 0) {
        lcd_fill_rect(x0, y0, x0 + thickness, y0 + thickness, rgb565);
        return;
    }

    int r = thickness / 2;
    for (int i = 0; i <= steps; i++) {
        int x = x0 + (dx * i) / steps;
        int y = y0 + (dy * i) / steps;
        lcd_fill_rect(x - r, y - r, x + r + 1, y + r + 1, rgb565);
    }
}

void lcd_draw_mic_status(uint32_t level, bool active,
                                bool codec_ready, bool muted)
{
    if (!g_lcd) return;

    if (level > VAD_FULL_LEVEL) {
        level = VAD_FULL_LEVEL;
    }

    const int cx = LCD_W / 2;
    uint16_t mic = codec_ready
        ? (muted ? COL_LIGHT_GRAY : (active ? COL_WHITE : COL_CYAN))
        : COL_RED;
    uint16_t accent = codec_ready
        ? (muted ? COL_DARK_GRAY : (active ? COL_GREEN : COL_DIM_CYAN))
        : COL_RED;

    lcd_fill_rect(0, 34, LCD_W, 184, COL_BLACK);

    // Mic capsule outline.
    lcd_fill_rect(cx - 10, 58, cx + 11, 62, mic);
    lcd_fill_rect(cx - 15, 62, cx + 16, 66, mic);
    lcd_fill_rect(cx - 18, 66, cx - 14, 112, mic);
    lcd_fill_rect(cx + 14, 66, cx + 18, 112, mic);
    lcd_fill_rect(cx - 15, 112, cx + 16, 116, mic);
    lcd_fill_rect(cx - 10, 116, cx + 11, 120, mic);

    if (active && codec_ready && !muted) {
        lcd_fill_rect(cx - 10, 72, cx + 11, 106, COL_DIM_CYAN);
    }

    // Pickup frame and stand.
    lcd_fill_rect(cx - 30, 92, cx - 26, 114, mic);
    lcd_fill_rect(cx + 26, 92, cx + 30, 114, mic);
    lcd_fill_rect(cx - 26, 120, cx + 27, 124, mic);
    lcd_fill_rect(cx - 2, 124, cx + 3, 148, mic);
    lcd_fill_rect(cx - 20, 148, cx + 21, 152, mic);

    // Tiny level meter: enough movement to show life, not a bright animation.
    lcd_fill_rect(cx - 30, 166, cx + 31, 170, COL_DARK_GRAY);
    if (codec_ready && !muted && level > 0) {
        int meter_w = (int)(level * 61U / VAD_FULL_LEVEL);
        if (meter_w < 2) meter_w = 2;
        lcd_fill_rect(cx - 30, 166, cx - 30 + meter_w, 170, accent);
    }

    lcd_fill_rect(0, 184, LCD_W, 204, COL_BLACK);
    zh_center(188, !codec_ready ? "麥克風故障" : (muted ? "已靜音 平放中" : (active ? "收音中" : "待命")),
              !codec_ready ? COL_RED : (muted ? COL_LIGHT_GRAY : (active ? COL_GREEN : COL_CYAN)), 1);
    if (muted) {
        lcd_draw_thick_line(cx - 28, 150, cx + 28, 62, 4, COL_RED);
    } else if (!codec_ready) {
        lcd_draw_thick_line(cx - 28, 154, cx + 28, 58, 5, COL_RED);
        lcd_draw_thick_line(cx - 28, 58, cx + 28, 154, 5, COL_RED);
    }
}

// Presenter screen: orange right-arrow on black. Mic is muted in this mode.
static volatile bool g_presenter_redraw = false;
volatile TickType_t g_passkey_hold_until = 0;   // keep passkey on screen
// Presenter-screen draws come from two tasks (LCD task, NimBLE host task).
StaticSemaphore_t g_pres_lcd_mu_buf;
SemaphoreHandle_t g_pres_lcd_mu = NULL;

// Tiny 5x7 text for menu labels. Column-major, bit0 = top row.
static const char    k_font_chars[] = "ABCDEGHIKLMNOPRSTUY0123456789%-FVWQXJZ:./!~";
static const uint8_t k_font[][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06}, {0x7F,0x09,0x19,0x29,0x46},
    {0x26,0x49,0x49,0x49,0x32}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x07,0x08,0x70,0x08,0x07},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E}, {0x23,0x13,0x08,0x64,0x62}, {0x08,0x08,0x08,0x08,0x08},
    {0x7F,0x09,0x09,0x09,0x01}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
    {0x3E,0x41,0x51,0x21,0x5E},
    {0x63,0x14,0x08,0x14,0x63}, {0x20,0x40,0x41,0x3F,0x01}, {0x61,0x51,0x49,0x45,0x43},   // X J Z
    {0x00,0x36,0x36,0x00,0x00}, {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},   // : . /
    {0x00,0x00,0x5F,0x00,0x00},                                                          // !
    {0x08,0x04,0x04,0x08,0x10},                                                          // ~
};

int lcd_text_width(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;
}

// ---- Chinese / full text from the 12 px bitmap font (zh_font.c) ---------------
static const zh_glyph_t *zh_find(uint32_t cp)
{
    int lo = 0, hi = k_zh_glyph_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (k_zh_glyphs[mid].cp == cp) return &k_zh_glyphs[mid];
        if (k_zh_glyphs[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

// Next code point of a UTF-8 string (BMP only); advances *s.
static uint32_t zh_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = p[0];
    int n = 1;
    if (c >= 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) { c = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
    else if (c >= 0xC0 && (p[1] & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
    *s += n;
    return c;
}

int zh_text_width(const char *s, int scale)
{
    int w = 0;
    while (*s) { const zh_glyph_t *g = zh_find(zh_next(&s)); w += (g ? g->w : 12) * scale; }
    return w;
}

#define ZH_MAX_SCALE 3
static uint16_t g_zh_buf[3][12 * ZH_MAX_SCALE * 12 * ZH_MAX_SCALE];   // ring: the previous glyph may still be on the wire
static int g_zh_sel = 0;

// Draws text with a background colour (the cell is fully painted, no clearing needed).
int zh_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    uint16_t f = lcd_swap_rgb565(fg), b = lcd_swap_rgb565(bg);
    if (!g_lcd) return x;
    if (scale < 1) scale = 1;
    if (scale > ZH_MAX_SCALE) scale = ZH_MAX_SCALE;
    while (*s) {
        const zh_glyph_t *g = zh_find(zh_next(&s));
        int cw = (g ? g->w : 12) * scale;
        if (x + cw > LCD_W) break;
        if (g) {
            uint16_t *buf = g_zh_buf[g_zh_sel = (g_zh_sel + 1) % 3];
            for (int r = 0; r < 12 * scale; r++)
                for (int c = 0; c < cw; c++)
                    buf[r * cw + c] = (g->rows[r / scale] >> (c / scale)) & 1 ? f : b;
            lcd_spi_lock();
            if (g_lcd) {
                vox_where_lcd = (uint16_t)(3000 + x);
                esp_lcd_panel_draw_bitmap(g_lcd, x, y, x + cw, y + 12 * scale, buf);
                lcd_flush();
            }
            xSemaphoreGive(g_lcd_spi_mu);
        }
        x += cw;
    }
    return x;
}

// Centered Chinese/ASCII text in the 12 px font; returns nothing. Caller holds the UI lock.
void zh_center(int y, const char *s, uint16_t fg, int scale)
{
    int x = (LCD_W - zh_text_width(s, scale)) / 2;
    zh_text(x < 0 ? 0 : x, y, s, fg, COL_BLACK, scale);
}

void lcd_draw_text(int x, int y, const char *s, int scale, uint16_t col)
{
    for (; *s; s++, x += 6 * scale) {
        const char *hit = strchr(k_font_chars, *s);
        if (*s == ' ' || !hit) continue;
        const uint8_t *g = k_font[hit - k_font_chars];
        for (int c = 0; c < 5; c++) {
            int r = 0;
            while (r < 7) {
                if (!(g[c] & (1 << r))) { r++; continue; }
                int r0 = r;
                while (r < 7 && (g[c] & (1 << r))) r++;
                lcd_fill_rect(x + c * scale, y + r0 * scale,
                              x + (c + 1) * scale, y + r * scale, col);
            }
        }
    }
}


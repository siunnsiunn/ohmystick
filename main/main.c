// voxstick firmware — Step 1: USB Audio Class mic only.
//
// Pipeline: ES8311 codec (I2C0 config + I2S RX data) → UAC input callback →
// host sees a 16 kHz / mono / 16-bit USB microphone called "StickS3-Mic".
//
// We rely on Espressif's `esp_codec_dev` to handle the ~100 lines of ES8311
// register dance (MCLK ratio, ADC gain, mic bias, etc.) and on
// `usb_device_uac` to handle every byte of the USB descriptor + class layer.
// The application code is intentionally thin so it stays auditable.
//
// Pin map (M5Stack StickS3):
//   I2C0 SDA=47   SCL=48                 -> ES8311 (addr 0x18) + PMIC + IMU
//   I2S1 MCLK=18  BCLK=17  WS=15  DIN=16 -> ES8311 ADC (mic data)
//
// PA / speaker output (DOUT=14) is intentionally untouched in v1; the StickS3
// PA defaults to ON via the PMIC and will tick if we leave I2S TX disabled,
// but until we drive the speaker that is acceptable. Step 3 will silence it
// via the M5PM1 register set.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "usb_device_uac.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_rom_sys.h"
#include "tusb.h"
#include "class/hid/hid.h"
#include "class/hid/hid_device.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "bmi270.h"
#include "bmi2_defs.h"
#include "vox_config.h"

// Set to 1 to skip TinyUSB / UAC init - keeps USB-Serial/JTAG console
// alive on /dev/cu.usbmodem* so we can read ESP_LOG output. Diagnostic
// build only; production must be 0.
#ifndef VOX_DEBUG_NO_UAC
#define VOX_DEBUG_NO_UAC  0
#endif

// Development escape hatches. Production defaults keep the app running.
// Set either value from CMake, e.g.:
//   idf.py -DVOX_AUTO_DOWNLOAD_AFTER_SEC=90 build
// so a flashed UAC/HID build can put itself back into ROM download mode
// without a physical button press.
#ifndef VOX_AUTO_DOWNLOAD_AFTER_SEC
#define VOX_AUTO_DOWNLOAD_AFTER_SEC  0
#endif

#ifndef VOX_CODEC_FAIL_DOWNLOAD_AFTER_SEC
#define VOX_CODEC_FAIL_DOWNLOAD_AFTER_SEC  0
#endif

// Diagnostic only: emit a recognizable square wave instead of silence when
// codec init fails. Production keeps this off so VoiceInk never receives fake
// audio if the mic hardware is unhealthy.
#ifndef VOX_DIAG_CODEC_FAIL_TONE
#define VOX_DIAG_CODEC_FAIL_TONE  0
#endif

static const char *TAG = "voxstick";

// ---- Board pin map -------------------------------------------------------
#define I2C_PORT      I2C_NUM_0
#define I2C_SDA_PIN   47
#define I2C_SCL_PIN   48

#define I2S_PORT      I2S_NUM_1
#define I2S_MCLK_PIN  18
#define I2S_BCLK_PIN  17
#define I2S_WS_PIN    15
#define I2S_DIN_PIN   16

// StickS3 face button. BtnA = front, big M5 logo. BtnB header is unpopulated
// on this hardware revision so we don't bother polling it.
#define BTN_A_GPIO    GPIO_NUM_11

// ---- LCD (ST7789P3 135x240, SPI3) ---------------------------------------
// Numbers from the M5StickS3 datasheet / weclawbot-base reference.
#define LCD_HOST          SPI3_HOST
#define LCD_BL_PIN        38
#define LCD_RST_PIN       21
#define LCD_DC_PIN        45
#define LCD_CS_PIN        41
#define LCD_SCK_PIN       40
#define LCD_MOSI_PIN      39
#define LCD_W             135
#define LCD_H             240
#define LCD_BL_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define LCD_BL_LEDC_TIMER     LEDC_TIMER_0
#define LCD_BL_LEDC_CHANNEL   LEDC_CHANNEL_0
#define LCD_BL_DUTY_RES       LEDC_TIMER_10_BIT
#define LCD_BL_DUTY_MAX       ((1U << 10) - 1U)
#define LCD_BL_DUTY_READY     ((LCD_BL_DUTY_MAX * 12U) / 100U)
// ST7789 has 240-row visible window with a 40-row offset for 135-wide panels.
#define LCD_X_OFFSET      52
#define LCD_Y_OFFSET      40

// 16-bit RGB565 colors for low-power status drawing. Runtime leaves most
// pixels black and keeps the backlight dim; the user still gets a clear mic
// open / muted signal without lighting the whole LCD.
#define COL_BLACK         0x0000
#define COL_WHITE         0xFFFF
#define COL_RED           0xF800
#define COL_GREEN         0x07E0
#define COL_BLUE          0x001F
#define COL_YELLOW        0xFFE0
#define COL_CYAN          0x07FF
#define COL_MAGENTA       0xF81F
#define COL_ORANGE        0xFD20
#define COL_DIM_RED       0x4000
#define COL_DIM_CYAN      0x0210
#define COL_DARK_GRAY     0x0841
#define COL_LIGHT_GRAY    0xBDF7

// ---- Audio format (must match sdkconfig CONFIG_UAC_*) --------------------
#define AUDIO_SAMPLE_RATE  16000
#define AUDIO_CHANNELS     1
#define AUDIO_BITS         16
#define AUDIO_MCLK_MULTIPLE I2S_MCLK_MULTIPLE_128
#define AUDIO_MCLK_DIV      128
#define AUDIO_INPUT_MASK    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1)

// Short enough for deliberate tooling, long enough for normal host jitter.
#define DOWNLOAD_MAGIC_WINDOW_MS 2000
#define VENDOR_DOWNLOAD_REQUEST  0x5D
#define VENDOR_DOWNLOAD_VALUE    0x5344  // "SD"
#define VENDOR_DOWNLOAD_INDEX    0x4C44  // "LD"
#define VENDOR_DOWNLOAD_MAGIC    "VOXSTICK_DOWNLOAD"

// ---- On-device VAD display ----------------------------------------------
// Cheap energy gate for the LCD indicator. It is intentionally not a speech
// recognition VAD; VoiceInk does the real transcription. This just makes the
// stick feel alive and tells us whether audio energy is reaching firmware.
#define VAD_ON_LEVEL       900U
#define VAD_OFF_LEVEL      450U
#define VAD_FULL_LEVEL     5000U
#define VAD_FRAME_MS       100

// ---- Globals -------------------------------------------------------------
static i2c_master_bus_handle_t  g_i2c_bus     = NULL;
static i2s_chan_handle_t        g_i2s_rx      = NULL;
static esp_codec_dev_handle_t   g_codec_dev   = NULL;
static volatile bool            g_codec_ready = false;
static volatile bool            g_uac_muted   = false;
static volatile bool            g_download_scheduled = false;
static TaskHandle_t             g_download_task_handle = NULL;
static const char              *g_download_reason = "download requested";
static uint32_t                 g_download_delay_ms = 0;
static uint32_t                 g_diag_tone_phase = 0;
static volatile uint32_t        g_vad_level = 0;
static volatile bool            g_vad_active = false;
static volatile bool            g_vad_display_enabled = false;
// IMU orientation-based mute: true when the BMI270 reports the stick is
// lying flat. ORed into the audio path so flat-on-table = silence,
// pick-it-up = live mic. Independent of g_uac_muted (host volume mute)
// and g_codec_ready (codec init failure).
static volatile bool            g_imu_mute  = false;

static bool effective_mic_muted(void)
{
    return g_uac_muted || (vox_config_flat_mute_enabled() && g_imu_mute);
}

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
static esp_lcd_panel_handle_t g_lcd = NULL;
// Static line buffer is enough for a single full-width row — colors fit in a
// uint16_t and we paint by repeating one row 240 times.
static uint16_t g_lcd_line[LCD_W];

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

static esp_err_t lcd_init(void)
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

static void lcd_fill_rect(int x0, int y0, int x1, int y1, uint16_t rgb565)
{
    if (!g_lcd) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LCD_W) x1 = LCD_W;
    if (y1 > LCD_H) y1 = LCD_H;
    if (x0 >= x1 || y0 >= y1) return;

    uint16_t px = lcd_swap_rgb565(rgb565);
    int width = x1 - x0;
    for (int x = 0; x < width; x++) g_lcd_line[x] = px;
    for (int y = y0; y < y1; y++) {
        esp_lcd_panel_draw_bitmap(g_lcd, x0, y, x1, y + 1, g_lcd_line);
    }
}

static void lcd_clear(uint16_t rgb565)
{
    lcd_fill_rect(0, 0, LCD_W, LCD_H, rgb565);
}

// Low-power boot/status marker: black screen plus a tiny colour badge.
static void lcd_status(uint16_t rgb565)
{
    if (!g_lcd) return;
    lcd_clear(COL_BLACK);
    lcd_fill_rect((LCD_W - 38) / 2, LCD_H - 20,
                  (LCD_W + 38) / 2, LCD_H - 12, rgb565);
}

static void lcd_draw_thick_line(int x0, int y0, int x1, int y1,
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

static void lcd_draw_mic_status(uint32_t level, bool active,
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

    if (muted) {
        lcd_draw_thick_line(cx - 28, 150, cx + 28, 62, 4, COL_RED);
    } else if (!codec_ready) {
        lcd_draw_thick_line(cx - 28, 154, cx + 28, 58, 5, COL_RED);
        lcd_draw_thick_line(cx - 28, 58, cx + 28, 154, 5, COL_RED);
    }
}

static void vad_lcd_task(void *arg)
{
    (void)arg;
    g_vad_display_enabled = true;

    bool last_active = false;
    bool last_codec_ready = false;
    bool last_muted = false;
    uint32_t last_bucket = UINT32_MAX;

    while (1) {
        if (!g_vad_display_enabled) {
            vTaskDelay(pdMS_TO_TICKS(VAD_FRAME_MS));
            continue;
        }

        uint32_t level = g_vad_level;
        uint32_t capped = level > VAD_FULL_LEVEL ? VAD_FULL_LEVEL : level;
        bool active = g_vad_active;
        bool codec_ready = g_codec_ready;
        bool muted = effective_mic_muted();
        if (muted || !codec_ready) {
            capped = 0;
            active = false;
        }
        uint32_t bucket = capped / 240U;

        if (active != last_active || codec_ready != last_codec_ready ||
            muted != last_muted || bucket != last_bucket) {
            lcd_draw_mic_status(capped, active, codec_ready, muted);
            last_active = active;
            last_codec_ready = codec_ready;
            last_muted = muted;
            last_bucket = bucket;
        }

        vTaskDelay(pdMS_TO_TICKS(VAD_FRAME_MS));
    }
}

// =========================================================================
// ROM download escape hatch
// =========================================================================
static void enter_rom_download_mode(const char *reason)
{
    g_vad_display_enabled = false;
    lcd_status(COL_BLUE);
    ESP_LOGW(TAG, "%s - rebooting into ROM download mode", reason);
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    // Give logs/LCD/USB callbacks a short chance to drain before reset.
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

static void download_task(void *arg)
{
    (void)arg;
    if (g_download_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(g_download_delay_ms));
    }
    enter_rom_download_mode(g_download_reason);
    vTaskDelete(NULL);
}

static void request_rom_download(const char *reason, uint32_t delay_ms)
{
    if (g_download_scheduled) {
        if (delay_ms >= g_download_delay_ms) {
            ESP_LOGW(TAG, "download already scheduled sooner/equal; ignoring request: %s", reason);
            return;
        }

        ESP_LOGW(TAG, "download rescheduled from %lu ms to %lu ms: %s",
                 (unsigned long)g_download_delay_ms,
                 (unsigned long)delay_ms,
                 reason);
        if (g_download_task_handle != NULL) {
            vTaskDelete(g_download_task_handle);
            g_download_task_handle = NULL;
        }
        g_download_scheduled = false;
    }

    g_download_scheduled = true;
    g_download_reason = reason;
    g_download_delay_ms = delay_ms;
    ESP_LOGW(TAG, "ROM download scheduled in %lu ms: %s",
             (unsigned long)delay_ms, reason);
    BaseType_t ok = xTaskCreate(download_task, "romdl", 3072, NULL,
                                configMAX_PRIORITIES - 1, &g_download_task_handle);
    if (ok != pdPASS) {
        enter_rom_download_mode(reason);
    }
}

// =========================================================================
// I2C bus (shared with PMIC + IMU + codec)
// =========================================================================
static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t cfg = {
        .clk_source                  = I2C_CLK_SRC_DEFAULT,
        .i2c_port                    = I2C_PORT,
        .scl_io_num                  = I2C_SCL_PIN,
        .sda_io_num                  = I2C_SDA_PIN,
        .glitch_ignore_cnt           = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &g_i2c_bus);
}

// =========================================================================
// PMIC — M5PM1 on the StickS3 internal I2C0.
//
// This is not an AXP2101 register map. Earlier experiments writing
// AXP2101-style LDO registers to this chip left the ES8311 unreachable until
// a true power-on reset. Keep this init intentionally tiny: discover the PMIC
// address, then configure only M5PM1 GPIO3, which controls the speaker PA.
//
// Observed address on this StickS3 is 0x6E. The 0x34/0x36 probes are left only
// as harmless diagnostics for board variation.
// =========================================================================
static uint8_t g_pmic_addr = 0;     // discovered at boot

static esp_err_t pmic_write(uint8_t reg, uint8_t val)
{
    if (g_pmic_addr == 0) return ESP_ERR_NOT_FOUND;
    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = g_pmic_addr,
        .scl_speed_hz    = 100000,        // conservative; this board dropped
                                          // ACKs during earlier 400 kHz tests.
    };
    esp_err_t ret = i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &dev);
    if (ret != ESP_OK) return ret;
    uint8_t buf[2] = { reg, val };
    ret = i2c_master_transmit(dev, buf, sizeof(buf), 100);
    i2c_master_bus_rm_device(dev);
    return ret;
}

// M5StickS3's M5PM1 always sits at 0x6E (verified across builds; the
// AXP2101 0x34 / 0x36 alternates were probed during early development
// and never ACK'd on this hardware). Probe still confirms presence so a
// dead-bus failure mode is logged loudly.
static esp_err_t pmic_probe(void)
{
    if (i2c_master_probe(g_i2c_bus, 0x6E, 100) != ESP_OK) {
        ESP_LOGE(TAG, "no pmic at 0x6E");
        lcd_status(COL_RED);
        return ESP_ERR_NOT_FOUND;
    }
    g_pmic_addr = 0x6E;
    ESP_LOGI(TAG, "pmic ACK at 0x6E");
    return ESP_OK;
}

// Scan the I2C bus and log every device that ACKs. Used once to find out
// which address the M5PM1 actually sits at on this hardware revision.
static void i2c_scan(void)
{
    ESP_LOGI(TAG, "i2c bus scan starting...");
    int found = 0;
    for (uint8_t a = 1; a < 0x78; a++) {
        if (i2c_master_probe(g_i2c_bus, a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  -> 0x%02x ACK", a);
            found++;
        }
    }
    ESP_LOGI(TAG, "i2c scan done: %d device(s)", found);
}

// PMIC reg helper for read-modify-write of single bits, M5Unified-style.
static esp_err_t pmic_read(uint8_t reg, uint8_t *val)
{
    if (g_pmic_addr == 0) return ESP_ERR_NOT_FOUND;
    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = g_pmic_addr,
        .scl_speed_hz    = 100000,
    };
    esp_err_t ret = i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &dev);
    if (ret != ESP_OK) return ret;
    ret = i2c_master_transmit_receive(dev, &reg, 1, val, 1, 100);
    i2c_master_bus_rm_device(dev);
    return ret;
}

static esp_err_t pmic_clear_bit(uint8_t reg, uint8_t bit_mask)
{
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(pmic_read(reg, &v), TAG, "pmic_read");
    return pmic_write(reg, v & ~bit_mask);
}

static esp_err_t pmic_set_bit(uint8_t reg, uint8_t bit_mask)
{
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(pmic_read(reg, &v), TAG, "pmic_read");
    return pmic_write(reg, v | bit_mask);
}

static esp_err_t pmic_enable_lcd_codec_rail(void)
{
    // From M5GFX/src/M5GFX.cpp board_M5StickS3 init:
    //
    //   "PM1_G2 -- L3B Enable, LCD Power On (M5Stack PM1 G2)"
    //
    // The LCD module Vcc and the ES8311 audio codec share an external
    // L3B-style LDO whose enable pin is wired to M5PM1 GPIO2. M5GFX's
    // board_M5StickS3 path drives this output HIGH before initializing the
    // panel; mirror that sequence exactly here.
    ESP_RETURN_ON_ERROR(pmic_clear_bit(0x16, 1 << 2), TAG, "G2 gpio fn");
    ESP_RETURN_ON_ERROR(pmic_set_bit  (0x10, 1 << 2), TAG, "G2 dir out");
    ESP_RETURN_ON_ERROR(pmic_clear_bit(0x13, 1 << 2), TAG, "G2 push-pull");
    ESP_RETURN_ON_ERROR(pmic_set_bit  (0x11, 1 << 2), TAG, "G2 out HIGH/L3B on");

    // Register 0x09 = I2C_CFG. M5GFX comment:
    //
    //   "Set to 0x00 to disable I2C idle sleep mode. PMIC is always-on
    //    powered, and with battery power, shutdown doesn't reset the chip.
    //    This register may have been modified elsewhere, causing PMIC
    //    communication issues. Explicitly set it here during init to
    //    ensure proper operation."
    //
    // Without this we've seen the PMIC stop ACK'ing after a few seconds.
    ESP_RETURN_ON_ERROR(pmic_write(0x09, 0x00), TAG, "I2C_CFG no-sleep");

    // L3B settle. M5GFX waits 100 ms before initializing the LCD bus.
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static void pmic_dump_regs(const char *label)
{
    static const uint8_t regs[] = { 0x06, 0x10, 0x11, 0x13, 0x16 };
    ESP_LOGI(TAG, "pmic dump: %s", label);
    for (size_t i = 0; i < sizeof(regs); i++) {
        uint8_t v = 0;
        esp_err_t ret = pmic_read(regs[i], &v);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  reg 0x%02x = 0x%02x", regs[i], v);
        } else {
            ESP_LOGW(TAG, "  reg 0x%02x read failed: %s",
                     regs[i], esp_err_to_name(ret));
        }
    }
}

// M5StickS3 PMIC init based on M5GFX/M5Unified upstream:
//   * GPIO2 (PM1_G2) HIGH — enables L3B LDO that powers the LCD module Vcc
//     and the ES8311 audio codec rail. Has to come up before any LCD or
//     codec bring-up is attempted.
//   * register 0x09 = 0x00 — disables PMIC I2C idle sleep so subsequent
//     transactions don't drop after a few seconds of bus quiet.
//   * GPIO3 (PM1_G3) LOW with push-pull output — speaker PA mute, prevents
//     the amplifier ticking on every I2S DMA start.
//
// M5PM1 register map (from M5Unified / M5GFX):
//   0x06  Power config (bit 2 = LDO_EN on M5PaperColor; *not* LCD on StickS3)
//   0x09  I2C_CFG       (bit 0 = idle sleep enable; clear to keep PMIC awake)
//   0x10  GPIO direction      bit n: 1 = output, 0 = input
//   0x11  GPIO output level   bit n: 1 = high, 0 = low
//   0x13  GPIO mode           bit n: 1 = open-drain, 0 = push-pull
//   0x16  GPIO function       bit n: 1 = alt, 0 = GPIO
static esp_err_t pmic_init(void)
{
    ESP_RETURN_ON_ERROR(pmic_probe(), TAG, "pmic probe");
    pmic_dump_regs("before init");

    ESP_RETURN_ON_ERROR(pmic_enable_lcd_codec_rail(), TAG, "L3B / I2C_CFG");

    // Speaker PA off: GPIO3 as push-pull output, low.
    ESP_RETURN_ON_ERROR(pmic_clear_bit(0x16, 1 << 3), TAG, "PA gpio fn");
    ESP_RETURN_ON_ERROR(pmic_set_bit  (0x10, 1 << 3), TAG, "PA dir out");
    ESP_RETURN_ON_ERROR(pmic_clear_bit(0x13, 1 << 3), TAG, "PA push-pull");
    ESP_RETURN_ON_ERROR(pmic_clear_bit(0x11, 1 << 3), TAG, "PA out low");

    pmic_dump_regs("after init");

    ESP_LOGI(TAG, "pmic configured (G2=HIGH/L3B on, G3=LOW/PA off)");
    return ESP_OK;
}

// =========================================================================
// I2S RX channel — pure data path. Codec config goes through the I2C side.
// =========================================================================
static esp_err_t i2s_rx_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &g_i2s_rx),
                        TAG, "i2s_new_channel rx");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_DIN_PIN,
            .invert_flags = { 0 },
        },
    };
    // M5Unified's StickS3 path samples the ES8311 on I2S1, right slot, with
    // MCLK = 128 * fs. Using the left slot gives a perfectly valid but silent
    // DMA stream on this board.
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
    std_cfg.clk_cfg.mclk_multiple = AUDIO_MCLK_MULTIPLE;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_i2s_rx, &std_cfg),
                        TAG, "i2s_channel_init_std_mode");
    // Enable the channel NOW (not later in codec_open) so MCLK starts
    // toggling on GPIO18 — ES8311 doesn't ACK on I2C without MCLK to
    // drive its internal logic and we'd otherwise see "Fail to read
    // from dev 30" on every codec_init probe.
    return i2s_channel_enable(g_i2s_rx);
}

// =========================================================================
// ES8311 codec — register init via esp_codec_dev
// =========================================================================
static esp_err_t codec_init(void)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port    = I2C_PORT,
        .addr    = ES8311_CODEC_DEFAULT_ADDR,    // 7-bit addr 0x18 shifted
        .bus_handle = g_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "i2c ctrl_if");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = I2S_PORT,
        .rx_handle = g_i2s_rx,
        .tx_handle = NULL,
        .clk_src   = I2S_CLK_SRC_PLL_160M,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "i2s data_if");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if    = ctrl_if,
        .gpio_if    = NULL,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,   // ADC = mic in only
        .pa_pin     = -1,
        .use_mclk   = true,
        .digital_mic = false,
        .mclk_div   = AUDIO_MCLK_DIV,
        // CRITICAL: in mic-only builds the DAC isn't running, so the default
        // "ADCL + DACR" internal reference (es8311.c writes 0x58 to REG44)
        // leaves the ADC referenced to a dead DAC right channel — every
        // sample comes back as 0x0000 even though the codec ACKs and the
        // I2S DMA is healthy. no_dac_ref=true switches REG44 to 0x08 which
        // uses the codec's standalone ADC reference instead.
        .no_dac_ref = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311_codec_new");

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if  = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
    };
    g_codec_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(g_codec_dev, ESP_FAIL, TAG, "esp_codec_dev_new");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = AUDIO_SAMPLE_RATE,
        .channel         = 2,
        .channel_mask    = AUDIO_INPUT_MASK,
        .bits_per_sample = AUDIO_BITS,
        .mclk_multiple   = AUDIO_MCLK_MULTIPLE,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(g_codec_dev, &fs),
                        TAG, "codec open");

    // Mirror M5Unified's _microphone_enabled_cb_sticks3 ES8311 register set
    // verbatim. esp_codec_dev's es8311 driver does most of these on open(),
    // but its 0x01 default uses non-inverted BCLK and on this hardware the
    // ADC then samples on the wrong edge and reads constant zero.
    //
    //   0x00 = 0x80  RESET / CSM POWER ON
    //   0x01 = 0xBA  CLOCK_MANAGER, MCLK=BCLK, BCLK_INV (mic-specific!)
    //   0x02 = 0x18  CLOCK_MANAGER, MULT_PRE=3
    //   0x0D = 0x01  SYSTEM, power up analog circuitry
    //   0x0E = 0x02  SYSTEM, enable analog PGA + ADC modulator
    //   0x14 = 0x10  ADC_REG14, select Mic1p-Mic1n single-ended-diff input.
    //                NOTE: PGA *gain* lives at 0x16, not 0x14. 0x10 here just
    //                turns the analog PGA on with mic1p/n input selected.
    //   0x16 = 0x07  ADC_REG16, mic PGA gain step (7 = +42 dB max).
    //   0x17 = 0xFF  ADC_VOLUME, max digital gain
    //   0x1C = 0x6A  ADC, equalizer bypass + DC offset cancel
    esp_codec_dev_write_reg(g_codec_dev, 0x00, 0x80);
    esp_codec_dev_write_reg(g_codec_dev, 0x01, 0xBA);
    esp_codec_dev_write_reg(g_codec_dev, 0x02, 0x18);
    esp_codec_dev_write_reg(g_codec_dev, 0x0D, 0x01);
    esp_codec_dev_write_reg(g_codec_dev, 0x0E, 0x02);
    esp_codec_dev_write_reg(g_codec_dev, 0x14, 0x10);
    esp_codec_dev_write_reg(g_codec_dev, 0x16, 0x07);
    esp_codec_dev_write_reg(g_codec_dev, 0x17, 0xFF);
    esp_codec_dev_write_reg(g_codec_dev, 0x1C, 0x6A);

    g_codec_ready = true;
    return ESP_OK;
}

// =========================================================================
// BMI270 IMU — orientation-based mic auto-mute
//
// Flat on table  -> mic muted (privacy: stick can't pick up nearby talk)
// Picked up      -> mic live
//
// 6-axis IMU on the same internal I2C bus as the codec / PMIC, address 0x68.
// We use Bosch's official driver because the chip refuses to deliver
// readings without a ~8 KB feature config blob the driver loads at init.
// We only ever read the accelerometer; gyro / step counter / wrist gestures
// stay disabled to keep average current low.
// =========================================================================
#define IMU_I2C_ADDR             BMI2_I2C_PRIM_ADDR    // 0x68
#define IMU_I2C_SPEED_HZ         400000
#define IMU_TX_BUF_LEN           64                    // > read_write_len + 1
// |Z accel| above this fraction of 1 g => "lying flat", mute.
//   16384 LSB = 1 g at ±2 g range, 16-bit. 0.7 g ~= 11500.
#define IMU_FLAT_THRESHOLD_LSB   11500
// Hysteresis to prevent chatter when the stick is at 45° on a stack of paper.
#define IMU_LIVE_THRESHOLD_LSB   9500
#define IMU_POLL_MS              200

static struct bmi2_dev          g_bmi;
static i2c_master_dev_handle_t  g_imu_dev = NULL;
static uint8_t                  g_imu_tx_buf[IMU_TX_BUF_LEN];

static BMI2_INTF_RETURN_TYPE imu_i2c_read(uint8_t reg_addr, uint8_t *reg_data,
                                          uint32_t len, void *intf_ptr)
{
    (void)intf_ptr;
    if (!g_imu_dev) return BMI2_E_COM_FAIL;
    esp_err_t err = i2c_master_transmit_receive(g_imu_dev, &reg_addr, 1,
                                                reg_data, len, 100);
    return (err == ESP_OK) ? BMI2_OK : BMI2_E_COM_FAIL;
}

static BMI2_INTF_RETURN_TYPE imu_i2c_write(uint8_t reg_addr,
                                           const uint8_t *reg_data,
                                           uint32_t len, void *intf_ptr)
{
    (void)intf_ptr;
    if (!g_imu_dev) return BMI2_E_COM_FAIL;
    if (len + 1 > sizeof(g_imu_tx_buf)) return BMI2_E_COM_FAIL;
    g_imu_tx_buf[0] = reg_addr;
    memcpy(g_imu_tx_buf + 1, reg_data, len);
    esp_err_t err = i2c_master_transmit(g_imu_dev, g_imu_tx_buf, len + 1, 200);
    return (err == ESP_OK) ? BMI2_OK : BMI2_E_COM_FAIL;
}

static void imu_delay_us(uint32_t period_us, void *intf_ptr)
{
    (void)intf_ptr;
    if (period_us >= 1000) {
        vTaskDelay(pdMS_TO_TICKS(period_us / 1000));
    } else {
        esp_rom_delay_us(period_us);
    }
}

static esp_err_t imu_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = IMU_I2C_ADDR,
        .scl_speed_hz    = IMU_I2C_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &g_imu_dev),
                        TAG, "imu add_device");

    static uint8_t addr = IMU_I2C_ADDR;
    g_bmi.intf           = BMI2_I2C_INTF;
    g_bmi.intf_ptr       = &addr;
    g_bmi.read           = imu_i2c_read;
    g_bmi.write          = imu_i2c_write;
    g_bmi.delay_us       = imu_delay_us;
    g_bmi.read_write_len = IMU_TX_BUF_LEN - 1;

    int8_t r = bmi270_init(&g_bmi);
    if (r != BMI2_OK) {
        ESP_LOGW(TAG, "bmi270_init failed: %d", r);
        return ESP_FAIL;
    }

    struct bmi2_sens_config conf = { .type = BMI2_ACCEL };
    if (bmi2_get_sensor_config(&conf, 1, &g_bmi) != BMI2_OK) return ESP_FAIL;
    conf.cfg.acc.odr         = BMI2_ACC_ODR_50HZ;        // plenty for 5 Hz check
    conf.cfg.acc.range       = BMI2_ACC_RANGE_2G;
    conf.cfg.acc.bwp         = BMI2_ACC_NORMAL_AVG4;
    conf.cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
    if (bmi2_set_sensor_config(&conf, 1, &g_bmi) != BMI2_OK) return ESP_FAIL;

    uint8_t sens_list[1] = { BMI2_ACCEL };
    if (bmi2_sensor_enable(sens_list, 1, &g_bmi) != BMI2_OK) return ESP_FAIL;

    ESP_LOGI(TAG, "bmi270 ready (accel @50 Hz, ±2 g)");
    return ESP_OK;
}

static void imu_task(void *arg)
{
    struct bmi2_sens_data data = {0};
    bool prev_flat = false;
    while (1) {
        if (bmi2_get_sensor_data(&data, &g_bmi) == BMI2_OK &&
            (data.status & BMI2_DRDY_ACC)) {
            // Convention: stick lying flat (face up or face down) on a
            // table -> |z| dominates. Pick it up to portrait or any
            // tilted orientation -> |z| drops, |x|/|y| dominate.
            int16_t z = data.acc.z;
            int32_t az = (z < 0) ? -(int32_t)z : (int32_t)z;
            bool flat;
            if (prev_flat) {
                flat = (az > IMU_LIVE_THRESHOLD_LSB);
            } else {
                flat = (az > IMU_FLAT_THRESHOLD_LSB);
            }
            if (flat != prev_flat) {
                prev_flat = flat;
                g_imu_mute = flat;
                bool flat_mute_enabled = vox_config_flat_mute_enabled();
                ESP_LOGI(TAG, "imu: %s (|z|=%ld lsb, flat_mute=%s)",
                         flat ? "flat" : "upright",
                         (long)az,
                         flat_mute_enabled ? "on" : "off");
                // Refresh the LCD when not in VAD-display mode so the
                // user has visual confirmation of the mute state.
                if (!g_vad_display_enabled) {
                    if (flat && flat_mute_enabled) {
                        lcd_status(COL_DIM_RED);
                    } else {
                        lcd_status(g_codec_ready ? COL_GREEN : COL_RED);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_POLL_MS));
    }
}

// =========================================================================
// USB UAC callbacks — host pulls bytes from us via input_cb
// =========================================================================
static void fill_codec_fail_diag_tone(uint8_t *buf, size_t len)
{
#if VOX_DIAG_CODEC_FAIL_TONE
    int16_t *samples = (int16_t *)buf;
    size_t count = len / sizeof(int16_t);
    for (size_t i = 0; i < count; i++) {
        // 1 kHz square wave at 16 kHz sample rate: 8 samples high, 8 low.
        samples[i] = ((g_diag_tone_phase++ & 0x0f) < 8) ? 4000 : -4000;
    }
    if (len & 1) {
        buf[len - 1] = 0;
    }
#else
    memset(buf, 0, len);
#endif
}

static void vad_update_from_pcm(uint8_t const *buf, size_t len, bool valid_audio)
{
    if (!valid_audio || buf == NULL || len < sizeof(int16_t)) {
        g_vad_level = (g_vad_level * 3U) / 4U;
        if (g_vad_level < VAD_OFF_LEVEL) {
            g_vad_active = false;
        }
        return;
    }

    int16_t const *samples = (int16_t const *)buf;
    size_t count = len / sizeof(int16_t);
    uint64_t sum_abs = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t v = samples[i];
        sum_abs += (uint32_t)(v < 0 ? -v : v);
    }

    uint32_t avg = (uint32_t)(sum_abs / count);
    uint32_t smoothed = (g_vad_level * 3U + avg) / 4U;
    g_vad_level = smoothed;

    if (g_vad_active) {
        g_vad_active = smoothed >= VAD_OFF_LEVEL;
    } else {
        g_vad_active = smoothed >= VAD_ON_LEVEL;
    }
}

static esp_err_t uac_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *arg)
{
    // Two independent mute paths feed the same silence-the-stream branch:
    //   g_uac_muted - host-side mute (UAC volume control says mic off)
    //   g_imu_mute  - physical orientation mute (stick lying flat on table)
    if (effective_mic_muted()) {
        memset(buf, 0, len);
        vad_update_from_pcm(NULL, 0, false);
        *bytes_read = len;
        return ESP_OK;
    }
    if (!g_codec_ready) {
        fill_codec_fail_diag_tone(buf, len);
        vad_update_from_pcm(buf, len, VOX_DIAG_CODEC_FAIL_TONE != 0);
        *bytes_read = len;
        return ESP_OK;
    }
    esp_err_t ret = esp_codec_dev_read(g_codec_dev, buf, len);
    if (ret == ESP_OK) {
        // Software make-up gain: ES8311 max analog PGA + max digital still
        // peaks around -38 dB on the StickS3 MEMS mic, which whisper.cpp /
        // VoiceInk struggle to transcribe. 8x = +18 dB lifts speech to
        // ~ -20 dB peak, comfortably above ASR's threshold. Saturating
        // clamp at int16 range avoids wraparound on loud claps.
        int16_t *s = (int16_t *)buf;
        size_t cnt = len / sizeof(int16_t);
        for (size_t i = 0; i < cnt; i++) {
            int32_t v = (int32_t)s[i] * 8;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            s[i] = (int16_t)v;
        }
    }
    vad_update_from_pcm(buf, len, ret == ESP_OK);
    *bytes_read = (ret == ESP_OK) ? len : 0;
    return ret;
}

static esp_err_t uac_output_cb(uint8_t *buf, size_t len, void *arg)
{
    // No speaker in v1 — host shouldn't even see an output stream because
    // CONFIG_UAC_MIC_*-only is configured, but be safe.
    return ESP_OK;
}

static void uac_set_mute_cb(uint32_t mute, void *arg)
{
    g_uac_muted = !!mute;
    ESP_LOGI(TAG, "host mute=%d", (int)mute);
}

static void uac_set_volume_cb(uint32_t volume, void *arg)
{
    // _volume = (volume_db + 50) * 2  -> volume_db
    int volume_db = (int)volume / 2 - 50;
    // Map volume_db to ES8311 in_gain (0..40 dB roughly). Below -10 dB just
    // ride at minimum; whisper.cpp won't transcribe useful audio quieter.
    float gain = 30.0f + (float)volume_db * 0.5f;
    if (gain < 0.0f)  gain = 0.0f;
    if (gain > 40.0f) gain = 40.0f;
    if (g_codec_dev) {
        esp_codec_dev_set_in_gain(g_codec_dev, gain);
    }
    ESP_LOGI(TAG, "host volume=%d dB -> mic gain=%.1f", volume_db, gain);
}

// Interface numbers must match those in usb_descriptors.c. usb_device_uac
// needs to know which streaming interface(s) it owns so it can answer
// SET_INTERFACE / GET_CUR / etc. on the right ITF.
#define UAC_ITF_NUM_MIC   1
#define UAC_ITF_NUM_SPK   -1   // mic-only build

// Force the linker to pull usb_descriptors.c.obj out of libmain.a. tinyusb
// references these symbols from inside its own static archive, so without
// at least one reference from a non-archive object the descriptor file
// silently gets dropped. ESP-IDF's main isn't built with --whole-archive.
extern uint8_t const *tud_descriptor_device_cb(void);
extern uint8_t const *tud_descriptor_configuration_cb(uint8_t index);
extern void voxstick_force_link_usb_recovery(void);
__attribute__((used)) static const void *const _link_usb_descriptors[] = {
    (const void *)tud_descriptor_device_cb,
    (const void *)tud_descriptor_configuration_cb,
    (const void *)voxstick_force_link_usb_recovery,
};

static esp_err_t uac_init(void)
{
    uac_device_config_t cfg = {
        .output_cb     = uac_output_cb,
        .input_cb      = uac_input_cb,
        .set_mute_cb   = uac_set_mute_cb,
        .set_volume_cb = uac_set_volume_cb,
        .cb_ctx        = NULL,
        .spk_itf_num   = UAC_ITF_NUM_SPK,
        .mic_itf_num   = UAC_ITF_NUM_MIC,
    };
    return uac_device_init(&cfg);
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == VENDOR_DOWNLOAD_REQUEST) {
        ESP_LOGW(TAG, "USB vendor download magic");
        request_rom_download("USB vendor magic requested ROM download", 150);
        return tud_control_status(rhport, request);
    }

    return false;
}

// =========================================================================
// Buttons — BtnA only. Tap and long-press output are configurable through
// NVS. Defaults are Left Ctrl+F12 for WeType voice toggle and Enter for send.
//
// USB HID Keyboard/Keypad usage page (0x07):
//   F13 = 0x68 .. F24 = 0x73   (TinyUSB also exposes HID_KEY_F13..F24)
//
// Debounce: a level transition must persist for 2 polls (= 20 ms at the
// 10 ms tick) before we commit it. Stops jitter from re-triggering PTT.
// =========================================================================
#define BTN_POLL_MS         10
#define BTN_DEBOUNCE_TICKS  2

#define HID_REPORT_ID_KBD   1
#define HID_DOWNLOAD_LED_MAGIC       0x1F
#define HID_DOWNLOAD_MAGIC_REPEATS   3
#define HID_DOWNLOAD_MAGIC_WINDOW_MS DOWNLOAD_MAGIC_WINDOW_MS

// Host-side recovery path: the keyboard HID descriptor includes the standard
// 1-byte LED Output report. Normal OS LED updates are values like CapsLock;
// our tooling sends all five LED bits (0x1f) three times in a short window.
// That gives us a no-button way to put UAC/HID firmware back into ROM
// download mode while keeping accidental keyboard LED updates harmless.
void voxstick_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                                   hid_report_type_t report_type,
                                   uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    if (report_type != HID_REPORT_TYPE_OUTPUT || buffer == NULL || bufsize == 0) {
        return;
    }

    uint8_t led_bits = 0;
    bool has_led_bits = false;

    if (report_id == HID_REPORT_ID_KBD) {
        led_bits = buffer[0];
        has_led_bits = true;
    } else if (bufsize >= 2 && buffer[0] == HID_REPORT_ID_KBD) {
        // Some host stacks include the report ID as byte 0 even though TinyUSB
        // also passes it separately. Accept both forms to keep the recovery
        // tool boring.
        led_bits = buffer[1];
        has_led_bits = true;
    }

    if (!has_led_bits) {
        return;
    }

    static uint8_t magic_seen = 0;
    static TickType_t last_magic_tick = 0;
    TickType_t now = xTaskGetTickCount();

    if ((led_bits & HID_DOWNLOAD_LED_MAGIC) == HID_DOWNLOAD_LED_MAGIC) {
        if (last_magic_tick == 0 ||
            now - last_magic_tick > pdMS_TO_TICKS(HID_DOWNLOAD_MAGIC_WINDOW_MS)) {
            magic_seen = 0;
        }
        last_magic_tick = now;
        magic_seen++;
        ESP_LOGW(TAG, "HID download magic %u/%u",
                 magic_seen, HID_DOWNLOAD_MAGIC_REPEATS);
        if (magic_seen >= HID_DOWNLOAD_MAGIC_REPEATS) {
            magic_seen = 0;
            request_rom_download("HID LED magic requested ROM download", 150);
        }
    } else {
        magic_seen = 0;
        last_magic_tick = now;
    }
}

static void buttons_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask  = (1ULL << BTN_A_GPIO),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

// Boot-time gesture: hold BtnA + tap side Reset → app starts → app sees
// BtnA still held → reboot to ROM download mode. Software-driven so the
// M5StickS3 PMIC's BOOT-pin latch (triggered by long-pressing Reset) is
// never involved — that latch survives reset and bricks the stick until
// the battery drains. esptool clears FORCE_DOWNLOAD_BOOT on the next
// flash so we never get stuck a second time.
//
// During runtime BtnA has no time-out: it's the PTT key for VoiceInk
// and gets held for as long as the user wants to dictate.
static void check_boot_button_for_download(void)
{
    gpio_reset_pin(BTN_A_GPIO);
    gpio_set_direction(BTN_A_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_A_GPIO, GPIO_PULLUP_ONLY);
    // Internal pull-up needs a tick or two of settle before we sample.
    vTaskDelay(pdMS_TO_TICKS(20));

    if (gpio_get_level(BTN_A_GPIO) != 0) {
        return;     // BtnA not held → continue normal startup
    }
    enter_rom_download_mode("BtnA held at boot");
}

// Tap-vs-long-press disambiguator for BtnA.
// We only emit the HID report on RELEASE, after we know which it was.
// While the button is held the LCD shifts colour to give feedback:
//   before long threshold: magenta (still in tap territory)
//   after  long threshold: blue    (long-press latched)
static void hid_send_tap(uint8_t modifier, uint8_t keycode)
{
    if (keycode == 0) return;
    if (!tud_hid_ready()) return;
    uint8_t down[6] = { keycode };
    uint8_t up  [6] = { 0 };
    tud_hid_keyboard_report(HID_REPORT_ID_KBD, modifier, down);
    vTaskDelay(pdMS_TO_TICKS(15));
    tud_hid_keyboard_report(HID_REPORT_ID_KBD, 0, up);
}

static void button_task(void *arg)
{
    bool a_committed = true;     // released = high
    bool a_pending   = true;
    int  a_stable    = 0;
    TickType_t a_press_start = 0;
    vox_config_wire_t a_press_cfg = {0};
    bool       a_long_latched = false;  // true once the press has crossed
                                        // the long threshold this hold

    while (1) {
        bool a = gpio_get_level(BTN_A_GPIO);

        if (a != a_pending) { a_pending = a; a_stable = 0; }
        else if (a_stable < BTN_DEBOUNCE_TICKS) { a_stable++; }
        else if (a_pending != a_committed) {
            a_committed = a_pending;
            if (!a_committed) {
                // Just pressed.
                a_press_start = xTaskGetTickCount();
                vox_config_get(&a_press_cfg);
                a_long_latched = false;
                ESP_LOGI(TAG, "btn A down");
                if (!g_vad_display_enabled) lcd_status(COL_MAGENTA);
            } else {
                // Just released — fire the right tap.
                TickType_t held = xTaskGetTickCount() - a_press_start;
                bool is_long = pdTICKS_TO_MS(held) >= a_press_cfg.long_press_ms;
                uint8_t modifier = is_long ?
                    a_press_cfg.hold_modifier : a_press_cfg.tap_modifier;
                uint8_t keycode = is_long ?
                    a_press_cfg.hold_keycode : a_press_cfg.tap_keycode;
                ESP_LOGI(TAG, "btn A up after %lu ms (%s 0x%02x+0x%02x)",
                         (unsigned long)pdTICKS_TO_MS(held),
                         is_long ? "long" : "tap",
                         modifier,
                         keycode);
                hid_send_tap(modifier, keycode);
                if (!g_vad_display_enabled) {
                    lcd_status(g_codec_ready ? COL_GREEN : COL_RED);
                }
            }
        }

        // While still held, switch to blue once the long-press threshold is
        // crossed so the user gets visual confirmation that releasing now
        // will send Enter rather than toggle voice.
        if (!a_committed && !a_long_latched) {
            TickType_t held = xTaskGetTickCount() - a_press_start;
            if (pdTICKS_TO_MS(held) >= a_press_cfg.long_press_ms) {
                a_long_latched = true;
                if (!g_vad_display_enabled) lcd_status(COL_BLUE);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

// =========================================================================
// app_main
// =========================================================================
void app_main(void)
{
    // Recovery path: holding BtnA at boot reroutes us to ROM download
    // mode without touching the PMIC. Must run before any USB OTG init
    // so esptool can talk to the chip on /dev/cu.usbmodem*.
    check_boot_button_for_download();

    ESP_LOGI(TAG, "voxstick boot — fw v0.1.6");

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs init failed: %s; using volatile defaults",
                 esp_err_to_name(nvs_ret));
    }
    vox_config_init();

    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_LOGI(TAG, "i2c0 up (SDA=%d SCL=%d)", I2C_SDA_PIN, I2C_SCL_PIN);

    // PMIC must come up before LCD/codec — otherwise the 3.3 V peripheral
    // rail can be off and both the screen and ES8311 stay dark.
    if (pmic_init() != ESP_OK) {
        ESP_LOGW(TAG, "pmic init failed — codec may not respond");
    }

    // LCD after PMIC/LDO so a corrupted PMIC state can be repaired before
    // we try to talk to the panel.
    esp_err_t lcd_ret = lcd_init();
    if (lcd_ret == ESP_OK) {
        lcd_status(COL_WHITE);
    } else {
        ESP_LOGE(TAG, "lcd_init failed: %s", esp_err_to_name(lcd_ret));
    }

    ESP_ERROR_CHECK(i2s_rx_init());
    lcd_status(COL_YELLOW);
    ESP_LOGI(TAG, "i2s%d rx up (MCLK=%d BCLK=%d WS=%d DIN=%d right-slot mclk=%dx)",
             I2S_PORT, I2S_MCLK_PIN, I2S_BCLK_PIN, I2S_WS_PIN, I2S_DIN_PIN,
             AUDIO_MCLK_DIV);

    if (codec_init() != ESP_OK) {
        // Don't panic-loop: keep going so USB UAC + HID still come up and
        // we can use the rest of the device while we debug the codec.
        // Mic stream will deliver silence in this state.
        lcd_status(COL_RED);
        ESP_LOGW(TAG, "codec_init failed — continuing without mic audio");
    } else {
        lcd_status(COL_ORANGE);
        ESP_LOGI(TAG, "es8311 ready @ %d Hz / %d ch / %d bit",
                 AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS);
    }

#if VOX_DEBUG_NO_UAC
    // Debug build: skip TinyUSB / UAC init so the chip's built-in
    // USB-Serial/JTAG console stays connected and ESP_LOG output is
    // visible on /dev/cu.usbmodem*. No mic + no HID until we flash a
    // production build, but we get to see why the codec is failing.
    ESP_LOGW(TAG, "DEBUG build: UAC + HID skipped, USB-Serial/JTAG kept alive");
    lcd_status(COL_MAGENTA);
#else
    ESP_ERROR_CHECK(uac_init());
    lcd_status(COL_CYAN);
    ESP_LOGI(TAG, "uac+hid composite advertised as 'StickS3-Mic'");
#endif

    buttons_init();
    xTaskCreate(button_task, "btn", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "buttons up (BtnA=GPIO%d)", BTN_A_GPIO);

    // BMI270 IMU for orientation-based mic auto-mute. Best-effort: if init
    // fails (chip absent / I2C glitch / blob load failure) we just skip the
    // task — the rest of the device keeps working as a normal UAC mic.
    if (imu_init() == ESP_OK) {
        xTaskCreate(imu_task, "imu", 4096, NULL, 4, NULL);
    } else {
        ESP_LOGW(TAG, "imu init failed — flat-detect mute disabled");
    }

#if !VOX_DEBUG_NO_UAC
    lcd_draw_mic_status(0, false, g_codec_ready, effective_mic_muted());
    xTaskCreate(vad_lcd_task, "vad_lcd", 3072, NULL, 4, NULL);
#endif

#if VOX_AUTO_DOWNLOAD_AFTER_SEC > 0
    request_rom_download("firmware auto-download timeout",
                         VOX_AUTO_DOWNLOAD_AFTER_SEC * 1000UL);
#endif

#if VOX_CODEC_FAIL_DOWNLOAD_AFTER_SEC > 0
    if (!g_codec_ready) {
        request_rom_download("codec failed; auto-download timeout",
                             VOX_CODEC_FAIL_DOWNLOAD_AFTER_SEC * 1000UL);
    }
#endif

    // Idle — UAC + I2S DMA + button task all run in their own tasks.
    // In VOX_DEBUG_NO_UAC builds we ESP_LOGI diagnostic state every
    // tick so it's visible on USB-Serial/JTAG even if cat starts late.
    int n = 0;
    while (1) {
#if VOX_DEBUG_NO_UAC
        ESP_LOGI(TAG, "----- tick %d -----", n++);
        ESP_LOGI(TAG, "pmic_addr_found=0x%02x", g_pmic_addr);
        i2c_scan();
        vTaskDelay(pdMS_TO_TICKS(3000));
#else
        (void)n;
        vTaskDelay(pdMS_TO_TICKS(5000));
#endif
    }
}

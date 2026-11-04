#pragma once
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
#include "freertos/semphr.h"
#include "esp_log.h"
#include "zh_font.h"
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
#include "esp_private/panic_internal.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h"
#include "nvs_flash.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "bmi270.h"
#include "bmi2_defs.h"
#include "vox_config.h"
#include "ble_presenter.h"
#include "btmic.h"
#include "wifimic.h"
#include "launcher.h"
#include "zh_font.h"
#include "ui.h"

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

static const char *TAG __attribute__((unused)) = "voxstick";

// ---- Board pin map -------------------------------------------------------
#define I2C_PORT      I2C_NUM_0
#define I2C_SDA_PIN   47
#define I2C_SCL_PIN   48

#define I2S_PORT      I2S_NUM_1
#define I2S_MCLK_PIN  18
#define I2S_BCLK_PIN  17
#define I2S_WS_PIN    15
#define I2S_DIN_PIN   16
#define I2S_DOUT_PIN  14

// StickS3 face button. BtnA = front, big M5 logo. BtnB header is unpopulated
// on this hardware revision so we don't bother polling it.
#define BTN_A_GPIO    GPIO_NUM_11
// Side button (KEY2 per M5 docs, G12). Used only in presenter mode: previous
// slide. Ignored in dictation mode so a stray press can never type anything.
#define BTN_B_GPIO    GPIO_NUM_12

// ---- LCD (ST7789P3 135x240, SPI3) ---------------------------------------
// Numbers from the M5StickS3 datasheet / weclawbot-base reference.
#define LCD_HOST          SPI3_HOST
#define LCD_BL_PIN        38
#define LCD_RST_PIN       21
#define LCD_DC_PIN        45
#define LCD_CS_PIN        41
#define LCD_SCK_PIN       40
#define LCD_MOSI_PIN      39
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


// ---- shared between modules (generated by the stage-1 split) ----
#define BATT_LOW_PCT   15
#define HID_KEY_RIGHT_ARROW 0x4F
#define HID_KEY_LEFT_ARROW  0x50

void app_main(void);
extern vox_config_wire_t g_press_cfg;
extern const app_t app_timer;
void codec_mic_regs(void);
extern volatile int g_beep_dbg;   // 0 never, 1 codec not ready, 2 tx channel failed, 3 enable failed, 4 played
void beep_play(int count);        // short beeps on the speaker (runs in its own task)
void pmic_speaker(bool on);       // speaker amplifier enable
extern volatile uint32_t g_imu_flips;
extern const app_t app_slide;
const app_t *launcher_app(int i);
int          launcher_count(void);
esp_err_t i2s_rx_init(void);
esp_err_t codec_init(void);
void buttons_init(void);
void check_boot_button_for_download(void);
void hid_send_tap(uint8_t modifier, uint8_t keycode);
void button_task(void *arg);
void icon_diag(int x, int y, uint16_t fg, uint16_t bg);
uint32_t diag_tick(bool first);
void diag_front_up(uint32_t held_ms, bool is_long);
void diag_front_down(void);
void vad_lcd_task(void *arg);
void enter_rom_download_mode(const char *reason);
bool host_download_allowed(void);
void request_rom_download(const char *reason, uint32_t delay_ms);
extern i2c_master_bus_handle_t  g_i2c_bus;
extern i2s_chan_handle_t        g_i2s_rx;
extern esp_codec_dev_handle_t   g_codec_dev;
extern volatile bool            g_codec_ready;
extern volatile bool            g_uac_muted;
extern volatile bool            g_download_scheduled;
extern TaskHandle_t             g_download_task_handle;
extern const char              *g_download_reason;
extern uint32_t                 g_download_delay_ms;
extern uint32_t                 g_diag_tone_phase;
extern volatile uint32_t        g_vad_level;
extern volatile bool            g_vad_active;
extern volatile bool            g_vad_display_enabled;
extern volatile bool            g_imu_mute;
extern volatile uint32_t vox_panic_magic;
extern volatile uint32_t vox_panic_pc;
extern volatile uint32_t vox_panic_exc;
extern volatile uint8_t  vox_where_disp;
extern volatile uint8_t  vox_where_btn;
extern volatile uint32_t vox_stall_magic;
extern volatile char     vox_holder[5];
extern volatile uint8_t  vox_stuck_mask;
extern volatile uint32_t g_hb_disp;
extern volatile uint32_t g_hb_btn;
extern volatile int g_hb_stuck_max[3];
extern TaskHandle_t g_disp_task;
extern const app_t *volatile g_cur;
extern volatile TickType_t g_last_activity;
extern volatile int        g_menu_sel;
void enter_app(const app_t *a);
bool effective_mic_muted(void);
esp_err_t imu_init(void);
void imu_task(void *arg);
extern esp_lcd_panel_handle_t g_lcd;
extern volatile uint16_t vox_where_lcd;
extern volatile uint32_t g_lcd_contended;
extern volatile uint32_t g_lcd_wait_max_ms;
esp_err_t lcd_init(void);
void lcd_fill_rect(int x0, int y0, int x1, int y1, uint16_t rgb565);
extern volatile bool g_bar_dirty;
void lcd_clear(uint16_t rgb565);
void lcd_status(uint16_t rgb565);
void lcd_draw_thick_line(int x0, int y0, int x1, int y1, int thickness, uint16_t rgb565);
void lcd_draw_mic_status(uint32_t level, bool active, bool codec_ready, bool muted);
extern volatile TickType_t g_passkey_hold_until;
extern StaticSemaphore_t g_pres_lcd_mu_buf;
extern SemaphoreHandle_t g_pres_lcd_mu;
int lcd_text_width(const char *s, int scale);
int zh_text_width(const char *s, int scale);
int zh_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
void zh_center(int y, const char *s, uint16_t fg, int scale);
void lcd_draw_text(int x, int y, const char *s, int scale, uint16_t col);
void monitor_task(void *arg);
esp_err_t i2c_bus_init(void);
extern uint8_t g_pmic_addr;
void battery_sample(void);
void i2c_scan(void);
esp_err_t pmic_init(void);
void lcd_draw_menu(int sel, int prev);
void lcd_clear_locked(void);
void lcd_draw_btrec(int ui, bool note);
void lcd_draw_wifimic(int ui);
void lcd_draw_presenter(void);
extern int           g_batt_pct;
extern volatile TickType_t g_batt_good_tick;
bool battery_is_estimate(void);
void pmic_regs_text(char *buf, size_t n);
extern bool          g_batt_ext;
extern int g_fault_reason;
extern uint32_t g_fault_crumb;
extern char g_fault_who[20];
extern volatile int g_batt_mv;
extern volatile int g_batt_dbg;
void statusbar_paint_if_dirty(void);
void statusbar_tick(void);
void lcd_fill_circle(int cx, int cy, int r, uint16_t c);
void lcd_icon_mic(int x, int y, uint16_t c);
void lcd_icon_record(int x, int y, uint16_t c, uint16_t bg);
void lcd_icon_slide(int x, int y, uint16_t c);
int btmic_pcm_read(int16_t *dst, int n);
esp_err_t uac_init(void);

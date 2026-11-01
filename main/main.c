#include "vox_internal.h"

// ---- Globals -------------------------------------------------------------
i2c_master_bus_handle_t  g_i2c_bus     = NULL;
i2s_chan_handle_t        g_i2s_rx      = NULL;
esp_codec_dev_handle_t   g_codec_dev   = NULL;
volatile bool            g_codec_ready = false;
volatile bool            g_uac_muted   = false;
volatile bool            g_download_scheduled = false;
TaskHandle_t             g_download_task_handle = NULL;
const char              *g_download_reason = "download requested";
uint32_t                 g_download_delay_ms = 0;
uint32_t                 g_diag_tone_phase = 0;
volatile uint32_t        g_vad_level = 0;
volatile bool            g_vad_active = false;
volatile bool            g_vad_display_enabled = false;
// IMU orientation-based mute: true when the BMI270 reports the stick is
// lying flat. ORed into the audio path so flat-on-table = silence,
// pick-it-up = live mic. Independent of g_uac_muted (host volume mute)
// and g_codec_ready (codec init failure).
volatile bool            g_imu_mute  = false;

// Presenter mode: button sends slide keys and the mic is forced silent.
// The device boots into the launcher menu. The side button (KEY2) moves the
// highlight and the front button (BtnA) opens the app; apps decide what the
// buttons do from then on (see launcher.h).

// Task monitor: each task bumps its counter every loop and records, in `where`,
// which call it is inside (0 = idle). A stall restarts the stick and the next
// boot shows the markers (see monitor_task).
#include "esp_attr.h"
RTC_NOINIT_ATTR volatile uint32_t vox_panic_magic;   // set by the panic hook below
RTC_NOINIT_ATTR volatile uint32_t vox_panic_pc;
RTC_NOINIT_ATTR volatile uint32_t vox_panic_exc;
RTC_NOINIT_ATTR volatile uint8_t  vox_where_disp;
RTC_NOINIT_ATTR volatile uint8_t  vox_where_btn;
RTC_NOINIT_ATTR volatile uint32_t vox_stall_magic;
RTC_NOINIT_ATTR volatile char     vox_holder[5];     // who held the LCD lock at the stall
RTC_NOINIT_ATTR volatile uint8_t  vox_stuck_mask;    // bit0 display, bit1 button, bit2 recording
volatile uint32_t g_hb_disp = 0;
volatile uint32_t g_hb_btn = 0;
volatile int g_hb_stuck_max[3];            // longest quiet spell (s) seen per watched task: display, button, recording
TaskHandle_t g_disp_task = NULL;          // the display task, for the markers in lcd_fill_rect

const app_t *volatile g_cur = NULL;    // NULL = the menu
volatile TickType_t g_last_activity = 0;   // buttons / UI changes; drives screen sleep
volatile int        g_menu_sel = 0;

void enter_app(const app_t *a)
{
    const app_t *old = g_cur;
    if (old && old->leave) old->leave();
    g_cur = a;
    g_last_activity = xTaskGetTickCount();
    uint32_t f = a ? a->flags : 0;
    btmic_set_mode((f & APP_SYNC) != 0, a == NULL || (f & APP_MAINT) != 0);
    ESP_LOGW(TAG, "app -> %s", a ? a->name : "MENU");
    if (a) {
        if (f & APP_BLE) ble_presenter_start_async();   // radio on only after an explicit choice
        if (a->enter) a->enter();
    }
}

bool effective_mic_muted(void)
{
    const app_t *a = g_cur;
    return !(a && (a->flags & APP_USB_MIC)) || g_uac_muted || (vox_config_flat_mute_enabled() && g_imu_mute);
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

    g_pres_lcd_mu = xSemaphoreCreateMutexStatic(&g_pres_lcd_mu_buf);   // before any task can draw
    buttons_init();
    {   // Abnormal reset? Remember why, and where the stick said it was working.
        esp_reset_reason_t rr = esp_reset_reason();
        if (rr == ESP_RST_SW && vox_stall_magic == 0xBEEF) {            // the monitor restarted us
            g_fault_reason = 50;                                          // our own code; 8 is ESP_RST_DEEPSLEEP
            snprintf(g_fault_who, sizeof(g_fault_who), "M%u H%c L%u", (unsigned)vox_stuck_mask, vox_holder[0] ? vox_holder[0] : 'N', (unsigned)vox_where_lcd);
            g_fault_crumb = (uint32_t)vox_where_disp * 10000 + (uint32_t)vox_where_btn * 1000 + (vox_crumb % 1000);
        } else if (rr == ESP_RST_WDT) {                                  // the hardware watchdog: whole chip was stuck
            g_fault_reason = (int)rr;
            g_fault_crumb = (uint32_t)vox_where_disp * 10000 + (uint32_t)vox_where_btn * 1000 + (vox_crumb % 1000);
            snprintf(g_fault_who, sizeof(g_fault_who), "HW HANG");
        } else if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
                   rr == ESP_RST_BROWNOUT) {
            g_fault_reason = (int)rr;
            g_fault_crumb = vox_crumb;
            if (vox_panic_magic == 0xFACE) snprintf(g_fault_who, sizeof(g_fault_who), "%08x E%u", (unsigned)vox_panic_pc, (unsigned)vox_panic_exc);
        }
        vox_panic_magic = 0;
        vox_stall_magic = 0;
        vox_crumb = 0;
    }
    btmic_init(btmic_pcm_read);
    btmic_set_mode(false, true);                  // the menu is up: housekeeping allowed
    g_last_activity = xTaskGetTickCount();
    xTaskCreate(button_task, "btn", 4096, NULL, 5, NULL);
    xTaskCreate(monitor_task, "mon", 3072, NULL, 7, NULL);
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
    xTaskCreate(vad_lcd_task, "vad_lcd", 3072, NULL, 6, NULL);   // above btmic/btn so uploads cannot starve the screen
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


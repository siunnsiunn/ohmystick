// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

// =========================================================================
// ROM download escape hatch
// =========================================================================
void enter_rom_download_mode(const char *reason)
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

// Host-initiated ROM download (vendor request / HID LED magic) is only honoured
// while BtnA is physically held, so a host process alone cannot force a reflash.
bool host_download_allowed(void)
{
    return gpio_get_level(BTN_A_GPIO) == 0;
}

void request_rom_download(const char *reason, uint32_t delay_ms)
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


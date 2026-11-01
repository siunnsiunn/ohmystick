// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

// Restarts the stick if the display, button or recording task stops reporting in
// (a deadlock or a hung call), after saving where each one was.
void monitor_task(void *arg)
{
    (void)arg;
    uint32_t last[3] = {0, 0, 0};
    int stuck[3] = {0, 0, 0};

    // Last line of defence: the RTC watchdog runs from its own clock, so it still
    // resets the chip when the whole system is stuck (a stalled flash operation, a
    // locked-up bus) and the software watchdogs cannot run. This task feeds it.
    wdt_hal_context_t rwdt = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_init(&rwdt, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&rwdt);
    wdt_hal_config_stage(&rwdt, WDT_STAGE0, (uint32_t)(8000ULL * rtc_clk_slow_freq_get_hz() / 1000ULL),
                         WDT_STAGE_ACTION_RESET_SYSTEM);   // RTC memory (our markers) survives this
    wdt_hal_enable(&rwdt);
    wdt_hal_write_protect_enable(&rwdt);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        wdt_hal_write_protect_disable(&rwdt);
        wdt_hal_feed(&rwdt);
        wdt_hal_write_protect_enable(&rwdt);
        uint32_t now[3] = { g_hb_disp, g_hb_btn, vox_hb_rec };
        bool dead = false;
        for (int i = 0; i < 3; i++) {
            stuck[i] = (now[i] == last[i]) ? stuck[i] + 1 : 0;
            if (i == 0 && !g_disp_task) stuck[0] = 0;        // no display task yet (or in a build without one)
            if (stuck[i] > g_hb_stuck_max[i]) g_hb_stuck_max[i] = stuck[i];
            last[i] = now[i];
            if (stuck[i] >= 6) dead = true;
        }
        if (dead) {
            TaskHandle_t h = g_pres_lcd_mu ? xSemaphoreGetMutexHolder(g_pres_lcd_mu) : NULL;
            const char *nm = h ? pcTaskGetName(h) : "";
            char c = !h ? 'N' : (h == g_disp_task ? 'D' : (strcmp(nm, "btn") == 0 ? 'B' :
                     (strcmp(nm, "btmic") == 0 ? 'R' : 'O')));      // none, display, button, recording, other
            vox_holder[0] = c; vox_holder[1] = 0;
            vox_stuck_mask = (stuck[0] >= 6) | ((stuck[1] >= 6) << 1) | ((stuck[2] >= 6) << 2);
            vox_stall_magic = 0xBEEF;
            // Restart first: a log line can block on the very lock the stuck task holds.
            esp_restart();
        }
    }
}

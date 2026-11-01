// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

// ---- Screen sleep: dim, then off, on the idle screens (menu, BT REC) -----------
#define SCREEN_DIM_MS   15000
#define SCREEN_OFF_MS   30000
#define BL_DUTY_DIM     ((LCD_BL_DUTY_MAX * 3U) / 100U)

static void backlight_set(uint32_t duty)
{
    ledc_set_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL);
}

// Apps flagged APP_NO_SLEEP and pairing screens never sleep. An app's
// activity_key changing (a recording started, a result arrived) wakes the screen.
static void screen_sleep_tick(const app_t *a)
{
    static int level = 2;                       // 2 full, 1 dim, 0 off
    static int last_key = -1;
    if (a && a->activity_key) {
        int key = a->activity_key();
        if (key != last_key) { last_key = key; g_last_activity = xTaskGetTickCount(); }
    } else {
        last_key = -1;
    }
    bool can_sleep = (a == NULL || !(a->flags & APP_NO_SLEEP)) && g_passkey_hold_until == 0;
    uint32_t idle = pdTICKS_TO_MS(xTaskGetTickCount() - g_last_activity);
    int want = !can_sleep ? 2 : (idle >= SCREEN_OFF_MS ? 0 : (idle >= SCREEN_DIM_MS ? 1 : 2));
    if (want == level) return;
    level = want;
    backlight_set(want == 2 ? LCD_BL_DUTY_READY : (want == 1 ? BL_DUTY_DIM : 0));
}

void vad_lcd_task(void *arg)
{
    (void)arg;
    g_disp_task = xTaskGetCurrentTaskHandle();
    g_vad_display_enabled = true;

    const app_t *last = (const app_t *)-1;      // forces the first paint
    int last_sel = -1;

    while (1) {
        const app_t *a = g_cur;
        int sel = g_menu_sel;
        g_hb_disp++;
        // A pairing code stays up until pairing ends. Only the two keyboard screens
        // clear it themselves; anywhere else it would hide the status bar and block
        // screen sleep for good.
        if (g_passkey_hold_until != 0 && a != &app_bt_dict && a != &app_slide) {
            int s = ble_presenter_state();
            if (s == 61 || s == 98 || (int32_t)(xTaskGetTickCount() - g_passkey_hold_until) >= 0) {
                g_passkey_hold_until = 0;
                lcd_clear_locked();
                last = (const app_t *)-1;           // repaint this screen from scratch
            }
        }
        vox_where_disp = 1; statusbar_tick();
        vox_where_disp = 2; screen_sleep_tick(a);
        vox_where_disp = 3;

        if (!a) {
            if (last != a || last_sel != sel) {
                int prev = (last == a) ? last_sel : -1;      // same screen: repaint only what changed
                last = a;
                last_sel = sel;
                lcd_draw_menu(sel, prev);
            }
            vox_where_disp = 0;
            vTaskDelay(pdMS_TO_TICKS(VAD_FRAME_MS));
            continue;
        }

        bool first = (last != a);
        if (first) {
            last = a;
            last_sel = -1;
            lcd_clear_locked();                 // drop the menu colours
        }
        vox_where_disp = 4;
        uint32_t wait = a->tick ? a->tick(first) : 0;
        vox_where_disp = 0;
        vTaskDelay(pdMS_TO_TICKS(wait ? wait : VAD_FRAME_MS));
    }
}


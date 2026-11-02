// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

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
// Hold this long (no HID sent) to switch dictation <-> presenter mode.
#define MODE_TOGGLE_MS      2500
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
            if (host_download_allowed()) {
                request_rom_download("HID LED magic requested ROM download", 150);
            } else {
                ESP_LOGW(TAG, "HID LED download magic ignored: BtnA not held");
            }
        }
    } else {
        magic_seen = 0;
        last_magic_tick = now;
    }
}

void buttons_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask  = (1ULL << BTN_A_GPIO) | (1ULL << BTN_B_GPIO),
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
void check_boot_button_for_download(void)
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
void hid_send_tap(uint8_t modifier, uint8_t keycode)
{
    if (keycode == 0) return;
    if (!tud_hid_ready()) return;
    uint8_t down[6] = { keycode };
    uint8_t up  [6] = { 0 };
    tud_hid_keyboard_report(HID_REPORT_ID_KBD, modifier, down);
    vTaskDelay(2);                                  // 20 ms: the host polls every 10 ms and must see the press
    // The release must not be lost, or the key sticks down and the host repeats it.
    for (int i = 0; i < 10; i++) {
        if (tud_hid_ready() && tud_hid_keyboard_report(HID_REPORT_ID_KBD, 0, up)) break;
        vTaskDelay(1);
    }
}


// A level change counts once it has held for BTN_DEBOUNCE_TICKS polls in a row.
typedef struct { bool committed, pending; int stable; } debounce_t;     // levels: true = released (pull-up)

// Returns true when the committed level just changed; the new level is in d->committed.
static bool debounce(debounce_t *d, bool level)
{
    if (level != d->pending) { d->pending = level; d->stable = 0; return false; }
    if (d->stable < BTN_DEBOUNCE_TICKS) { d->stable++; return false; }
    if (d->pending == d->committed) return false;
    d->committed = d->pending;
    return true;
}

// Side button: the menu's "next", otherwise the running app's side action (default: back to the menu).
static void side_button_pressed(void)
{
    const app_t *cur = g_cur;
    vox_where_btn = 3;
    if (!cur) {
        g_menu_sel = (g_menu_sel + 1) % launcher_count();
        ESP_LOGI(TAG, "menu selection -> %d", (int)g_menu_sel);
    } else if (cur->side_down) {
        cur->side_down();
    } else {
        enter_app(NULL);
    }
}

static TickType_t g_front_press_start;
static bool       g_front_long_latched;   // the current hold has crossed the long-press threshold

static void front_button_pressed(void)
{
    g_front_press_start = xTaskGetTickCount();
    vox_config_get(&g_press_cfg);
    g_front_long_latched = false;
    ESP_LOGI(TAG, "btn A down");
    const app_t *cur = g_cur;
    vox_where_btn = 1;
    if (cur && cur->front_down) cur->front_down();
}

// Released: decide what the press meant from how long it was held.
static void front_button_released(void)
{
    uint32_t held_ms = pdTICKS_TO_MS(xTaskGetTickCount() - g_front_press_start);
    bool is_long = held_ms >= g_press_cfg.long_press_ms;
    const app_t *cur = g_cur;
    vox_where_btn = 2;
    if (!cur) {
        enter_app(launcher_app(g_menu_sel));                 // any press length opens the highlighted app
    } else if ((cur->flags & APP_HOLD_EXITS) && held_ms >= MODE_TOGGLE_MS) {
        enter_app(NULL);                                     // back to the menu
    } else if (cur->front_up) {
        cur->front_up(held_ms, is_long);
    }
}

// While still held, switch the screen colour once the long-press threshold is crossed,
// so the user sees that releasing now counts as a long press.
static void front_button_held(void)
{
    if (g_front_long_latched) return;
    if (pdTICKS_TO_MS(xTaskGetTickCount() - g_front_press_start) < g_press_cfg.long_press_ms) return;
    g_front_long_latched = true;
    const app_t *cur = g_cur;
    if (cur && (cur->flags & APP_USB_MIC) && !g_vad_display_enabled) lcd_status(COL_BLUE);
}

void button_task(void *arg)
{
    (void)arg;
    debounce_t front = { true, true, 0 }, side = { true, true, 0 };
    while (1) {
        g_hb_btn++;
        vox_where_btn = 0;
        bool front_level = gpio_get_level(BTN_A_GPIO), side_level = gpio_get_level(BTN_B_GPIO);
        if (!front_level || !side_level) g_last_activity = xTaskGetTickCount();     // wakes a sleeping screen

        if (debounce(&side, side_level) && !side.committed) side_button_pressed();
        if (debounce(&front, front_level)) {
            if (!front.committed) front_button_pressed(); else front_button_released();
        }
        if (!front.committed) front_button_held();

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

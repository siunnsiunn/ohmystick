// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

// =========================================================================
// Launcher apps. To add one: write its callbacks (here or in its own file),
// define an app_t, and add it to k_apps below. See launcher.h.
// =========================================================================
vox_config_wire_t g_press_cfg;          // config snapshot taken when BtnA went down

static void icon_mic(int x, int y, uint16_t fg, uint16_t bg)       { (void)bg; lcd_icon_mic(x, y, fg); }
static void icon_bluetooth(int x, int y, uint16_t fg, uint16_t bg) { (void)bg; lcd_icon_bluetooth(x, y, fg); }
static void icon_note(int x, int y, uint16_t fg, uint16_t bg)      { lcd_icon_record(x, y, fg, bg); }
static void icon_slide(int x, int y, uint16_t fg, uint16_t bg)     { (void)bg; lcd_icon_slide(x, y, fg); }
static void icon_whisper(int x, int y, uint16_t fg, uint16_t bg)   // speech bubble with three dots
{
    (void)bg;
    lcd_fill_rect(x + 1, y + 3, x + 27, y + 5, fg);
    lcd_fill_rect(x + 1, y + 19, x + 27, y + 21, fg);
    lcd_fill_rect(x + 1, y + 3, x + 3, y + 21, fg);
    lcd_fill_rect(x + 25, y + 3, x + 27, y + 21, fg);
    lcd_draw_thick_line(x + 6, y + 21, x + 6, y + 27, 2, fg);
    lcd_draw_thick_line(x + 6, y + 27, x + 13, y + 21, 2, fg);
    for (int k = 0; k < 3; k++) lcd_fill_rect(x + 7 + k * 5, y + 10, x + 10 + k * 5, y + 14, fg);
}

// Taps or holds of the front button become a key chord; BLE apps try the radio
// first and fall back to USB so a plugged-in cable still works.
static void send_keys(bool ble_first, bool slide, bool is_long)
{
    uint8_t modifier = is_long ? g_press_cfg.hold_modifier : g_press_cfg.tap_modifier;
    uint8_t keycode  = is_long ? g_press_cfg.hold_keycode  : g_press_cfg.tap_keycode;
    if (slide) { modifier = 0; keycode = is_long ? HID_KEY_LEFT_ARROW : HID_KEY_RIGHT_ARROW; }
    ESP_LOGI(TAG, "front button %s -> 0x%02x+0x%02x", is_long ? "long" : "tap", modifier, keycode);
    bool via_ble = ble_first && ble_presenter_send_chord(modifier, keycode);
    if (!via_ble) hid_send_tap(modifier, keycode);
}

// ---- USB MIC: microphone + keys over the cable --------------------------------
static uint32_t usb_mic_tick(bool first)
{
    static bool last_active = false, last_codec_ready = false, last_muted = false;
    static uint32_t last_bucket = UINT32_MAX;
    if (first) {
        last_bucket = UINT32_MAX;                // force a mic-screen redraw
        if (!g_vad_display_enabled) lcd_status(g_codec_ready ? COL_GREEN : COL_RED);
    }
    if (!g_vad_display_enabled) return VAD_FRAME_MS;

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
        xSemaphoreTake(g_pres_lcd_mu, portMAX_DELAY);
        lcd_draw_mic_status(capped, active, codec_ready, muted);
        xSemaphoreGive(g_pres_lcd_mu);
        last_active = active;
        last_codec_ready = codec_ready;
        last_muted = muted;
        last_bucket = bucket;
    }
    return VAD_FRAME_MS;
}
static void usb_mic_front_down(void) { if (!g_vad_display_enabled) lcd_status(COL_MAGENTA); }
static void usb_mic_front_up(uint32_t held, bool is_long)
{
    (void)held;
    send_keys(false, false, is_long);
    if (!g_vad_display_enabled) lcd_status(g_codec_ready ? COL_GREEN : COL_RED);
}

// ---- BT DICT / SLIDE: BLE keyboard screens -------------------------------------
static uint32_t ble_screen_tick(bool first, bool slide)
{
    static bool last_up = false, force = true;
    static int last_st = -1;
    if (first) force = true;
    // Pairing in progress: leave the passkey up until encryption succeeds (state
    // 61), fails (98) or 60 s pass.
    if (g_passkey_hold_until != 0) {
        int s = ble_presenter_state();
        bool expired = (int32_t)(xTaskGetTickCount() - g_passkey_hold_until) >= 0;
        if (s == 61 || s == 98 || expired) {
            g_passkey_hold_until = 0;
            lcd_clear_locked();
            force = true;
        } else {
            return VAD_FRAME_MS;
        }
    }
    bool up = ble_presenter_connected();
    int st = ble_presenter_state();
    if (force || up != last_up || st != last_st) {
        force = false; last_up = up; last_st = st;
        if (slide) lcd_draw_presenter();
        else       lcd_draw_ble_dictation();
    }
    return VAD_FRAME_MS;
}
static uint32_t bt_dict_tick(bool first) { return ble_screen_tick(first, false); }
static uint32_t slide_tick(bool first)   { return ble_screen_tick(first, true); }
static void bt_dict_front_up(uint32_t held, bool is_long) { (void)held; send_keys(true, false, is_long); }
static void slide_front_up(uint32_t held, bool is_long)   { (void)held; send_keys(true, true, is_long); }
static void slide_side_down(void)                         // side button = previous slide
{
    ESP_LOGI(TAG, "side button (prev slide)");
    if (!ble_presenter_send_key(HID_KEY_LEFT_ARROW)) hid_send_tap(0, HID_KEY_LEFT_ARROW);
}

// ---- WHISPER / NOTE: hold the front button and talk ------------------------------
static uint32_t voice_tick(bool first, bool note)
{
    static int last_key = -1;
    int ui = (int)btmic_ui_state();
    int key = ui * 100 + btmic_pending() + (note ? 10000 : 0);
    if (first || key != last_key) {
        last_key = key;
        lcd_draw_btrec(ui, note);
    }
    return 50;
}
static uint32_t whisper_tick(bool first) { return voice_tick(first, false); }
static uint32_t note_tick(bool first)    { return voice_tick(first, true); }
static int  voice_activity(void)         { return (int)btmic_ui_state() * 100 + btmic_pending(); }
static void whisper_front_down(void)     { btmic_start(false); }   // live: needs the Mac listening
static void note_front_down(void)        { btmic_start(true); }    // stored on the stick
static void voice_front_up(uint32_t held, bool is_long) { (void)held; (void)is_long; btmic_stop(); }
static void voice_side_down(void)        { if (!btmic_recording()) enter_app(NULL); }


static const app_t app_usb_mic = {
    .name = "USB MIC", .name_zh = "USB麥克風", .desc_zh = "當電腦的麥克風", .color = COL_CYAN, .icon = icon_mic,
    .flags = APP_NO_SLEEP | APP_USB_MIC | APP_HOLD_EXITS,
    .tick = usb_mic_tick, .front_down = usb_mic_front_down, .front_up = usb_mic_front_up,
};
const app_t app_bt_dict = {
    .name = "BT DICT", .name_zh = "藍牙聽寫", .desc_zh = "藍牙鍵盤輸入", .color = COL_GREEN, .icon = icon_bluetooth,
    .flags = APP_NO_SLEEP | APP_BLE | APP_HOLD_EXITS,
    .tick = bt_dict_tick, .front_up = bt_dict_front_up,
};
static const app_t app_whisper = {
    .name = "WHISPER", .name_zh = "即時聽寫", .desc_zh = "邊說邊貼上", .color = COL_MAGENTA, .icon = icon_whisper,
    .flags = APP_BLE,
    .tick = whisper_tick, .activity_key = voice_activity,
    .front_down = whisper_front_down, .front_up = voice_front_up, .side_down = voice_side_down,
};
static const app_t app_note = {
    .name = "NOTE", .name_zh = "語音筆記", .desc_zh = "沒電腦也能錄", .color = COL_YELLOW, .icon = icon_note,
    .flags = APP_BLE | APP_SYNC | APP_MAINT,
    .tick = note_tick, .activity_key = voice_activity,
    .front_down = note_front_down, .front_up = voice_front_up, .side_down = voice_side_down,
};
const app_t app_slide = {
    .name = "SLIDE", .name_zh = "簡報翻頁", .desc_zh = "藍牙翻頁筆", .color = COL_ORANGE, .icon = icon_slide,
    .flags = APP_NO_SLEEP | APP_BLE | APP_HOLD_EXITS,
    .tick = slide_tick, .front_up = slide_front_up, .side_down = slide_side_down,
};

static const app_t app_diag = {
    .name = "DIAG", .name_zh = "狀態", .desc_zh = "診斷與配對", .color = COL_RED, .icon = icon_diag,
    .flags = APP_BLE,                                      // BLE up so LINK and UPLOAD mean something
    .tick = diag_tick, .front_up = diag_front_up,
};

static const app_t *const k_apps[] = { &app_usb_mic, &app_bt_dict, &app_whisper, &app_note, &app_slide, &app_diag };
const app_t *launcher_app(int i)  { return k_apps[i]; }
int          launcher_count(void) { return (int)(sizeof(k_apps) / sizeof(k_apps[0])); }

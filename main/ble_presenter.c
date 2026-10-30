#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_hidd.h"
#include "esp_hid_gap.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "ble_presenter.h"

static const char *TAG = "ble_pres";

// Test variants (overnight matrix) override the name and PID so the Mac keeps a
// separate entry per variant. Default build keeps the normal identity.
#ifndef VOX_BLE_NAME
#define VOX_BLE_NAME "StickS3 Remote"
#endif
#ifndef VOX_BLE_PID
#define VOX_BLE_PID 0x4002
#endif

void ble_store_config_init(void);

static esp_hidd_dev_t *s_hid = NULL;
static volatile bool s_connected = false;
static bool s_started = false;
static volatile int s_state = 0;   // 0 off,1 start,2 gap ok,3 adv ok,4 hidd ok,5 nimble ok,6 advertising; 9x = failure

// Boot-style 8 byte keyboard input report (id 1), no LED output.
static const uint8_t s_report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
};

static esp_hid_raw_report_map_t s_maps[] = {
    { .data = s_report_map, .len = sizeof(s_report_map) },
};

static esp_hid_device_config_t s_cfg = {
    .vendor_id = 0x303A,
    .product_id = VOX_BLE_PID,
    .version = 0x0100,
    .device_name = VOX_BLE_NAME,   // <=15 chars: adv packet is 31 B max
    .manufacturer_name = "voxstick",
    .serial_number = "0001",
    .report_maps = s_maps,
    .report_maps_len = 1,
};

static void hidd_event_cb(void *args, esp_event_base_t base, int32_t id, void *data)
{
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        s_state = (esp_hid_ble_gap_adv_start() == 0) ? 6 : 96;
        break;
    case ESP_HIDD_CONNECT_EVENT:
        s_connected = true;
        ESP_LOGI(TAG, "connected");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        ESP_LOGI(TAG, "disconnected");
        esp_hid_ble_gap_adv_start();
        break;
    default:
        break;
    }
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool ble_presenter_start(void)
{
    if (s_started) return true;
    s_state = 1;
    esp_err_t r;
    if ((r = esp_hid_gap_init(HIDD_BLE_MODE)) != ESP_OK) {
        ESP_LOGE(TAG, "gap init: %s", esp_err_to_name(r));
        s_state = 92;
        return false;
    }
    s_state = 2;
    // Name shown by hosts comes from the GAP service, not the adv packet.
    ble_svc_gap_device_name_set(s_cfg.device_name);
    if ((r = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD,
                                      s_cfg.device_name)) != ESP_OK) {
        ESP_LOGE(TAG, "adv init: %s", esp_err_to_name(r));
        s_state = 93;
        return false;
    }
    s_state = 3;
    if ((r = esp_hidd_dev_init(&s_cfg, ESP_HID_TRANSPORT_BLE,
                               hidd_event_cb, &s_hid)) != ESP_OK) {
        ESP_LOGE(TAG, "hidd init: %s", esp_err_to_name(r));
        s_state = 94;
        return false;
    }
    s_state = 4;
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    if (esp_nimble_enable(host_task) != 0) {
        ESP_LOGE(TAG, "nimble enable failed");
        s_state = 95;
        return false;
    }
    s_state = 5;
    s_started = true;
    return true;
}

static void start_task(void *arg)
{
    (void)ble_presenter_start();
    vTaskDelete(NULL);
}

// BLE/NimBLE init needs far more stack than the 2 KB button task has.
void ble_presenter_start_async(void)
{
    static bool requested = false;
#if !VOX_ENABLE_BLE_PRESENTER
    // BLE presenter is experimental (macOS never created a HID device for it);
    // keep the radio off unless built with -DVOX_ENABLE_BLE_PRESENTER=ON.
    return;
#endif
    if (requested) return;
    requested = true;
    if (xTaskCreate(start_task, "ble_start", 6144, NULL, 4, NULL) != pdPASS) {
        requested = false;
        ESP_LOGE(TAG, "ble start task create failed");
    }
}

static volatile bool s_encrypted = false;

void ble_presenter_set_state(int st)
{
    if (st == 61) s_encrypted = true;
    else if (st == 6 || st == 7 || st >= 90) s_encrypted = false;
    s_state = st;
}

int ble_presenter_state(void)
{
    return s_state;
}

bool ble_presenter_connected(void)
{
    return s_started && s_connected;
}

bool ble_presenter_send_chord(uint8_t modifier, uint8_t keycode)
{
    if (keycode == 0) return false;
    // Only trust the link once pairing has encrypted it; otherwise the caller
    // falls back to USB.
    if (!ble_presenter_connected() || !s_encrypted || s_hid == NULL) return false;
    uint8_t rpt[8] = {0};
    rpt[0] = modifier;
    rpt[2] = keycode;
    esp_err_t r1 = esp_hidd_dev_input_set(s_hid, 0, 1, rpt, sizeof(rpt));
    vTaskDelay(pdMS_TO_TICKS(30));
    memset(rpt, 0, sizeof(rpt));
    esp_err_t r2 = esp_hidd_dev_input_set(s_hid, 0, 1, rpt, sizeof(rpt));
    bool ok = (r1 == ESP_OK && r2 == ESP_OK);
    ESP_LOGI(TAG, "send chord 0x%02x+0x%02x: %s/%s", modifier, keycode,
             esp_err_to_name(r1), esp_err_to_name(r2));
    s_state = ok ? 71 : 72;     // shown on LCD: 71 sent ok, 72 send failed
    return ok;
}

bool ble_presenter_send_key(uint8_t keycode)
{
    return ble_presenter_send_chord(0, keycode);
}

// Hooks referenced by esp_hid_gap.c (the upstream example used them to start
// demo tasks). Presenter mode sends keys on demand, so nothing to do.
void ble_hid_task_start_up(void) {}
void ble_hid_task_shut_down(void) {}

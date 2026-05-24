#include "vox_config.h"

#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "nvs.h"

#define VOX_CONFIG_MAGIC        0x31474656u  // "VFG1", little-endian
#define VOX_CONFIG_STORE_VER    1
#define VOX_CONFIG_NAMESPACE    "voxstick"
#define VOX_CONFIG_KEY          "cfg"

#define HID_KEY_F12             0x45
#define HID_KEY_ENTER           0x28
#define HID_MOD_LEFT_CTRL       0x01

#define LONG_PRESS_MIN_MS       250
#define LONG_PRESS_MAX_MS       2000

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    vox_config_wire_t data;
} vox_config_store_t;

static const char *TAG = "voxstick";
static portMUX_TYPE s_config_lock = portMUX_INITIALIZER_UNLOCKED;
static vox_config_wire_t s_config;

static vox_config_wire_t default_config(void)
{
    return (vox_config_wire_t) {
        .flat_mute_enabled = 1,
        .tap_modifier = HID_MOD_LEFT_CTRL,
        .tap_keycode = HID_KEY_F12,
        .hold_modifier = 0,
        .hold_keycode = HID_KEY_ENTER,
        .reserved = 0,
        .long_press_ms = 600,
        .reserved2 = 0,
    };
}

static bool keycode_valid(uint8_t keycode, bool allow_none)
{
    if (keycode == 0) {
        return allow_none;
    }

    // Printable keys, arrows, and F1..F24 all live in this range in the
    // USB HID Keyboard/Keypad usage page. We keep validation broad so the
    // config page can grow without needing firmware changes.
    return keycode >= 0x04 && keycode <= 0x73;
}

static bool config_valid(vox_config_wire_t const *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    if (!keycode_valid(cfg->tap_keycode, false)) {
        return false;
    }
    if (!keycode_valid(cfg->hold_keycode, true)) {
        return false;
    }
    if (cfg->long_press_ms < LONG_PRESS_MIN_MS ||
        cfg->long_press_ms > LONG_PRESS_MAX_MS) {
        return false;
    }
    return true;
}

static esp_err_t config_save(vox_config_wire_t const *cfg)
{
    vox_config_store_t store = {
        .magic = VOX_CONFIG_MAGIC,
        .version = VOX_CONFIG_STORE_VER,
        .size = sizeof(vox_config_store_t),
        .data = *cfg,
    };

    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open(VOX_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_blob(nvs, VOX_CONFIG_KEY, &store, sizeof(store));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static esp_err_t config_load(vox_config_wire_t *out)
{
    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open(VOX_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    vox_config_store_t store = {0};
    size_t len = sizeof(store);
    ret = nvs_get_blob(nvs, VOX_CONFIG_KEY, &store, &len);
    nvs_close(nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    if (len != sizeof(store) ||
        store.magic != VOX_CONFIG_MAGIC ||
        store.version != VOX_CONFIG_STORE_VER ||
        store.size != sizeof(store) ||
        !config_valid(&store.data)) {
        return ESP_ERR_INVALID_STATE;
    }

    *out = store.data;
    return ESP_OK;
}

void vox_config_init(void)
{
    vox_config_wire_t cfg = default_config();
    esp_err_t ret = config_load(&cfg);
    if (ret == ESP_ERR_NVS_NOT_FOUND || ret == ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGI(TAG, "config: using defaults");
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "config load failed (%s); using defaults",
                 esp_err_to_name(ret));
    }

    portENTER_CRITICAL(&s_config_lock);
    s_config = cfg;
    portEXIT_CRITICAL(&s_config_lock);

    ESP_LOGI(TAG, "config: flat_mute=%u tap=0x%02x+0x%02x hold=0x%02x+0x%02x long=%u ms",
             cfg.flat_mute_enabled,
             cfg.tap_modifier,
             cfg.tap_keycode,
             cfg.hold_modifier,
             cfg.hold_keycode,
             cfg.long_press_ms);
}

void vox_config_get(vox_config_wire_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_config_lock);
    *out = s_config;
    portEXIT_CRITICAL(&s_config_lock);
}

bool vox_config_flat_mute_enabled(void)
{
    bool enabled;
    portENTER_CRITICAL(&s_config_lock);
    enabled = s_config.flat_mute_enabled != 0;
    portEXIT_CRITICAL(&s_config_lock);
    return enabled;
}

esp_err_t vox_config_set(vox_config_wire_t const *cfg)
{
    if (!config_valid(cfg)) {
        return ESP_ERR_INVALID_ARG;
    }

    vox_config_wire_t normalized = *cfg;
    normalized.flat_mute_enabled = normalized.flat_mute_enabled ? 1 : 0;
    normalized.reserved = 0;
    normalized.reserved2 = 0;

    esp_err_t ret = config_save(&normalized);
    if (ret != ESP_OK) {
        return ret;
    }

    portENTER_CRITICAL(&s_config_lock);
    s_config = normalized;
    portEXIT_CRITICAL(&s_config_lock);

    ESP_LOGI(TAG, "config saved: flat_mute=%u tap=0x%02x+0x%02x hold=0x%02x+0x%02x long=%u ms",
             normalized.flat_mute_enabled,
             normalized.tap_modifier,
             normalized.tap_keycode,
             normalized.hold_modifier,
             normalized.hold_keycode,
             normalized.long_press_ms);
    return ESP_OK;
}

esp_err_t vox_config_reset(void)
{
    vox_config_wire_t cfg = default_config();
    return vox_config_set(&cfg);
}

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOX_CONFIG_PROTOCOL_VERSION 1

#define VOX_CONFIG_CMD_GET   1
#define VOX_CONFIG_CMD_SET   2
#define VOX_CONFIG_CMD_RESET 3

#define VOX_CONFIG_STATUS_OK       0
#define VOX_CONFIG_STATUS_BAD_REQ  1
#define VOX_CONFIG_STATUS_INVALID  2
#define VOX_CONFIG_STATUS_STORAGE  3

typedef struct __attribute__((packed)) {
    uint8_t  flat_mute_enabled;
    uint8_t  tap_modifier;
    uint8_t  tap_keycode;
    uint8_t  hold_modifier;
    uint8_t  hold_keycode;
    uint8_t  reserved;
    uint16_t long_press_ms;
    uint32_t reserved2;
} vox_config_wire_t;

void vox_config_init(void);
void vox_config_get(vox_config_wire_t *out);
bool vox_config_flat_mute_enabled(void);
esp_err_t vox_config_set(vox_config_wire_t const *cfg);
esp_err_t vox_config_reset(void);

#ifdef __cplusplus
}
#endif

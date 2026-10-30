#pragma once
// BT REC: streams the StickS3 microphone to a Mac helper over a custom BLE GATT
// service (docs/BT-MIC.md). Audio is IMA ADPCM, 20 ms per notification, and is
// only captured while the front button is held.
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BTMIC_UI_NOLINK = 0,   // not subscribed / not encrypted / MTU too small
    BTMIC_UI_READY,        // link usable, waiting for the button
    BTMIC_UI_REC,          // recording and streaming
    BTMIC_UI_BUSY,         // the Mac is transcribing
    BTMIC_UI_DONE,         // text delivered
    BTMIC_UI_ERR,          // the Mac reported an error
} btmic_ui_t;

// Supplies mono 16 kHz int16 samples (blocking). Returns the number of samples
// read, or a negative value on error.
typedef int (*btmic_read_fn)(int16_t *dst, int n);

// Starts the sender task. Call once after the codec is ready.
void btmic_init(btmic_read_fn reader);

// GATT service registration (called from the esp_hid component, before the
// GATT server starts) and the hooks esp_hid_gap.c forwards.
int  vox_audio_svc_init(void);
void btmic_on_subscribe(uint16_t conn_handle, uint16_t attr_handle, uint8_t cur_notify);
void btmic_on_disconnect(void);

bool       btmic_start(void);       // false if the link is not ready
void       btmic_stop(void);
bool       btmic_recording(void);
btmic_ui_t btmic_ui_state(void);

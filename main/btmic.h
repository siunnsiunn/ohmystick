#pragma once
// WHISPER / NOTE: streams the StickS3 microphone to a Mac helper over a custom BLE GATT
// service (docs/BT-MIC.md). Audio is IMA ADPCM, 20 ms per notification, and is
// only captured while the front button is held.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BTMIC_UI_NOLINK = 0,   // not subscribed / not encrypted / MTU too small
    BTMIC_UI_READY,        // link usable, waiting for the button
    BTMIC_UI_REC,          // recording and streaming
    BTMIC_UI_BUSY,         // the Mac is transcribing
    BTMIC_UI_DONE,         // text delivered
    BTMIC_UI_ERR,          // the Mac reported an error
    BTMIC_UI_REC_OFF,      // recording to flash (no Mac in reach)
    BTMIC_UI_SYNC,         // uploading stored recordings
    BTMIC_UI_FULL,         // no free slot for an offline recording
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

// Which screens are up: stored recordings upload only on the BT REC screen, and
// flash housekeeping runs only on idle screens (menu, BT REC).
void       btmic_set_mode(bool sync_ok, bool maintain_ok);
int        btmic_pending(void);     // recordings stored on the stick
void       btmic_debug(char *buf, size_t n);   // short upload diagnostics for the screen
// Pairing policy (esp_hid_gap.c). vox_pair_open() only sets a flag, so any task may call it;
// vox_pair_service() touches the host and is run by the recording task.
void vox_pair_open(void);
void vox_pair_service(void);
void vox_pair_clear_all(void);       // forget every bonded device (and open the pairing window); done by vox_pair_service()
int  vox_pair_status(int *left_s);   // 0 locked, 1 window open (seconds left), 2 nothing paired yet
void vox_pair_info(char *buf, size_t n, char *sec, size_t sn);
extern volatile uint32_t vox_crumb;
extern volatile uint32_t vox_hb_rec;            // last activity marker; checked by the monitor task

bool       btmic_start(bool offline); // live (needs the Mac listening) or stored on the stick; false if not possible
void       btmic_stop(void);
bool       btmic_recording(void);
btmic_ui_t btmic_ui_state(void);

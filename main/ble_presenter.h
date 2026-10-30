#pragma once
// BLE HID keyboard used by presenter mode (slide keys) and BT dictation mode
// (hotkey that starts the host's own dictation).
// The radio stays OFF until ble_presenter_start() is called by an explicit
// user gesture, and is never started in dictation mode.
#include <stdbool.h>
#include <stdint.h>

bool ble_presenter_start(void);
void ble_presenter_start_async(void);   // safe to call from small-stack tasks
bool ble_presenter_connected(void);
void ble_presenter_set_state(int st);   // 7 linked, 8 passkey shown, 61 encrypted
int ble_presenter_state(void);   // 0 off .. 6 advertising, 9x failure
bool ble_presenter_send_key(uint8_t keycode);
bool ble_presenter_send_chord(uint8_t modifier, uint8_t keycode);   // modifier bitmask + one key

#pragma once
// WIFI: the WHISPER flow over Wi-Fi instead of BLE (docs/WIFI-PLAN.md).
// The stick joins the Wi-Fi network, opens a TCP connection to winvoice on the PC, proves it knows
// the pairing key and encrypts everything after that. It sends the same frames as
// docs/BT-MIC.md, each prefixed with its length.
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    WIFIMIC_UI_OFF = 0,     // radio not started yet
    WIFIMIC_UI_SETUP,       // setup hotspot is up, waiting for the phone
    WIFIMIC_UI_SAVED,       // settings saved, restarting
    WIFIMIC_UI_WIFI,        // joining the Wi-Fi network
    WIFIMIC_UI_TAILNET,     // joining the tailnet
    WIFIMIC_UI_PEER,        // connecting to winvoice on the PC
    WIFIMIC_UI_READY,       // link usable, waiting for the button
    WIFIMIC_UI_REC,         // recording and streaming
    WIFIMIC_UI_BUSY,        // the PC is transcribing
    WIFIMIC_UI_DONE,        // text delivered
    WIFIMIC_UI_ERR,         // the PC reported an error
} wifimic_ui_t;

void         wifimic_enter(void);            // app entered: start Wi-Fi + tailnet on first use
wifimic_ui_t wifimic_ui_state(void);
bool         wifimic_start(void);            // front button down; false if there is no link
void         wifimic_stop(void);             // front button up
bool         wifimic_recording(void);
void         wifimic_request_setup(void);    // reboot into the setup hotspot

// Screen details for the current state: setup hotspot name / password / address,
// or the Wi-Fi name, the PC address and whether the tunnel is direct or relayed.
void wifimic_detail(char *a, size_t an, char *b, size_t bn);

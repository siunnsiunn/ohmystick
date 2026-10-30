#pragma once
// A launcher entry ("app"). The menu, button dispatch, screen sleep, BLE start-up
// and USB-mic gating are all driven from this table, so a new feature is one
// source file plus one line in the table in main.c.
//
//   static uint32_t my_tick(bool first) { ...draw if needed...; return 100; }
//   const app_t app_my = { .name = "MY APP", .color = COL_CYAN, .icon = my_icon,
//                          .flags = APP_BLE, .tick = my_tick, ... };
//
// Callbacks run on the display task (tick) or the button task (the rest); keep
// them short and never block. Drawing helpers live in ui.h.
#include <stdbool.h>
#include <stdint.h>

#define APP_NO_SLEEP    0x01   // never dim or switch off the screen
#define APP_USB_MIC     0x02   // the USB microphone is live while this app is up
#define APP_BLE         0x04   // start the BLE radio on entry
#define APP_SYNC        0x08   // stored voice notes may upload to the Mac
#define APP_MAINT       0x10   // flash housekeeping (erasing voice-note slots) is fine
#define APP_HOLD_EXITS  0x20   // holding the front button for 2.5 s returns to the menu

typedef struct app_s {
    const char *name;                       // menu label, ASCII, uppercase letters/digits
    uint16_t    color;                      // RGB565
    void      (*icon)(int x, int y, uint16_t fg, uint16_t bg);   // 28x28 at (x, y)
    uint32_t    flags;

    void      (*enter)(void);               // optional
    void      (*leave)(void);               // optional
    // Called about every `return` ms (0 = 100). `first` is true on the first call
    // after entry; the screen has already been cleared then.
    uint32_t  (*tick)(bool first);
    int       (*activity_key)(void);        // optional: a changing value wakes the screen
    void      (*front_down)(void);          // optional
    void      (*front_up)(uint32_t held_ms, bool is_long);       // optional
    void      (*side_down)(void);           // NULL = back to the menu
} app_t;

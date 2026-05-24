// voxstick TinyUSB config — enables UAC (mic) + HID Keyboard composite.
//
// We're in CONFIG_USB_DEVICE_UAC_AS_PART=y mode, so the usb_device_uac
// component delegates all descriptor + tusb_config decisions back to us.
// We provide:
//   - this header (TinyUSB compile-time class enables, EP counts, MTUs)
//   - usb_descriptors.c (device + configuration + report descriptors)
//
// Layout: 1 audio control + 1 mic streaming + 1 HID + 1 vendor = 4 interfaces.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "sdkconfig.h"
#include "uac_config.h"          // SPEAK/MIC channel #defines from menuconfig
#include "uac_descriptors.h"     // brings TUD_AUDIO_DESC_* helper macros
#include "tusb_config_uac.h"     // EP sizes computed from sample rate / channels

// ---- Board / port -------------------------------------------------------
// StickS3 ESP32-S3 has only full-speed PHY (no HS variant pins exposed).
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CONFIG_USB_HS           0

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS             OPT_OS_FREERTOS
#endif

#ifndef ESP_PLATFORM
#define ESP_PLATFORM 1
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG          0
#endif

#if TU_CHECK_MCU(OPT_MCU_ESP32S2, OPT_MCU_ESP32S3, OPT_MCU_ESP32P4)
#define CFG_TUSB_OS_INC_PATH    freertos/
#endif

#define CFG_TUD_ENABLED         1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN      __attribute__ ((aligned(4)))
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE  64
#endif

// ---- Class enables ------------------------------------------------------
// CFG_TUD_AUDIO is defined by tusb_config_uac.h based on UAC_*_CHANNEL_NUM.
// We add HID for the push-to-talk button and a tiny vendor pipe for browser
// configuration plus recovery tooling.
#define CFG_TUD_HID             1
#define CFG_TUD_HID_EP_BUFSIZE  16   // keyboard report = 8 B; 16 is plenty

#define CFG_TUD_VENDOR          1
#define CFG_TUD_VENDOR_EPSIZE   64
#define CFG_TUD_VENDOR_RX_BUFSIZE 64
#define CFG_TUD_VENDOR_TX_BUFSIZE 64

#ifdef __cplusplus
}
#endif

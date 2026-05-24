// voxstick USB descriptors — composite UAC microphone + HID Keyboard +
// vendor control pipe.
//
// Adapted from espressif/usb_device_uac's reference usb_descriptors.c
// (MIT, © 2019 Ha Thach / 2020 Jerzy Kasenbreg). The UAC pieces are
// untouched; we add a HID interface at the end so a single physical USB
// connection presents both an audio input and a keyboard to the host.
//
// Why a Keyboard report and not Consumer Control: VoiceInk (and most
// macOS dictation tools) bind their push-to-talk hotkey to a regular
// keyboard key. F19 is the standard "I will never collide with anything"
// pick — mac-native keyboards don't have it, no app maps it by default.
//
// The vendor interface is intentionally boring: one bulk OUT/IN pair for
// browser configuration and a no-button ROM download fallback.

#include <string.h>
#include "tusb.h"
#include "uac_descriptors.h"
#include "sdkconfig.h"

// AS_PART mode hides usb_device_uac's CONFIG_UAC_TUSB_* knobs (they're
// inside `if !USB_DEVICE_UAC_AS_PART` in its Kconfig), so we own these.
// Espressif USB VID 0x303A is fine to reuse for community projects per
// their PID allocation policy.
#define DESC_VID            0x303A
#define DESC_PID            0x8001
#define DESC_MANUFACTURER   "voxstick"
#define DESC_PRODUCT        "StickS3-Mic"
#define DESC_SERIAL         "0001"

// =========================================================================
// Interface numbering — we are CONFIG_USB_DEVICE_UAC_AS_PART=y, so we own
// the ITF_NUM_* enum.
// =========================================================================
enum {
    ITF_NUM_AUDIO_CONTROL = 0,
#if MIC_CHANNEL_NUM
    ITF_NUM_AUDIO_STREAMING_MIC,
#endif
    ITF_NUM_HID,
    ITF_NUM_VENDOR,
    ITF_NUM_TOTAL
};

// Endpoints: TinyUSB convention is bit 7 = direction (1 = IN, 0 = OUT).
// UAC IN comes first; HID IN sits after it.
#define EPNUM_AUDIO_IN   0x82
#define EPNUM_HID_IN     0x83
#define EPNUM_VENDOR_OUT 0x01
#define EPNUM_VENDOR_IN  0x81

// =========================================================================
// Device descriptor
// =========================================================================
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    // Multi-class device → IAD-aware enumeration on the host side.
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = DESC_VID,
    .idProduct          = DESC_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

// =========================================================================
// HID Report descriptor — minimal Boot Keyboard with a Report ID
// =========================================================================
uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1))
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return desc_hid_report;
}

void voxstick_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                                   hid_report_type_t report_type,
                                   uint8_t const *buffer, uint16_t bufsize);

// Host can ask us for state via SET_REPORT/GET_REPORT — we don't track
// LEDs (caps lock etc.) since we only emit a single key.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    voxstick_hid_set_report_cb(instance, report_id, report_type, buffer, bufsize);
}

// =========================================================================
// Configuration descriptor
// =========================================================================
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN \
                           + CFG_TUD_AUDIO * TUD_AUDIO_DEVICE_DESC_LEN \
                           + TUD_HID_DESC_LEN \
                           + CFG_TUD_VENDOR * TUD_VENDOR_DESC_LEN)

uint8_t const desc_configuration[] = {
    // bConfigurationValue=1, ITF count, string idx, total length, attr,
    // bus power in mA (100 mA — the audio class doesn't tell us much, and
    // the StickS3's battery is buffered behind PMIC anyway).
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // Audio control + mic streaming. EP OUT = 0 because we're mic-only,
    // EP feedback = 0 for the same reason.
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, /* iAS */ 4,
                         /* EP_OUT */ 0,
                         /* EP_IN  */ EPNUM_AUDIO_IN,
                         /* EP_FB  */ 0),

    // HID Keyboard. iInterface=5, no boot protocol (we set HID_ITF_PROTOCOL_NONE
    // because we're a "regular" keyboard for VoiceInk, not a BIOS keyboard).
    // 10 ms polling matches the audio interval — both interrupts share one bus.
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, /* iInterface */ 5,
                       HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report),
                       EPNUM_HID_IN, CFG_TUD_HID_EP_BUFSIZE,
                       /* polling ms */ 10),

    // Vendor-specific control interface for WebUSB config and recovery tools.
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, /* iInterface */ 6,
                          EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN,
                          CFG_TUD_VENDOR_EPSIZE),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

// =========================================================================
// String descriptors
// =========================================================================
char const *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },        // 0: language = English (0x0409)
    DESC_MANUFACTURER,                   // 1: iManufacturer
    DESC_PRODUCT,                        // 2: iProduct
    DESC_SERIAL,                         // 3: iSerialNumber
    "voxstick audio",                 // 4: UAC interface
    "voxstick PTT",                   // 5: HID interface
    "voxstick control",               // 6: vendor control interface
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
            return NULL;
        }
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = (uint16_t)str[i];
        }
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

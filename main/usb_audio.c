// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

// =========================================================================
// USB UAC callbacks — host pulls bytes from us via input_cb
// =========================================================================
static void fill_codec_fail_diag_tone(uint8_t *buf, size_t len)
{
#if VOX_DIAG_CODEC_FAIL_TONE
    int16_t *samples = (int16_t *)buf;
    size_t count = len / sizeof(int16_t);
    for (size_t i = 0; i < count; i++) {
        // 1 kHz square wave at 16 kHz sample rate: 8 samples high, 8 low.
        samples[i] = ((g_diag_tone_phase++ & 0x0f) < 8) ? 4000 : -4000;
    }
    if (len & 1) {
        buf[len - 1] = 0;
    }
#else
    memset(buf, 0, len);
#endif
}

static void vad_update_from_pcm(uint8_t const *buf, size_t len, bool valid_audio)
{
    if (!valid_audio || buf == NULL || len < sizeof(int16_t)) {
        g_vad_level = (g_vad_level * 3U) / 4U;
        if (g_vad_level < VAD_OFF_LEVEL) {
            g_vad_active = false;
        }
        return;
    }

    int16_t const *samples = (int16_t const *)buf;
    size_t count = len / sizeof(int16_t);
    uint64_t sum_abs = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t v = samples[i];
        sum_abs += (uint32_t)(v < 0 ? -v : v);
    }

    uint32_t avg = (uint32_t)(sum_abs / count);
    uint32_t smoothed = (g_vad_level * 3U + avg) / 4U;
    g_vad_level = smoothed;

    if (g_vad_active) {
        g_vad_active = smoothed >= VAD_OFF_LEVEL;
    } else {
        g_vad_active = smoothed >= VAD_ON_LEVEL;
    }
}

static esp_err_t uac_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *arg)
{
    // Two independent mute paths feed the same silence-the-stream branch:
    //   g_uac_muted - host-side mute (UAC volume control says mic off)
    //   g_imu_mute  - physical orientation mute (stick lying flat on table)
    if (effective_mic_muted()) {
        memset(buf, 0, len);
        vad_update_from_pcm(NULL, 0, false);
        *bytes_read = len;
        return ESP_OK;
    }
    if (!g_codec_ready) {
        fill_codec_fail_diag_tone(buf, len);
        vad_update_from_pcm(buf, len, VOX_DIAG_CODEC_FAIL_TONE != 0);
        *bytes_read = len;
        return ESP_OK;
    }
    esp_err_t ret = esp_codec_dev_read(g_codec_dev, buf, len);
    if (ret == ESP_OK) {
        // Software make-up gain: ES8311 max analog PGA + max digital still
        // peaks around -38 dB on the StickS3 MEMS mic, which whisper.cpp /
        // VoiceInk struggle to transcribe. 8x = +18 dB lifts speech to
        // ~ -20 dB peak, comfortably above ASR's threshold. Saturating
        // clamp at int16 range avoids wraparound on loud claps.
        int16_t *s = (int16_t *)buf;
        size_t cnt = len / sizeof(int16_t);
        for (size_t i = 0; i < cnt; i++) {
            int32_t v = (int32_t)s[i] * 8;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            s[i] = (int16_t)v;
        }
    }
    vad_update_from_pcm(buf, len, ret == ESP_OK);
    *bytes_read = (ret == ESP_OK) ? len : 0;
    return ret;
}

// Feeds btmic: same mic path and +18 dB make-up gain as the USB microphone.
int btmic_pcm_read(int16_t *dst, int n)
{
    if (!g_codec_ready) return -1;
    esp_err_t ret = esp_codec_dev_read(g_codec_dev, dst, n * (int)sizeof(int16_t));
    if (ret != ESP_OK) return -1;
    for (int i = 0; i < n; i++) {
        int32_t v = (int32_t)dst[i] * 8;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        dst[i] = (int16_t)v;
    }
    return n;
}

static esp_err_t uac_output_cb(uint8_t *buf, size_t len, void *arg)
{
    // No speaker in v1 — host shouldn't even see an output stream because
    // CONFIG_UAC_MIC_*-only is configured, but be safe.
    return ESP_OK;
}

static void uac_set_mute_cb(uint32_t mute, void *arg)
{
    g_uac_muted = !!mute;
    ESP_LOGI(TAG, "host mute=%d", (int)mute);
}

static void uac_set_volume_cb(uint32_t volume, void *arg)
{
    // _volume = (volume_db + 50) * 2  -> volume_db
    int volume_db = (int)volume / 2 - 50;
    // Map volume_db to ES8311 in_gain (0..40 dB roughly). Below -10 dB just
    // ride at minimum; whisper.cpp won't transcribe useful audio quieter.
    float gain = 30.0f + (float)volume_db * 0.5f;
    if (gain < 0.0f)  gain = 0.0f;
    if (gain > 40.0f) gain = 40.0f;
    if (g_codec_dev) {
        esp_codec_dev_set_in_gain(g_codec_dev, gain);
    }
    ESP_LOGI(TAG, "host volume=%d dB -> mic gain=%.1f", volume_db, gain);
}

// Interface numbers must match those in usb_descriptors.c. usb_device_uac
// needs to know which streaming interface(s) it owns so it can answer
// SET_INTERFACE / GET_CUR / etc. on the right ITF.
#define UAC_ITF_NUM_MIC   1
#define UAC_ITF_NUM_SPK   -1   // mic-only build

// Force the linker to pull usb_descriptors.c.obj out of libmain.a. tinyusb
// references these symbols from inside its own static archive, so without
// at least one reference from a non-archive object the descriptor file
// silently gets dropped. ESP-IDF's main isn't built with --whole-archive.
extern uint8_t const *tud_descriptor_device_cb(void);
extern uint8_t const *tud_descriptor_configuration_cb(uint8_t index);
extern void voxstick_force_link_usb_recovery(void);
__attribute__((used)) static const void *const _link_usb_descriptors[] = {
    (const void *)tud_descriptor_device_cb,
    (const void *)tud_descriptor_configuration_cb,
    (const void *)voxstick_force_link_usb_recovery,
};

esp_err_t uac_init(void)
{
    uac_device_config_t cfg = {
        .output_cb     = uac_output_cb,
        .input_cb      = uac_input_cb,
        .set_mute_cb   = uac_set_mute_cb,
        .set_volume_cb = uac_set_volume_cb,
        .cb_ctx        = NULL,
        .spk_itf_num   = UAC_ITF_NUM_SPK,
        .mic_itf_num   = UAC_ITF_NUM_MIC,
    };
    return uac_device_init(&cfg);
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == VENDOR_DOWNLOAD_REQUEST) {
        if (!host_download_allowed()) {
            ESP_LOGW(TAG, "vendor download request ignored: BtnA not held");
            return false;
        }
        ESP_LOGW(TAG, "USB vendor download magic");
        request_rom_download("USB vendor magic requested ROM download", 150);
        return tud_control_status(rhport, request);
    }

    return false;
}


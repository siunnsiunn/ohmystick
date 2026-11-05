// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"
#include <math.h>

// =========================================================================
// Short beeps through the speaker, for the hourglass. The microphone path is
// left alone until the first beep: the output half of the I2S port, the codec's
// DAC and the amplifier are only switched on while a beep plays, then off again.
// =========================================================================
#define BEEP_HZ      880
#define BEEP_AMP     350            // of 32767
#define BEEP_DAC_VOL 0x80           // ES8311 DAC volume, 0.5 dB a step: 0xBF is 0 dB, so this is about -32 dB
#define BEEP_ON_MS   180
#define BEEP_OFF_MS  140

static i2s_chan_handle_t s_tx = NULL;
static volatile int s_beeps_left = 0;
static bool s_busy = false;
volatile int g_beep_dbg = 0;

static esp_err_t tx_open(void)
{
    if (s_tx) return ESP_OK;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "beep tx channel");
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = I2S_GPIO_UNUSED, .ws = I2S_GPIO_UNUSED,
                      .dout = I2S_DOUT_PIN, .din = I2S_GPIO_UNUSED },
    };
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
    std_cfg.clk_cfg.mclk_multiple = AUDIO_MCLK_MULTIPLE;       // same as the receive half: the clocks are shared
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    esp_err_t e = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (e != ESP_OK) { i2s_del_channel(s_tx); s_tx = NULL; }
    return e;
}

// The register set M5Unified uses to play on the StickS3 (it resets the chip first, so the
// microphone registers are written again afterwards).
static void dac_on(void)
{
    static const uint8_t seq[][2] = {
        {0x00, 0x80}, {0x01, 0xB5}, {0x02, 0x18}, {0x0D, 0x01},
        {0x12, 0x00}, {0x13, 0x10}, {0x32, BEEP_DAC_VOL}, {0x37, 0x08},
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) esp_codec_dev_write_reg(g_codec_dev, seq[i][0], seq[i][1]);
}

static void dac_off(void)
{
    codec_mic_regs();
}

static void beep_task(void *arg)
{
    (void)arg;
    esp_err_t e = ESP_OK;
    if (!g_codec_ready) g_beep_dbg = 1;
    else if ((e = tx_open()) != ESP_OK) g_beep_dbg = 2;
    else if ((e = i2s_channel_enable(s_tx)) != ESP_OK) g_beep_dbg = 3;
    else {
        dac_on();
        pmic_speaker(true);
        vTaskDelay(pdMS_TO_TICKS(30));
        static int16_t buf[AUDIO_SAMPLE_RATE / 10 * 2];      // 100 ms, left and right the same
        const int n = AUDIO_SAMPLE_RATE / 10;
        while (s_beeps_left > 0) {
            for (int ms = 0; ms < BEEP_ON_MS; ms += 100) {
                for (int i = 0; i < n; i++) {
                    int t = ms * (AUDIO_SAMPLE_RATE / 1000) + i;                          // sample index in this beep
                    int total = BEEP_ON_MS * (AUDIO_SAMPLE_RATE / 1000), ramp = AUDIO_SAMPLE_RATE / 25;    // 40 ms ramps: soft attack and release
                    float env = t >= total ? 0.f : (t < ramp ? (float)t / ramp : (total - t < ramp ? (float)(total - t) / ramp : 1.f));
                    buf[2 * i] = buf[2 * i + 1] = (int16_t)(BEEP_AMP * env * sinf(2.f * (float)M_PI * BEEP_HZ * t / AUDIO_SAMPLE_RATE));
                }
                size_t w = 0;
                i2s_channel_write(s_tx, buf, sizeof(buf), &w, 500);
            }
            memset(buf, 0, sizeof(buf));
            for (int ms = 0; ms < BEEP_OFF_MS; ms += 100) {
                size_t w = 0;
                i2s_channel_write(s_tx, buf, sizeof(buf), &w, 500);
            }
            s_beeps_left--;
        }
        pmic_speaker(false);
        dac_off();
        i2s_channel_disable(s_tx);
        g_beep_dbg = 4;
    }
    if (g_beep_dbg < 4) ESP_LOGW(TAG, "beep not played: stage %d, %s", g_beep_dbg, esp_err_to_name(e));
    s_beeps_left = 0;
    s_busy = false;
    vTaskDelete(NULL);
}

void beep_play(int count)
{
    if (s_busy || count <= 0) return;
    s_busy = true;
    s_beeps_left = count;
    if (xTaskCreate(beep_task, "beep", 4096, NULL, 3, NULL) != pdPASS) s_busy = false;
}

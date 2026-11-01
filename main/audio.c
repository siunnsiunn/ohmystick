// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

// =========================================================================
// I2S RX channel — pure data path. Codec config goes through the I2C side.
// =========================================================================
esp_err_t i2s_rx_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &g_i2s_rx),
                        TAG, "i2s_new_channel rx");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_DIN_PIN,
            .invert_flags = { 0 },
        },
    };
    // M5Unified's StickS3 path samples the ES8311 on I2S1, right slot, with
    // MCLK = 128 * fs. Using the left slot gives a perfectly valid but silent
    // DMA stream on this board.
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
    std_cfg.clk_cfg.mclk_multiple = AUDIO_MCLK_MULTIPLE;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_i2s_rx, &std_cfg),
                        TAG, "i2s_channel_init_std_mode");
    // Enable the channel NOW (not later in codec_open) so MCLK starts
    // toggling on GPIO18 — ES8311 doesn't ACK on I2C without MCLK to
    // drive its internal logic and we'd otherwise see "Fail to read
    // from dev 30" on every codec_init probe.
    return i2s_channel_enable(g_i2s_rx);
}

// =========================================================================
// ES8311 codec — register init via esp_codec_dev
// =========================================================================
esp_err_t codec_init(void)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port    = I2C_PORT,
        .addr    = ES8311_CODEC_DEFAULT_ADDR,    // 7-bit addr 0x18 shifted
        .bus_handle = g_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "i2c ctrl_if");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = I2S_PORT,
        .rx_handle = g_i2s_rx,
        .tx_handle = NULL,
        .clk_src   = I2S_CLK_SRC_PLL_160M,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "i2s data_if");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if    = ctrl_if,
        .gpio_if    = NULL,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,   // ADC = mic in only
        .pa_pin     = -1,
        .use_mclk   = true,
        .digital_mic = false,
        .mclk_div   = AUDIO_MCLK_DIV,
        // CRITICAL: in mic-only builds the DAC isn't running, so the default
        // "ADCL + DACR" internal reference (es8311.c writes 0x58 to REG44)
        // leaves the ADC referenced to a dead DAC right channel — every
        // sample comes back as 0x0000 even though the codec ACKs and the
        // I2S DMA is healthy. no_dac_ref=true switches REG44 to 0x08 which
        // uses the codec's standalone ADC reference instead.
        .no_dac_ref = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311_codec_new");

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if  = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
    };
    g_codec_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(g_codec_dev, ESP_FAIL, TAG, "esp_codec_dev_new");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = AUDIO_SAMPLE_RATE,
        .channel         = 2,
        .channel_mask    = AUDIO_INPUT_MASK,
        .bits_per_sample = AUDIO_BITS,
        .mclk_multiple   = AUDIO_MCLK_MULTIPLE,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(g_codec_dev, &fs),
                        TAG, "codec open");

    // Mirror M5Unified's _microphone_enabled_cb_sticks3 ES8311 register set
    // verbatim. esp_codec_dev's es8311 driver does most of these on open(),
    // but its 0x01 default uses non-inverted BCLK and on this hardware the
    // ADC then samples on the wrong edge and reads constant zero.
    //
    //   0x00 = 0x80  RESET / CSM POWER ON
    //   0x01 = 0xBA  CLOCK_MANAGER, MCLK=BCLK, BCLK_INV (mic-specific!)
    //   0x02 = 0x18  CLOCK_MANAGER, MULT_PRE=3
    //   0x0D = 0x01  SYSTEM, power up analog circuitry
    //   0x0E = 0x02  SYSTEM, enable analog PGA + ADC modulator
    //   0x14 = 0x10  ADC_REG14, select Mic1p-Mic1n single-ended-diff input.
    //                NOTE: PGA *gain* lives at 0x16, not 0x14. 0x10 here just
    //                turns the analog PGA on with mic1p/n input selected.
    //   0x16 = 0x07  ADC_REG16, mic PGA gain step (7 = +42 dB max).
    //   0x17 = 0xFF  ADC_VOLUME, max digital gain
    //   0x1C = 0x6A  ADC, equalizer bypass + DC offset cancel
    esp_codec_dev_write_reg(g_codec_dev, 0x00, 0x80);
    esp_codec_dev_write_reg(g_codec_dev, 0x01, 0xBA);
    esp_codec_dev_write_reg(g_codec_dev, 0x02, 0x18);
    esp_codec_dev_write_reg(g_codec_dev, 0x0D, 0x01);
    esp_codec_dev_write_reg(g_codec_dev, 0x0E, 0x02);
    esp_codec_dev_write_reg(g_codec_dev, 0x14, 0x10);
    esp_codec_dev_write_reg(g_codec_dev, 0x16, 0x07);
    esp_codec_dev_write_reg(g_codec_dev, 0x17, 0xFF);
    esp_codec_dev_write_reg(g_codec_dev, 0x1C, 0x6A);

    g_codec_ready = true;
    return ESP_OK;
}


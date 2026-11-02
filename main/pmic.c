// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

// =========================================================================
// I2C bus (shared with PMIC + IMU + codec)
// =========================================================================
esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t cfg = {
        .clk_source                  = I2C_CLK_SRC_DEFAULT,
        .i2c_port                    = I2C_PORT,
        .scl_io_num                  = I2C_SCL_PIN,
        .sda_io_num                  = I2C_SDA_PIN,
        .glitch_ignore_cnt           = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &g_i2c_bus);
}

// =========================================================================
// PMIC — M5PM1 on the StickS3 internal I2C0.
//
// This is not an AXP2101 register map. Earlier experiments writing
// AXP2101-style LDO registers to this chip left the ES8311 unreachable until
// a true power-on reset. Keep this init intentionally tiny: discover the PMIC
// address, then configure only M5PM1 GPIO3, which controls the speaker PA.
//
// Observed address on this StickS3 is 0x6E. The 0x34/0x36 probes are left only
// as harmless diagnostics for board variation.
// =========================================================================
uint8_t g_pmic_addr = 0;     // discovered at boot

// Battery: M5PM1 VBAT = reg 0x22 (low byte) + 0x23 (low 4 bits), in mV; PWR_SRC
// reg 0x04 is 0 when running from the USB 5V input (register map: m5stack/M5PM1).
// The chip has no charge-complete flag, so "USB powered" stands in for charging.
// One device handle, made on first use and kept: the chip is only talked to at 100 kHz
// (400 kHz dropped ACKs on this board), so the speed is fixed here.
static i2c_master_dev_handle_t pmic_dev(void)
{
    static i2c_master_dev_handle_t dev = NULL;
    if (!dev && g_pmic_addr) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = g_pmic_addr,
            .scl_speed_hz    = 100000,
        };
        if (i2c_master_bus_add_device(g_i2c_bus, &cfg, &dev) != ESP_OK) dev = NULL;
    }
    return dev;
}

static esp_err_t pmic_read_n(uint8_t reg, uint8_t *out, size_t n)
{
    i2c_master_dev_handle_t d = pmic_dev();
    return d ? i2c_master_transmit_receive(d, &reg, 1, out, n, 100) : ESP_ERR_NOT_FOUND;
}

static esp_err_t pmic_read(uint8_t reg, uint8_t *val) { return pmic_read_n(reg, val, 1); }

static esp_err_t pmic_write(uint8_t reg, uint8_t val)
{
    i2c_master_dev_handle_t d = pmic_dev();
    uint8_t buf[2] = { reg, val };
    return d ? i2c_master_transmit(d, buf, sizeof(buf), 100) : ESP_ERR_NOT_FOUND;
}

// Linear map of cell voltage to percent (same curve as before: 100 % at 4100 mV).
#define BATT_EMPTY_MV 3300
#define BATT_FULL_MV  4100

// A reading older than this is shown as an estimate ("約"). On battery power the chip
// often answers with nonsense, so the last good value is all there is.
#define BATT_FRESH_MS 60000
// Raw control registers for the details page, to see what differs between a battery boot
// (battery voltage unreadable) and after a USB plug-in (readable). Reads only.
void pmic_regs_text(char *buf, size_t n)
{
    static const uint8_t regs[] = { 0x04, 0x06, 0x09, 0x2A };
    int len = snprintf(buf, n, "PM");
    for (size_t i = 0; i < sizeof(regs) && len > 0 && (size_t)len < n; i++) {
        uint8_t v = 0;
        if (pmic_read_n(regs[i], &v, 1) == ESP_OK) len += snprintf(buf + len, n - len, " %02X:%02X", regs[i], v);
        else len += snprintf(buf + len, n - len, " %02X:--", regs[i]);
    }
}

bool battery_is_estimate(void)
{
    return g_batt_pct >= 0 && g_batt_good_tick != 0 &&
           pdTICKS_TO_MS(xTaskGetTickCount() - g_batt_good_tick) > BATT_FRESH_MS;
}

void battery_sample(void)
{
    // While the chip keeps answering nonsense, ask less often and with fewer retries:
    // each attempt holds up the display task.
    static int s_odd_streak = 0, s_skip = 0;
    if (s_odd_streak >= 3 && (s_skip++ % 6) != 0) return;
    uint8_t v[2] = {0}, src = 0xFF;
    // The codec shares this I2C bus, so a read can fail while it is busy. Try a few
    // times; if it still fails keep showing the last good value, and after repeated
    // failures reset the bus (a stuck transfer would otherwise fail every read).
    static int s_fails = 0;
    // While the radio is busy the chip sometimes answers with nonsense (about 12 mV
    // was seen), so a reading outside the plausible range is retried like an error.
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < (s_odd_streak >= 3 ? 2 : 6); i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(60));
        e = pmic_read_n(0x22, v, 2);
        int m = v[0] | ((v[1] & 0x0F) << 8);
        if (e == ESP_OK && m >= 2500 && m <= 4600) break;
    }
    if (e != ESP_OK) {
        g_batt_dbg = (int)e < 0 ? -(int)e : (int)e;
        ESP_LOGW(TAG, "battery read failed (%s), %d in a row", esp_err_to_name(e), s_fails + 1);
        // Resetting the bus cuts into other devices on it (IMU, codec): only after repeated
        // failures, and never when the chip is simply not there.
        if (++s_fails >= 5 && e != ESP_ERR_NOT_FOUND) { i2c_master_bus_reset(g_i2c_bus); s_fails = 0; }
        return;                                  // keep the previous reading on screen
    }
    s_fails = 0;
    int mv = v[0] | ((v[1] & 0x0F) << 8);
    if (pmic_read_n(0x04, &src, 1) != ESP_OK) src = 0xFF;
    // Measured on a StickS3: VIN reads ~1100 mV with a cable in and 0 on battery
    // (not the real 5 V, so the ADC is scaled), 5VINOUT ~960 vs ~20, and PWR_SRC
    // 5 vs 4. The input voltage separates the two cleanly.
    uint8_t w[4] = {0};
    int vin = 0, vio = 0;
    if (pmic_read_n(0x24, w, 4) == ESP_OK) {
        vin = w[0] | ((w[1] & 0x0F) << 8);
        vio = w[2] | ((w[3] & 0x0F) << 8);
        g_batt_ext = (vin > 500 || vio > 500);      // a failed read keeps the last answer (no flicker of the bolt)
    }
    if (mv < 2500 || mv > 4600) {                          // implausible: keep the last good reading
        g_batt_dbg = 10000 + (mv % 10000);
        s_odd_streak++;
    } else {
        s_odd_streak = 0;
        g_batt_good_tick = xTaskGetTickCount() ? xTaskGetTickCount() : 1;
        g_batt_dbg = 0;
        g_batt_mv = mv;
        int pct = (mv - BATT_EMPTY_MV) * 100 / (BATT_FULL_MV - BATT_EMPTY_MV);
        g_batt_pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    }
    ESP_LOGI(TAG, "battery %d mV -> %d%%, pwr_src=%u vin=%d vio=%d ext=%d",
             mv, g_batt_pct, (unsigned)src, vin, vio, (int)g_batt_ext);
}

// M5StickS3's M5PM1 always sits at 0x6E (verified across builds; the
// AXP2101 0x34 / 0x36 alternates were probed during early development
// and never ACK'd on this hardware). Probe still confirms presence so a
// dead-bus failure mode is logged loudly.
static esp_err_t pmic_probe(void)
{
    if (i2c_master_probe(g_i2c_bus, 0x6E, 100) != ESP_OK) {
        ESP_LOGE(TAG, "no pmic at 0x6E");
        lcd_status(COL_RED);
        return ESP_ERR_NOT_FOUND;
    }
    g_pmic_addr = 0x6E;
    ESP_LOGI(TAG, "pmic ACK at 0x6E");
    return ESP_OK;
}

// Scan the I2C bus and log every device that ACKs. Used once to find out
// which address the M5PM1 actually sits at on this hardware revision.
void i2c_scan(void)
{
    ESP_LOGI(TAG, "i2c bus scan starting...");
    int found = 0;
    for (uint8_t a = 1; a < 0x78; a++) {
        if (i2c_master_probe(g_i2c_bus, a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  -> 0x%02x ACK", a);
            found++;
        }
    }
    ESP_LOGI(TAG, "i2c scan done: %d device(s)", found);
}

// PMIC reg helper for read-modify-write of single bits, M5Unified-style.
static esp_err_t pmic_clear_bit(uint8_t reg, uint8_t bit_mask)
{
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(pmic_read(reg, &v), TAG, "pmic_read");
    return pmic_write(reg, v & ~bit_mask);
}

static esp_err_t pmic_set_bit(uint8_t reg, uint8_t bit_mask)
{
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(pmic_read(reg, &v), TAG, "pmic_read");
    return pmic_write(reg, v | bit_mask);
}

static esp_err_t pmic_enable_lcd_codec_rail(void)
{
    // From M5GFX/src/M5GFX.cpp board_M5StickS3 init:
    //
    //   "PM1_G2 -- L3B Enable, LCD Power On (M5Stack PM1 G2)"
    //
    // The LCD module Vcc and the ES8311 audio codec share an external
    // L3B-style LDO whose enable pin is wired to M5PM1 GPIO2. M5GFX's
    // board_M5StickS3 path drives this output HIGH before initializing the
    // panel; mirror that sequence exactly here.
    ESP_RETURN_ON_ERROR(pmic_clear_bit(0x16, 1 << 2), TAG, "G2 gpio fn");
    ESP_RETURN_ON_ERROR(pmic_set_bit  (0x10, 1 << 2), TAG, "G2 dir out");
    ESP_RETURN_ON_ERROR(pmic_clear_bit(0x13, 1 << 2), TAG, "G2 push-pull");
    ESP_RETURN_ON_ERROR(pmic_set_bit  (0x11, 1 << 2), TAG, "G2 out HIGH/L3B on");

    // Register 0x09 = I2C_CFG. M5GFX comment:
    //
    //   "Set to 0x00 to disable I2C idle sleep mode. PMIC is always-on
    //    powered, and with battery power, shutdown doesn't reset the chip.
    //    This register may have been modified elsewhere, causing PMIC
    //    communication issues. Explicitly set it here during init to
    //    ensure proper operation."
    //
    // Without this we've seen the PMIC stop ACK'ing after a few seconds.
    ESP_RETURN_ON_ERROR(pmic_write(0x09, 0x00), TAG, "I2C_CFG no-sleep");

    // L3B settle. M5GFX waits 100 ms before initializing the LCD bus.
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static void pmic_dump_regs(const char *label)
{
    static const uint8_t regs[] = { 0x06, 0x10, 0x11, 0x13, 0x16 };
    ESP_LOGI(TAG, "pmic dump: %s", label);
    for (size_t i = 0; i < sizeof(regs); i++) {
        uint8_t v = 0;
        esp_err_t ret = pmic_read(regs[i], &v);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  reg 0x%02x = 0x%02x", regs[i], v);
        } else {
            ESP_LOGW(TAG, "  reg 0x%02x read failed: %s",
                     regs[i], esp_err_to_name(ret));
        }
    }
}

// M5StickS3 PMIC init based on M5GFX/M5Unified upstream:
//   * GPIO2 (PM1_G2) HIGH — enables L3B LDO that powers the LCD module Vcc
//     and the ES8311 audio codec rail. Has to come up before any LCD or
//     codec bring-up is attempted.
//   * register 0x09 = 0x00 — disables PMIC I2C idle sleep so subsequent
//     transactions don't drop after a few seconds of bus quiet.
//   * GPIO3 (PM1_G3) LOW with push-pull output — speaker PA mute, prevents
//     the amplifier ticking on every I2S DMA start.
//
// M5PM1 register map (from M5Unified / M5GFX):
//   0x06  Power config (bit 2 = LDO_EN on M5PaperColor; *not* LCD on StickS3)
//   0x09  I2C_CFG       (bit 0 = idle sleep enable; clear to keep PMIC awake)
//   0x10  GPIO direction      bit n: 1 = output, 0 = input
//   0x11  GPIO output level   bit n: 1 = high, 0 = low
//   0x13  GPIO mode           bit n: 1 = open-drain, 0 = push-pull
//   0x16  GPIO function       bit n: 1 = alt, 0 = GPIO
esp_err_t pmic_init(void)
{
    ESP_RETURN_ON_ERROR(pmic_probe(), TAG, "pmic probe");
    battery_sample();      // first reading now, before the radio is up: while it advertises the chip often answers nonsense
    pmic_dump_regs("before init");

    ESP_RETURN_ON_ERROR(pmic_enable_lcd_codec_rail(), TAG, "L3B / I2C_CFG");

    // Speaker PA off: GPIO3 as push-pull output, low.
    ESP_RETURN_ON_ERROR(pmic_clear_bit(0x16, 1 << 3), TAG, "PA gpio fn");
    ESP_RETURN_ON_ERROR(pmic_set_bit  (0x10, 1 << 3), TAG, "PA dir out");
    ESP_RETURN_ON_ERROR(pmic_clear_bit(0x13, 1 << 3), TAG, "PA push-pull");
    ESP_RETURN_ON_ERROR(pmic_clear_bit(0x11, 1 << 3), TAG, "PA out low");

    pmic_dump_regs("after init");

    ESP_LOGI(TAG, "pmic configured (G2=HIGH/L3B on, G3=LOW/PA off)");
    return ESP_OK;
}


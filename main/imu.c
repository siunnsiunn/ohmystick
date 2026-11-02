// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"

// =========================================================================
// BMI270 IMU — orientation-based mic auto-mute
//
// Flat on table  -> mic muted (privacy: stick can't pick up nearby talk)
// Picked up      -> mic live
//
// 6-axis IMU on the same internal I2C bus as the codec / PMIC, address 0x68.
// We use Bosch's official driver because the chip refuses to deliver
// readings without a ~8 KB feature config blob the driver loads at init.
// We only ever read the accelerometer; gyro / step counter / wrist gestures
// stay disabled to keep average current low.
// =========================================================================
#define IMU_I2C_ADDR             BMI2_I2C_PRIM_ADDR    // 0x68
#define IMU_I2C_SPEED_HZ         400000
#define IMU_TX_BUF_LEN           64                    // > read_write_len + 1
// |Z accel| above this fraction of 1 g => "lying flat", mute.
//   16384 LSB = 1 g at ±2 g range, 16-bit. 0.7 g ~= 11500.
#define IMU_FLAT_THRESHOLD_LSB   11500
// Hysteresis to prevent chatter when the stick is at 45° on a stack of paper.
#define IMU_LIVE_THRESHOLD_LSB   9500
#define IMU_POLL_MS              200

static struct bmi2_dev          g_bmi;
static i2c_master_dev_handle_t  g_imu_dev = NULL;
static uint8_t                  g_imu_tx_buf[IMU_TX_BUF_LEN];

static BMI2_INTF_RETURN_TYPE imu_i2c_read(uint8_t reg_addr, uint8_t *reg_data,
                                          uint32_t len, void *intf_ptr)
{
    (void)intf_ptr;
    if (!g_imu_dev) return BMI2_E_COM_FAIL;
    esp_err_t err = i2c_master_transmit_receive(g_imu_dev, &reg_addr, 1,
                                                reg_data, len, 100);
    return (err == ESP_OK) ? BMI2_OK : BMI2_E_COM_FAIL;
}

static BMI2_INTF_RETURN_TYPE imu_i2c_write(uint8_t reg_addr,
                                           const uint8_t *reg_data,
                                           uint32_t len, void *intf_ptr)
{
    (void)intf_ptr;
    if (!g_imu_dev) return BMI2_E_COM_FAIL;
    if (len + 1 > sizeof(g_imu_tx_buf)) return BMI2_E_COM_FAIL;
    g_imu_tx_buf[0] = reg_addr;
    memcpy(g_imu_tx_buf + 1, reg_data, len);
    esp_err_t err = i2c_master_transmit(g_imu_dev, g_imu_tx_buf, len + 1, 200);
    return (err == ESP_OK) ? BMI2_OK : BMI2_E_COM_FAIL;
}

static void imu_delay_us(uint32_t period_us, void *intf_ptr)
{
    (void)intf_ptr;
    if (period_us >= 10000) {                      // the tick is 10 ms: anything shorter rounds to a zero-tick delay
        vTaskDelay(pdMS_TO_TICKS(period_us / 1000) + 1);
    } else {
        esp_rom_delay_us(period_us);
    }
}

esp_err_t imu_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = IMU_I2C_ADDR,
        .scl_speed_hz    = IMU_I2C_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &g_imu_dev),
                        TAG, "imu add_device");

    static uint8_t addr = IMU_I2C_ADDR;
    g_bmi.intf           = BMI2_I2C_INTF;
    g_bmi.intf_ptr       = &addr;
    g_bmi.read           = imu_i2c_read;
    g_bmi.write          = imu_i2c_write;
    g_bmi.delay_us       = imu_delay_us;
    g_bmi.read_write_len = IMU_TX_BUF_LEN - 1;

    int8_t r = bmi270_init(&g_bmi);
    if (r != BMI2_OK) {
        ESP_LOGW(TAG, "bmi270_init failed: %d", r);
        return ESP_FAIL;
    }

    struct bmi2_sens_config conf = { .type = BMI2_ACCEL };
    if (bmi2_get_sensor_config(&conf, 1, &g_bmi) != BMI2_OK) return ESP_FAIL;
    conf.cfg.acc.odr         = BMI2_ACC_ODR_50HZ;        // plenty for 5 Hz check
    conf.cfg.acc.range       = BMI2_ACC_RANGE_2G;
    conf.cfg.acc.bwp         = BMI2_ACC_NORMAL_AVG4;
    conf.cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
    if (bmi2_set_sensor_config(&conf, 1, &g_bmi) != BMI2_OK) return ESP_FAIL;

    uint8_t sens_list[1] = { BMI2_ACCEL };
    if (bmi2_sensor_enable(sens_list, 1, &g_bmi) != BMI2_OK) return ESP_FAIL;

    ESP_LOGI(TAG, "bmi270 ready (accel @50 Hz, ±2 g)");
    return ESP_OK;
}

// Turning the stick over (like an hourglass): the gravity vector in the screen plane points
// the opposite way. Counted here so apps only watch g_imu_flips. A reference direction is kept
// while the stick is held on its edge (gravity in the plane > 0.7 g); lying flat does not change it.
volatile uint32_t g_imu_flips = 0;
#define IMU_EDGE_MIN_LSB   11500
static void imu_flip_update(int32_t x, int32_t y)
{
    static int32_t rx = 0, ry = 0;
    static int against = 0;
    int64_t mag2 = (int64_t)x * x + (int64_t)y * y;
    if (mag2 < (int64_t)IMU_EDGE_MIN_LSB * IMU_EDGE_MIN_LSB) { against = 0; return; }   // flat or tilted: no direction
    if (rx == 0 && ry == 0) { rx = x; ry = y; return; }
    int64_t dot = (int64_t)x * rx + (int64_t)y * ry;
    if (dot < 0) {
        if (++against >= 2) { against = 0; rx = x; ry = y; g_imu_flips++; ESP_LOGI(TAG, "imu: flipped"); }
    } else {
        against = 0;
        if (dot > 0 && mag2 > 0) { rx = x; ry = y; }          // follow slow turns so only a real flip counts
    }
}

void imu_task(void *arg)
{
    struct bmi2_sens_data data = {0};
    bool prev_flat = false;
    while (1) {
        if (bmi2_get_sensor_data(&data, &g_bmi) == BMI2_OK &&
            (data.status & BMI2_DRDY_ACC)) {
            // Convention: stick lying flat (face up or face down) on a
            // table -> |z| dominates. Pick it up to portrait or any
            // tilted orientation -> |z| drops, |x|/|y| dominate.
            imu_flip_update(data.acc.x, data.acc.y);
            int16_t z = data.acc.z;
            int32_t az = (z < 0) ? -(int32_t)z : (int32_t)z;
            bool flat;
            if (prev_flat) {
                flat = (az > IMU_LIVE_THRESHOLD_LSB);
            } else {
                flat = (az > IMU_FLAT_THRESHOLD_LSB);
            }
            if (flat != prev_flat) {
                prev_flat = flat;
                g_imu_mute = flat;
                bool flat_mute_enabled = vox_config_flat_mute_enabled();
                ESP_LOGI(TAG, "imu: %s (|z|=%ld lsb, flat_mute=%s)",
                         flat ? "flat" : "upright",
                         (long)az,
                         flat_mute_enabled ? "on" : "off");
                // Refresh the LCD when not in VAD-display mode so the
                // user has visual confirmation of the mute state.
                if (!g_vad_display_enabled) {
                    if (flat && flat_mute_enabled) {
                        lcd_status(COL_DIM_RED);
                    } else {
                        lcd_status(g_codec_ready ? COL_GREEN : COL_RED);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_POLL_MS));
    }
}


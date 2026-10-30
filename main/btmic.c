#include "btmic.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"

static const char *TAG = "btmic";

// 5e3b0000-8a7c-4f1e-9b2d-7c4a1f0e6d10 (little-endian bytes; n = last group byte).
#define VOX_UUID(n) BLE_UUID128_INIT(0x10, 0x6d, 0x0e, 0x1f, 0x4a, 0x7c, 0x2d, 0x9b, \
                                     0x1e, 0x4f, 0x7c, 0x8a, (n), 0x00, 0x3b, 0x5e)
static const ble_uuid128_t k_svc_uuid   = VOX_UUID(0x00);
static const ble_uuid128_t k_audio_uuid = VOX_UUID(0x01);
static const ble_uuid128_t k_ctrl_uuid  = VOX_UUID(0x02);

#define SAMPLES_PER_FRAME 320                      // 20 ms at 16 kHz
#define HEADER_LEN        12
#define FRAME_LEN         (HEADER_LEN + SAMPLES_PER_FRAME / 2)
#define MIN_ATT_MTU       (FRAME_LEN + 3)          // payload + ATT header
#define MAX_FRAMES        3000                     // 60 s safety stop
#define STATUS_HOLD_MS    3000                     // how long DONE/ERR stay up
#define BUSY_HOLD_MS      60000                    // BUSY lasts until DONE/ERR (or this)

enum { TYPE_AUDIO = 1, TYPE_END = 2, TYPE_START = 3 };
enum { CTRL_BUSY = 1, CTRL_DONE = 2, CTRL_ERR = 3 };

static uint16_t          s_audio_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool     s_sub = false;
static volatile bool     s_recording = false;
static volatile uint8_t  s_ctrl_status = 0;
static volatile TickType_t s_ctrl_until = 0;
static uint16_t          s_session = 0;
static btmic_read_fn     s_reader = NULL;
static TaskHandle_t      s_task = NULL;

// ---- IMA ADPCM -------------------------------------------------------------
static const int16_t k_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t k_idx[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

typedef struct { int pred; int idx; } adpcm_t;

static uint8_t adpcm_encode(int16_t sample, adpcm_t *s)
{
    int diff = sample - s->pred;
    int code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int step = k_step[s->idx];
    int vpdiff = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 1; vpdiff += step; }
    s->pred += (code & 8) ? -vpdiff : vpdiff;
    if (s->pred > 32767) s->pred = 32767;
    if (s->pred < -32768) s->pred = -32768;
    s->idx += k_idx[code];
    if (s->idx < 0) s->idx = 0;
    if (s->idx > 88) s->idx = 88;
    return (uint8_t)code;
}

// ---- GATT ------------------------------------------------------------------
static int audio_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;     // notify-only
}

static int ctrl_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t b = 0;
    if (OS_MBUF_PKTLEN(ctxt->om) < 1 || os_mbuf_copydata(ctxt->om, 0, 1, &b) != 0) return 0;
    if (b == CTRL_BUSY || b == CTRL_DONE || b == CTRL_ERR) {
        s_ctrl_status = b;
        s_ctrl_until = xTaskGetTickCount() +
                       pdMS_TO_TICKS(b == CTRL_BUSY ? BUSY_HOLD_MS : STATUS_HOLD_MS);
        ESP_LOGI(TAG, "ctrl status %u", b);
    }
    return 0;
}

static const struct ble_gatt_svc_def k_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &k_audio_uuid.u,
                .access_cb = audio_access,
                .val_handle = &s_audio_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
            },
            {
                .uuid = &k_ctrl_uuid.u,
                .access_cb = ctrl_access,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
            },
            { 0 },
        },
    },
    { 0 },
};

int vox_audio_svc_init(void)
{
    int rc = ble_gatts_count_cfg(k_svcs);
    if (rc != 0) return rc;
    return ble_gatts_add_svcs(k_svcs);
}

void btmic_on_subscribe(uint16_t conn_handle, uint16_t attr_handle, uint8_t cur_notify)
{
    if (attr_handle != s_audio_handle) return;
    s_conn = conn_handle;
    s_sub = cur_notify != 0;
    ESP_LOGI(TAG, "audio notify %s (conn %u)", s_sub ? "on" : "off", conn_handle);
}

void btmic_on_disconnect(void)
{
    s_sub = false;
    s_recording = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
}

// The link is usable only when subscribed, encrypted and the MTU fits a frame.
static bool link_ready(void)
{
    if (!s_sub || s_conn == BLE_HS_CONN_HANDLE_NONE) return false;
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(s_conn, &d) != 0 || !d.sec_state.encrypted) return false;
    return ble_att_mtu(s_conn) >= MIN_ATT_MTU;
}

static bool send_frame(const uint8_t *f, size_t n)
{
    for (int tries = 0; tries < 4; tries++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(f, n);
        if (!om) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        int rc = ble_gatts_notify_custom(s_conn, s_audio_handle, om);   // consumes om
        if (rc == 0) return true;
        if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

static void fill_header(uint8_t *f, uint8_t type, uint16_t seq, const adpcm_t *s)
{
    memset(f, 0, HEADER_LEN);
    f[0] = 1;
    f[1] = type;
    f[2] = s_session & 0xFF; f[3] = s_session >> 8;
    f[4] = seq & 0xFF;       f[5] = seq >> 8;
    f[6] = 1;                                   // codec: IMA ADPCM 16 kHz mono
    uint16_t p = (uint16_t)(int16_t)s->pred;
    f[8] = p & 0xFF; f[9] = p >> 8; f[10] = (uint8_t)s->idx;
}

static void rec_task(void *arg)
{
    (void)arg;
    static int16_t pcm[SAMPLES_PER_FRAME];
    uint8_t frame[FRAME_LEN];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_recording) continue;
        s_session++;
        uint16_t seq = 0;
        adpcm_t st = {0, 0};
        int dropped = 0;

        fill_header(frame, TYPE_START, seq++, &st);
        if (!send_frame(frame, HEADER_LEN)) dropped++;

        while (s_recording && seq < MAX_FRAMES) {
            int n = s_reader ? s_reader(pcm, SAMPLES_PER_FRAME) : -1;
            if (n < 0) { ESP_LOGW(TAG, "mic read failed"); break; }
            for (int i = n; i < SAMPLES_PER_FRAME; i++) pcm[i] = 0;
            fill_header(frame, TYPE_AUDIO, seq++, &st);
            for (int i = 0; i < SAMPLES_PER_FRAME / 2; i++) {
                uint8_t lo = adpcm_encode(pcm[2 * i], &st);
                uint8_t hi = adpcm_encode(pcm[2 * i + 1], &st);
                frame[HEADER_LEN + i] = lo | (hi << 4);
            }
            if (!send_frame(frame, FRAME_LEN)) dropped++;
        }
        s_recording = false;

        // The end frame carries the final sequence number so the Mac can tell
        // whether anything was lost; repeat it because notifications can drop.
        for (int i = 0; i < 3; i++) {
            fill_header(frame, TYPE_END, seq, &st);
            send_frame(frame, HEADER_LEN);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ESP_LOGI(TAG, "session %u: %u frames, %d dropped", s_session, seq, dropped);
    }
}

void btmic_init(btmic_read_fn reader)
{
    s_reader = reader;
    if (!s_task) xTaskCreate(rec_task, "btmic", 4096, NULL, 5, &s_task);
}

bool btmic_start(void)
{
    if (s_recording || !s_task || !link_ready()) return false;
    s_ctrl_status = 0;
    s_recording = true;
    xTaskNotifyGive(s_task);
    return true;
}

void btmic_stop(void)
{
    s_recording = false;
}

bool btmic_recording(void)
{
    return s_recording;
}

btmic_ui_t btmic_ui_state(void)
{
    if (s_recording) return BTMIC_UI_REC;
    if (s_ctrl_status != 0 && (int32_t)(xTaskGetTickCount() - s_ctrl_until) < 0) {
        switch (s_ctrl_status) {
        case CTRL_BUSY: return BTMIC_UI_BUSY;
        case CTRL_DONE: return BTMIC_UI_DONE;
        default:        return BTMIC_UI_ERR;
        }
    }
    return link_ready() ? BTMIC_UI_READY : BTMIC_UI_NOLINK;
}

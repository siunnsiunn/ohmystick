#include "btmic.h"
#include "esp_random.h"
#include "inbox.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"

static const char *TAG = "btmic";

// Survives a watchdog or panic reset so the next boot can show where it died.
RTC_NOINIT_ATTR volatile uint32_t vox_crumb;
volatile uint32_t vox_hb_rec = 0;                   // bumped by the recording/upload task; the monitor watches it
static volatile bool s_link_ok = false;            // cached; refreshed only by the recording task
static volatile int  s_last_rc = 0;                // last notify error, for the NOTE screen

// Why the last upload attempt ended: 1 read failed, 2 send failed, 3 link lost,
// 4 user started recording, 5 acknowledged, 6 answer timed out, 7 left NOTE.
static volatile uint8_t  s_sync_reason = 0;
static volatile uint16_t s_sync_tries = 0;
static TickType_t        s_sync_retry_at = 0;      // back-off after a failure

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
enum { CTRL_BUSY = 1, CTRL_DONE = 2, CTRL_ERR = 3, CTRL_FULL = 4 /* local only */, CTRL_PING = 5, CTRL_RETRY = 6 /* Mac: stored recording incomplete, send again */ };
#define PEER_ALIVE_MS     6000                     // no heartbeat for this long = no Mac listening
#define FLAG_OFFLINE      0x01                     // frame header byte 7: stored recording

static uint16_t          s_audio_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool     s_sub = false;
static volatile bool     s_recording = false;
static volatile uint8_t  s_ctrl_status = 0;
static volatile TickType_t s_ctrl_until = 0;
static uint16_t          s_session = 0;
static btmic_read_fn     s_reader = NULL;
static TaskHandle_t      s_task = NULL;
static volatile bool     s_sync_ok = false;        // BT REC screen is up: uploads allowed
static volatile bool     s_maint_ok = false;       // idle screens: flash housekeeping allowed
static volatile bool     s_syncing = false;
static volatile bool     s_rec_offline = false;
static volatile bool     s_ping_seen = false;      // heartbeat from the Mac helper
static volatile TickType_t s_last_ping = 0;
static volatile uint16_t s_ctrl_session = 0;       // session the last DONE/ERR/BUSY refers to (0 = unknown)
static volatile uint16_t s_retry_sess = 0;         // Mac asked for this session to be sent again
static volatile bool     s_retry_flag = false;
// A stored recording that has been sent and is waiting for the Mac's DONE. It is
// tracked in the background so recording or leaving the screen never loses the answer.
static int               s_ack_slot = -1;
static uint16_t          s_ack_sess = 0;
static TickType_t        s_ack_until = 0;
static TickType_t        s_ack_start = 0;
#define ACK_MAX_MS        900000                   // give up on one recording after 15 min of BUSY
#define ACK_WAIT_MS       90000                    // Mac needs this long at most per recording

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
    // [status] or [status, session lo, session hi]
    uint8_t buf[3] = {0};
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1) return 0;
    if (len > sizeof(buf)) len = sizeof(buf);
    if (os_mbuf_copydata(ctxt->om, 0, len, buf) != 0) return 0;
    uint8_t b = buf[0];
    uint16_t sess = len >= 3 ? (uint16_t)(buf[1] | (buf[2] << 8)) : 0;
    if (b == CTRL_PING) {
        s_last_ping = xTaskGetTickCount();
        s_ping_seen = true;
        return 0;
    }
    if (b == CTRL_RETRY) {
        s_retry_sess = sess;
        s_retry_flag = true;
        return 0;
    }
    if (b == CTRL_BUSY || b == CTRL_DONE || b == CTRL_ERR) {
        s_ctrl_session = sess;
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
    s_link_ok = false;
    s_sub = false;
    s_ping_seen = false;
    if (!s_rec_offline) s_recording = false;       // a recording to flash does not need the link
    s_conn = BLE_HS_CONN_HANDLE_NONE;
}

// The link is usable only when subscribed, encrypted, the MTU fits a frame, and
// the Mac helper is actually alive. macOS keeps the subscription (and the HID
// connection) after the helper quits, so the heartbeat is what tells them apart.
static bool link_compute(void)
{
    if (!s_sub || s_conn == BLE_HS_CONN_HANDLE_NONE) return false;
    if (!s_ping_seen || (xTaskGetTickCount() - s_last_ping) > pdMS_TO_TICKS(PEER_ALIVE_MS)) return false;
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(s_conn, &d) != 0 || !d.sec_state.encrypted) return false;
    return ble_att_mtu(s_conn) >= MIN_ATT_MTU;
}

// The display and button tasks must never call into the NimBLE host (ble_gap_*,
// ble_att_*): it takes the host lock, and the recording task holds that lock
// while it notifies. They read this cached answer instead.
static void link_refresh(void) { s_link_ok = link_compute(); }
static bool link_ready(void)   { return s_link_ok; }

// Live audio gives up quickly (late audio is useless); stored recordings keep
// retrying so nothing is lost when the radio buffers are momentarily full.
static bool send_frame_n(const uint8_t *f, size_t n, int max_tries, bool abortable)
{
    for (int tries = 0; tries < max_tries; tries++) {
        // Uploads of stored recordings step aside for a new recording or a screen change.
        if (abortable && (s_recording || !s_sync_ok)) return false;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(f, n);
        if (!om) { vTaskDelay(1)    /* one tick: 5 ms rounds to 0 at 100 Hz, which spins */; continue; }
        vox_crumb = 34;
        int rc = ble_gatts_notify_custom(s_conn, s_audio_handle, om);   // consumes om
        if (rc == 0) return true;
        s_last_rc = rc;
        vox_crumb = 33;
        if (rc == BLE_HS_ENOTCONN) return false;       // gone; anything else may clear up
        vTaskDelay(1)    /* one tick: 5 ms rounds to 0 at 100 Hz, which spins */;
    }
    return false;
}

static bool send_frame(const uint8_t *f, size_t n)        { return send_frame_n(f, n, 4, false); }
static bool send_frame_stored(const uint8_t *f, size_t n) { return send_frame_n(f, n, 400, true); }   // 400 ticks = up to ~4 s

static void fill_header(uint8_t *f, uint8_t type, uint16_t seq, int pred, int idx, uint8_t flags)
{
    memset(f, 0, HEADER_LEN);
    f[0] = 1;
    f[1] = type;
    f[2] = s_session & 0xFF; f[3] = s_session >> 8;
    f[4] = seq & 0xFF;       f[5] = seq >> 8;
    f[6] = 1;                                   // codec: IMA ADPCM 16 kHz mono
    f[7] = flags;
    uint16_t p = (uint16_t)(int16_t)pred;
    f[8] = p & 0xFF; f[9] = p >> 8; f[10] = (uint8_t)idx;
}

// The end frame carries the final sequence number so the Mac can tell whether
// anything was lost; repeat it because notifications can drop.
static void send_end(uint8_t *frame, uint16_t seq, uint8_t flags)
{
    for (int i = 0; i < 3; i++) {
        fill_header(frame, TYPE_END, seq, 0, 0, flags);
        send_frame(frame, HEADER_LEN);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Random start so a reply meant for a session before a reboot cannot match; 0 means "unknown".
static uint16_t next_session(void)
{
    uint16_t s = s_session + 1;
    return s ? s : 1;
}

static void record_session(void)
{
    vox_crumb = 10;
    static int16_t pcm[SAMPLES_PER_FRAME];
    uint8_t frame[FRAME_LEN];
    bool off = s_rec_offline;
    uint8_t flags = off ? FLAG_OFFLINE : 0;
    adpcm_t st = {0, 0};
    uint16_t seq = 0;
    int dropped = 0;

    if (off) {
        if (!inbox_begin()) { s_recording = false; return; }
    } else {
        s_session = next_session();
        fill_header(frame, TYPE_START, seq++, 0, 0, flags);
        if (!send_frame(frame, HEADER_LEN)) dropped++;
    }
    const uint16_t max_frames = off ? INBOX_MAX_FRAMES : MAX_FRAMES;

    while (s_recording && seq < max_frames) {
        vox_hb_rec++;
        if (!off && (seq & 15) == 0) link_refresh();
        int n = s_reader ? s_reader(pcm, SAMPLES_PER_FRAME) : -1;
        if (n < 0) { ESP_LOGW(TAG, "mic read failed"); break; }
        for (int i = n; i < SAMPLES_PER_FRAME; i++) pcm[i] = 0;
        int pred0 = st.pred, idx0 = st.idx;
        uint8_t *data = frame + HEADER_LEN;
        for (int i = 0; i < SAMPLES_PER_FRAME / 2; i++) {
            uint8_t lo = adpcm_encode(pcm[2 * i], &st);
            uint8_t hi = adpcm_encode(pcm[2 * i + 1], &st);
            data[i] = lo | (hi << 4);
        }
        if (off) {
            if (!inbox_append((int16_t)pred0, (uint8_t)idx0, data)) { ESP_LOGW(TAG, "flash write failed"); dropped++; break; }
            seq++;
        } else {
            fill_header(frame, TYPE_AUDIO, seq++, pred0, idx0, flags);
            if (!send_frame(frame, FRAME_LEN)) dropped++;
        }
    }
    s_recording = false;
    vox_crumb = 12;

    if (off) {
        bool ok = inbox_finish();
        ESP_LOGI(TAG, "offline recording %s: %u frames", ok ? "saved" : "discarded", seq);
    } else {
        send_end(frame, seq, flags);
        ESP_LOGI(TAG, "session %u: %u frames, %d dropped", s_session, seq, dropped);
    }
}

// Uploads the oldest stored recording. Returns when it is acknowledged, the
// link goes away, or the user starts recording (the slot then stays queued).
static void sync_fail(uint8_t reason)
{
    s_syncing = false;
    s_sync_reason = reason;
    s_sync_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(5000);   // no retry storms
}

static void sync_step(void)
{
    if (s_ack_slot >= 0) return;                       // sent already, waiting for the Mac
    if (!s_sync_ok || s_recording || !link_ready() || inbox_pending() == 0) { s_syncing = false; return; }
    if ((int32_t)(xTaskGetTickCount() - s_sync_retry_at) < 0) return;
    if (s_ctrl_status == CTRL_BUSY && (int32_t)(xTaskGetTickCount() - s_ctrl_until) < 0) return;   // Mac is busy
    int slot; uint32_t nf;
    if (!inbox_oldest(&slot, &nf)) { s_syncing = false; return; }

    static uint8_t frame[FRAME_LEN];
    vox_crumb = 30;
    s_syncing = true;
    s_sync_tries++;
    s_session = next_session();
    s_ctrl_status = 0;
    s_ctrl_session = 0;
    uint16_t seq = 0;
    fill_header(frame, TYPE_START, seq++, 0, 0, FLAG_OFFLINE);
    if (!send_frame_stored(frame, HEADER_LEN)) { sync_fail(2); return; }
    for (uint32_t i = 0; i < nf; i++) {
        vox_hb_rec++;
        if ((i & 15) == 0) link_refresh();
        if (s_recording) { sync_fail(4); return; }
        if (!link_ready()) { sync_fail(3); return; }
        int16_t pred; uint8_t idx;
        if (!inbox_read(slot, i, &pred, &idx, frame + HEADER_LEN)) { sync_fail(1); return; }
        fill_header(frame, TYPE_AUDIO, seq++, pred, idx, FLAG_OFFLINE);
        if (!send_frame_stored(frame, FRAME_LEN)) { sync_fail(2); return; }   // link trouble: keep it queued
        vTaskDelay(1);                           // one tick (10 ms): about twice real time
    }
    vox_crumb = 31;
    // Forget older replies first: only an answer that comes after the end frame counts.
    s_ctrl_status = 0;
    s_ctrl_session = 0;
    s_retry_flag = false;
    send_end(frame, seq, FLAG_OFFLINE);
    s_ack_slot = slot;
    s_ack_sess = s_session;
    s_ack_start = xTaskGetTickCount();
    s_ack_until = s_ack_start + pdMS_TO_TICKS(ACK_WAIT_MS);
}

// Called every round of the task: settles the recording that is waiting for the Mac.
static void ack_service(void)
{
    if (s_ack_slot < 0) return;
    TickType_t now = xTaskGetTickCount();
    uint8_t why = 0;
    if (s_ctrl_session == s_ack_sess && s_ctrl_status == CTRL_DONE) {
        vox_crumb = 32;
        inbox_delete(s_ack_slot);
        ESP_LOGI(TAG, "slot %d uploaded", s_ack_slot);
        s_ack_slot = -1;
        s_syncing = false;
        s_sync_reason = 5;
        return;
    }
    if (s_retry_flag && s_retry_sess == s_ack_sess) why = 8;                       // Mac got only part of it
    else if (s_ctrl_session == s_ack_sess && s_ctrl_status == CTRL_BUSY &&
             (int32_t)(now - s_ack_start) < (int32_t)pdMS_TO_TICKS(ACK_MAX_MS)) {
        TickType_t ext = now + pdMS_TO_TICKS(30000);                               // Mac is still working on it
        if ((int32_t)(ext - s_ack_until) > 0) s_ack_until = ext;
    }
    if (!why && (int32_t)(now - s_ack_until) >= 0) why = 6;
    if (!why && !link_ready()) why = 3;
    if (why) {
        s_ack_slot = -1;
        s_retry_flag = false;
        sync_fail(why);
    }
}

static void rec_task(void *arg)
{
    (void)arg;
    for (;;) {
        vox_hb_rec++;
        link_refresh();
        ack_service();
        // Short idle tick while there is erasing to do or a Mac to talk to, slow otherwise.
        TickType_t wait = (s_maint_ok && inbox_maintain_needed()) ? pdMS_TO_TICKS(60) : pdMS_TO_TICKS(1000);
        if (s_sub && wait > pdMS_TO_TICKS(200)) wait = pdMS_TO_TICKS(200);
        if (ulTaskNotifyTake(pdTRUE, wait) == 0) {
            if (s_maint_ok && !s_recording) { vox_crumb = 20; inbox_maintain(); }
            vox_crumb = 0;
            sync_step();
            continue;
        }
        if (s_recording) record_session();
    }
}

void btmic_init(btmic_read_fn reader)
{
    s_reader = reader;
    s_session = (uint16_t)esp_random();
    inbox_init();
    if (!s_task) xTaskCreate(rec_task, "btmic", 6144, NULL, 5, &s_task);
}

void btmic_set_mode(bool sync_ok, bool maintain_ok)
{
    s_sync_ok = sync_ok;
    s_maint_ok = maintain_ok;
}

int btmic_pending(void) { return inbox_pending(); }

// "S<reason>-<tries>" for the NOTE screen: why the last upload ended and how many were attempted.
void btmic_debug(char *buf, size_t n)
{
    snprintf(buf, n, "S%u-%u R%d", (unsigned)s_sync_reason, (unsigned)(s_sync_tries % 100), s_last_rc);
}

bool btmic_start(bool offline)
{
    if (s_recording || !s_task) return false;
    if (offline) {
        if (!inbox_has_free()) {                 // no erased slot (yet)
            s_ctrl_status = CTRL_FULL;
            s_ctrl_until = xTaskGetTickCount() + pdMS_TO_TICKS(STATUS_HOLD_MS);
            return false;
        }
        s_rec_offline = true;
    } else {
        if (!link_ready()) return false;         // live needs the Mac helper listening
        s_rec_offline = false;
    }
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
    if (s_recording) return s_rec_offline ? BTMIC_UI_REC_OFF : BTMIC_UI_REC;
    if (s_ctrl_status != 0 && (int32_t)(xTaskGetTickCount() - s_ctrl_until) < 0) {
        switch (s_ctrl_status) {
        case CTRL_BUSY: return BTMIC_UI_BUSY;
        case CTRL_DONE: return BTMIC_UI_DONE;
        case CTRL_FULL: return BTMIC_UI_FULL;
        default:        return BTMIC_UI_ERR;
        }
    }
    if (s_syncing) return BTMIC_UI_SYNC;
    return link_ready() ? BTMIC_UI_READY : BTMIC_UI_NOLINK;
}

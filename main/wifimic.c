// Part of the launcher firmware; shared declarations are in vox_internal.h.
#include "vox_internal.h"
#include "wifimic.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"

// =========================================================================
// WIFI — WHISPER over Wi-Fi, direct TCP on the local network (docs/WIFI-PLAN.md, appendix).
//
// Settings (Wi-Fi name and password, pairing key, PC address on the LAN) are typed on a
// phone: with nothing saved the stick opens a WPA2 hotspot whose password is shown on
// the screen, and serves a form at 192.168.4.1. Saving reboots the stick.
//
// Wire format on the TCP connection to winvoice (both directions little-endian):
//   stick -> PC: u16 length, then one docs/BT-MIC.md frame (start / audio / end)
//   PC -> stick: 4 bytes 'C', status, session lo, session hi
//                (status as BT-MIC ctrl_rx: 1 BUSY, 2 DONE, 3 ERR, 5 PING)
// =========================================================================
static const char *WTAG = "wifimic";

#define NVS_NS            "wifimic"
#define DEFAULT_PORT      47100
#define SAMPLES_PER_FRAME 320                // 20 ms at 16 kHz
#define HEADER_LEN        12
#define FRAME_LEN         (HEADER_LEN + SAMPLES_PER_FRAME / 2)
#define MAX_FRAMES        3000               // 60 s safety stop
#define PEER_ALIVE_MS     6000               // no PING from the PC for this long = no link
#define STATUS_HOLD_MS    3000
#define BUSY_HOLD_MS      60000

enum { TYPE_AUDIO = 1, TYPE_END = 2, TYPE_START = 3 };
enum { CTRL_BUSY = 1, CTRL_DONE = 2, CTRL_ERR = 3, CTRL_PING = 5 };

typedef struct {
    char     ssid[33];
    char     pass[65];
    char     key[128];                       // pairing key (unused until the encryption step)
    char     host[64];                       // PC: LAN IPv4 address or host name
    uint16_t port;
} wcfg_t;

static wcfg_t            s_cfg;
static TaskHandle_t      s_task = NULL;
static EventGroupHandle_t s_ev = NULL;
#define EV_GOT_IP        BIT0
static volatile wifimic_ui_t s_base = WIFIMIC_UI_OFF;   // link state, set by the task
static volatile bool     s_recording = false;
static volatile uint8_t  s_ctrl = 0;
static volatile TickType_t s_ctrl_until = 0;
static volatile TickType_t s_last_ping = 0;
static uint16_t          s_session = 0;
static char              s_ap_ssid[33];
static char              s_ap_pass[9];
static volatile uint8_t  s_wifi_reason = 0;        // last disconnect reason (esp_wifi_types.h), 0 = none
#define MAX_SCAN 16
static wifi_ap_record_t  s_scan[MAX_SCAN];          // networks seen when the setup hotspot opened
static uint16_t          s_scan_n = 0;

// ---- settings ----------------------------------------------------------------
static void get_str(nvs_handle_t h, const char *k, char *out, size_t n)
{
    size_t len = n;
    if (nvs_get_str(h, k, out, &len) != ESP_OK) out[0] = '\0';
}

static bool cfg_load(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.port = DEFAULT_PORT;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    get_str(h, "ssid", s_cfg.ssid, sizeof(s_cfg.ssid));
    get_str(h, "pass", s_cfg.pass, sizeof(s_cfg.pass));
    get_str(h, "key", s_cfg.key, sizeof(s_cfg.key));
    get_str(h, "host", s_cfg.host, sizeof(s_cfg.host));
    uint16_t port;
    if (nvs_get_u16(h, "port", &port) == ESP_OK && port) s_cfg.port = port;
    nvs_close(h);
    return s_cfg.ssid[0] && s_cfg.host[0];
}

static bool setup_flag(bool clear)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_get_u8(h, "setup", &v);
    if (clear && v) { nvs_erase_key(h, "setup"); nvs_commit(h); }
    nvs_close(h);
    return v != 0;
}

void wifimic_request_setup(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "setup", 1);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(WTAG, "restarting into the setup hotspot");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

// ---- Wi-Fi ---------------------------------------------------------------------
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, EV_GOT_IP);
        s_wifi_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        ESP_LOGW(WTAG, "wifi disconnected, reason %d", s_wifi_reason);
        esp_wifi_connect();                    // the driver paces its own retries
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(WTAG, "wifi up, ip " IPSTR, IP2STR(&((ip_event_got_ip_t *)data)->ip_info.ip));
        s_wifi_reason = 0;
        xEventGroupSetBits(s_ev, EV_GOT_IP);
    }
}

static bool wifi_base_init(void)
{
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // our own NVS namespace holds the settings
    return true;
}

static bool wifi_sta_start(void)
{
    esp_netif_create_default_wifi_sta();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL);
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, s_cfg.ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_cfg.pass, sizeof(wc.sta.password) - 1);
    // Accept WPA/WPA2/WPA3 personal (phone hotspots and older routers vary).
    wc.sta.threshold.authmode = s_cfg.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;        // pick the strongest AP with this name
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return false;
    if (esp_wifi_start() != ESP_OK) return false;
    // Power save delays every packet by up to a DTIM interval; audio wants low latency.
    esp_wifi_set_ps(WIFI_PS_NONE);
    return true;
}

// ---- setup hotspot and form -----------------------------------------------------
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

// Copies form field `name` from an x-www-form-urlencoded body, decoded.
static void form_field(const char *body, const char *name, char *out, size_t n)
{
    out[0] = '\0';
    size_t nl = strlen(name);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, name, nl) == 0 && p[nl] == '=') {
            p += nl + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < n) {
                if (*p == '+') { out[o++] = ' '; p++; }
                else if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
                    out[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2])); p += 3;
                } else out[o++] = *p++;
            }
            out[o] = '\0';
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static const char k_form_head[] =
    "<!doctype html><html lang=zh-Hant><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>ohmystick Wi-Fi</title><style>body{font-family:sans-serif;margin:16px;max-width:480px}"
    "label{display:block;margin-top:12px;font-weight:bold}input{width:100%;font-size:16px;padding:6px;box-sizing:border-box}"
    "button{margin-top:18px;font-size:18px;padding:8px 20px}small{color:#666}</style>"
    "<h2>ohmystick Wi-Fi 設定</h2>";

static void html_escape(httpd_req_t *req, const char *s)
{
    char buf[8];
    for (; *s; s++) {
        if (*s == '"' || *s == '<' || *s == '>' || *s == '&') {
            snprintf(buf, sizeof(buf), "&#%d;", *s);
            httpd_resp_sendstr_chunk(req, buf);
        } else {
            httpd_resp_send_chunk(req, s, 1);
        }
    }
}

static esp_err_t form_get(httpd_req_t *req)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)s_cfg.port);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, k_form_head);
    httpd_resp_sendstr_chunk(req, "<form method=post action=/save>"
        "<label>Wi-Fi 名稱（2.4 GHz）</label><input name=ssid list=nets value=\"");
    html_escape(req, s_cfg.ssid);
    httpd_resp_sendstr_chunk(req, "\"><datalist id=nets>");
    for (int i = 0; i < s_scan_n; i++) {
        httpd_resp_sendstr_chunk(req, "<option value=\"");
        html_escape(req, (const char *)s_scan[i].ssid);
        httpd_resp_sendstr_chunk(req, "\">");
    }
    httpd_resp_sendstr_chunk(req, "</datalist><small>Stick 看得到的 Wi-Fi：");
    if (s_scan_n == 0) httpd_resp_sendstr_chunk(req, "（沒有）");
    for (int i = 0; i < s_scan_n; i++) {
        char sig[16];
        snprintf(sig, sizeof(sig), "（%d）", s_scan[i].rssi);
        if (i) httpd_resp_sendstr_chunk(req, "、");
        html_escape(req, (const char *)s_scan[i].ssid);
        httpd_resp_sendstr_chunk(req, sig);
    }
    httpd_resp_sendstr_chunk(req, "。不在清單上的網路 Stick 連不到（5 GHz 也看不到）。</small>");
    httpd_resp_sendstr_chunk(req, "<input type=hidden name=_ value=\"");
    httpd_resp_sendstr_chunk(req, "\"><label>Wi-Fi 密碼</label><input name=pass type=password>"
        "<small>留空 = 沿用已存的密碼</small>"
        "<label>配對密碼（加密用，目前可留空）</label><input name=key>"
        "<small>留空 = 沿用已存的密碼</small>"
        "<label>電腦的區網位址</label><input name=host placeholder=192.168.1.x value=\"");
    html_escape(req, s_cfg.host);
    httpd_resp_sendstr_chunk(req, "\"><label>winvoice 埠號</label><input name=port inputmode=numeric value=\"");
    httpd_resp_sendstr_chunk(req, port);
    httpd_resp_sendstr_chunk(req, "\"><button>儲存並重新開機</button></form>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t form_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_FAIL;
    }
    char body[1025];
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read failed"); return ESP_FAIL; }
        got += r;
    }
    body[got] = '\0';

    wcfg_t c = s_cfg;                         // empty password / key fields keep the saved ones
    char tmp[128], port[8];
    form_field(body, "ssid", c.ssid, sizeof(c.ssid));
    form_field(body, "pass", tmp, sizeof(c.pass));
    if (tmp[0]) strlcpy(c.pass, tmp, sizeof(c.pass));
    form_field(body, "key", tmp, sizeof(tmp));
    if (tmp[0]) strlcpy(c.key, tmp, sizeof(c.key));
    form_field(body, "host", c.host, sizeof(c.host));
    form_field(body, "port", port, sizeof(port));
    int pv = atoi(port);
    c.port = (pv > 0 && pv < 65536) ? (uint16_t)pv : DEFAULT_PORT;

    nvs_handle_t h;
    bool ok = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_set_str(h, "ssid", c.ssid) == ESP_OK && nvs_set_str(h, "pass", c.pass) == ESP_OK &&
             nvs_set_str(h, "key", c.key) == ESP_OK && nvs_set_str(h, "host", c.host) == ESP_OK &&
             nvs_set_u16(h, "port", c.port) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, k_form_head);
    httpd_resp_sendstr_chunk(req, ok ? "<p>已儲存。Stick 重新開機後，進入 Wi-Fi 聽寫即可連線。</p>"
                                     : "<p>儲存失敗，請再試一次。</p>");
    httpd_resp_sendstr_chunk(req, NULL);
    if (ok) {
        ESP_LOGI(WTAG, "settings saved (ssid %s, host %s:%u)", c.ssid, c.host, (unsigned)c.port);
        s_base = WIFIMIC_UI_SAVED;
        xTaskCreate(restart_task, "wm_restart", 2048, NULL, 5, NULL);
    }
    return ESP_OK;
}

static void setup_mode(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "OHMYSTICK-%02X%02X", mac[4], mac[5]);
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));

    esp_netif_create_default_wifi_ap();
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    strlcpy((char *)ap.ap.password, s_ap_pass, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.channel = 6;
    // AP+STA so the stick can list the networks it can actually hear (2.4 GHz only).
    esp_netif_create_default_wifi_sta();
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (esp_wifi_start() != ESP_OK) { ESP_LOGE(WTAG, "hotspot failed"); return; }
    if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
        uint16_t n = MAX_SCAN;
        if (esp_wifi_scan_get_ap_records(&n, s_scan) == ESP_OK) s_scan_n = n;
        ESP_LOGI(WTAG, "scan: %u networks", (unsigned)s_scan_n);
    }

    httpd_handle_t srv = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;
    if (httpd_start(&srv, &hc) != ESP_OK) { ESP_LOGE(WTAG, "http server failed"); return; }
    const httpd_uri_t get = { .uri = "/", .method = HTTP_GET, .handler = form_get };
    const httpd_uri_t post = { .uri = "/save", .method = HTTP_POST, .handler = form_post };
    httpd_register_uri_handler(srv, &get);
    httpd_register_uri_handler(srv, &post);
    s_base = WIFIMIC_UI_SETUP;
    ESP_LOGI(WTAG, "setup hotspot %s up, form at http://192.168.4.1/", s_ap_ssid);
}

// ---- link to winvoice ---------------------------------------------------------------
static void fill_header(uint8_t *f, uint8_t type, uint16_t seq, int pred, int idx)
{
    memset(f, 0, HEADER_LEN);
    f[0] = 1;
    f[1] = type;
    f[2] = s_session & 0xFF; f[3] = s_session >> 8;
    f[4] = seq & 0xFF;       f[5] = seq >> 8;
    f[6] = 1;                                  // codec: IMA ADPCM 16 kHz mono
    uint16_t p = (uint16_t)(int16_t)pred;
    f[8] = p & 0xFF; f[9] = p >> 8; f[10] = (uint8_t)idx;
}

// ---- TCP to winvoice (plain lwIP sockets; the encryption step wraps these) ----------
static int net_connect(const char *host, uint16_t port, int timeout_ms)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char ps[8];
    snprintf(ps, sizeof(ps), "%u", (unsigned)port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set wf;
        FD_ZERO(&wf); FD_SET(fd, &wf);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        int err = 0;
        socklen_t el = sizeof(err);
        if (select(fd + 1, NULL, &wf, NULL, &tv) > 0 &&
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) rc = 0;
        else { if (err) ESP_LOGW(WTAG, "connect failed, errno %d", err); rc = -1; }
    }
    if (rc < 0) { close(fd); return -1; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static bool net_send(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        int w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            return false;
        }
        p += w; n -= (size_t)w;
    }
    return true;
}

// >0 bytes read, 0 nothing within the timeout, -1 connection closed or broken.
static int net_recv(int fd, uint8_t *buf, size_t n, int timeout_ms)
{
    fd_set rf;
    FD_ZERO(&rf); FD_SET(fd, &rf);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int s = select(fd + 1, &rf, NULL, NULL, &tv);
    if (s < 0) return -1;
    if (s == 0) return 0;
    int r = recv(fd, buf, n, 0);
    if (r > 0) return r;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;                                 // 0 = the PC closed the connection
}

static bool send_frame(int sock, const uint8_t *f, size_t n)
{
    uint8_t buf[2 + FRAME_LEN];
    buf[0] = n & 0xFF; buf[1] = n >> 8;
    memcpy(buf + 2, f, n);
    return net_send(sock, buf, n + 2);
}

static void set_ctrl(uint8_t st)
{
    s_ctrl = st;
    s_ctrl_until = xTaskGetTickCount() + pdMS_TO_TICKS(st == CTRL_BUSY ? BUSY_HOLD_MS : STATUS_HOLD_MS);
}

// Reads whatever the PC sent; false when the connection is gone.
static bool poll_ctrl(int sock, int timeout_ms)
{
    static uint8_t acc[4];
    static int have = 0;
    uint8_t buf[64];
    int n = net_recv(sock, buf, sizeof(buf), timeout_ms);
    if (n < 0) { have = 0; return false; }
    for (int i = 0; i < n; i++) {
        if (have == 0 && buf[i] != 'C') continue;  // resync on the marker
        acc[have++] = buf[i];
        if (have < 4) continue;
        have = 0;
        uint8_t st = acc[1];
        uint16_t sess = acc[2] | (acc[3] << 8);
        s_last_ping = xTaskGetTickCount();
        if (st == CTRL_PING) continue;
        if (sess == 0 || sess == s_session) set_ctrl(st);
    }
    return true;
}

static bool peer_alive(void)
{
    return s_last_ping != 0 &&
           (int32_t)(xTaskGetTickCount() - s_last_ping) < (int32_t)pdMS_TO_TICKS(PEER_ALIVE_MS);
}

static bool record_session(int sock)
{
    static int16_t pcm[SAMPLES_PER_FRAME];
    uint8_t frame[FRAME_LEN];
    vox_adpcm_t st = {0, 0};
    uint16_t seq = 0;
    bool ok = true;

    s_session = (uint16_t)(s_session + 1) ? (uint16_t)(s_session + 1) : 1;
    s_ctrl = 0;
    fill_header(frame, TYPE_START, seq++, 0, 0);
    ok = send_frame(sock, frame, HEADER_LEN);
    while (ok && s_recording && seq < MAX_FRAMES) {
        int n = btmic_pcm_read(pcm, SAMPLES_PER_FRAME);
        if (n < 0) { ESP_LOGW(WTAG, "mic read failed"); break; }
        for (int i = n; i < SAMPLES_PER_FRAME; i++) pcm[i] = 0;
        int pred0 = st.pred, idx0 = st.idx;
        uint8_t *data = frame + HEADER_LEN;
        for (int i = 0; i < SAMPLES_PER_FRAME / 2; i++) {
            uint8_t lo = vox_adpcm_encode(pcm[2 * i], &st);
            uint8_t hi = vox_adpcm_encode(pcm[2 * i + 1], &st);
            data[i] = lo | (hi << 4);
        }
        fill_header(frame, TYPE_AUDIO, seq++, pred0, idx0);
        ok = send_frame(sock, frame, FRAME_LEN);
    }
    s_recording = false;
    if (ok) {
        fill_header(frame, TYPE_END, seq, 0, 0);   // TCP is reliable: one end frame is enough
        ok = send_frame(sock, frame, HEADER_LEN);
    }
    ESP_LOGI(WTAG, "session %u: %u frames, %s", s_session, seq, ok ? "sent" : "connection lost");
    if (ok) set_ctrl(CTRL_BUSY);
    return ok;
}

static void link_loop(void)
{
    s_base = WIFIMIC_UI_WIFI;
    while (!(xEventGroupWaitBits(s_ev, EV_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(1000)) & EV_GOT_IP)) {}

    int sock = -1;
    for (;;) {
        if (!(xEventGroupGetBits(s_ev) & EV_GOT_IP)) {
            s_base = WIFIMIC_UI_WIFI;
            if (sock >= 0) { close(sock); sock = -1; s_last_ping = 0; }
        } else {
            if (sock < 0) {
                s_base = WIFIMIC_UI_PEER;
                sock = net_connect(s_cfg.host, s_cfg.port, 5000);
                if (sock >= 0) { ESP_LOGI(WTAG, "connected to %s:%u", s_cfg.host, (unsigned)s_cfg.port); s_last_ping = 0; }
            }
            if (sock >= 0) {
                bool alive = poll_ctrl(sock, 50);   // short: a button press starts recording within 50 ms
                if (alive && s_recording) alive = record_session(sock);
                if (alive && s_last_ping != 0 && !peer_alive()) alive = false;   // PC went quiet
                if (!alive) {
                    ESP_LOGW(WTAG, "link to the PC lost");
                    close(sock);
                    sock = -1;
                    s_last_ping = 0;
                    if (s_recording) { s_recording = false; set_ctrl(CTRL_ERR); }
                }
                s_base = (sock >= 0 && peer_alive()) ? WIFIMIC_UI_READY : WIFIMIC_UI_PEER;
                continue;                      // poll_ctrl already waited
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void wifimic_task(void *arg)
{
    (void)arg;
    s_session = (uint16_t)esp_random();
    if (!wifi_base_init()) { ESP_LOGE(WTAG, "wifi init failed"); vTaskDelete(NULL); return; }
    bool configured = cfg_load();
    if (setup_flag(true) || !configured) {
        setup_mode();
        vTaskDelete(NULL);                     // the http server task takes over
        return;
    }
    if (!wifi_sta_start()) { ESP_LOGE(WTAG, "wifi start failed"); vTaskDelete(NULL); return; }
    link_loop();
    vTaskDelete(NULL);
}

// ---- API for the app ----------------------------------------------------------------
void wifimic_enter(void)
{
    if (s_task) return;                        // the radio stays up until the next reboot
    s_ev = xEventGroupCreate();
    xTaskCreate(wifimic_task, "wifimic", 8192, NULL, 5, &s_task);
}

wifimic_ui_t wifimic_ui_state(void)
{
    if (s_recording) return WIFIMIC_UI_REC;
    wifimic_ui_t b = s_base;
    if (b == WIFIMIC_UI_READY || b == WIFIMIC_UI_PEER) {
        if (s_ctrl && (int32_t)(xTaskGetTickCount() - s_ctrl_until) < 0) {
            if (s_ctrl == CTRL_BUSY) return WIFIMIC_UI_BUSY;
            if (s_ctrl == CTRL_DONE) return WIFIMIC_UI_DONE;
            return WIFIMIC_UI_ERR;
        }
    }
    return b;
}

bool wifimic_start(void)
{
    if (s_base != WIFIMIC_UI_READY || s_recording) return false;
    s_recording = true;
    return true;
}

void wifimic_stop(void)      { s_recording = false; }
bool wifimic_recording(void) { return s_recording; }

void wifimic_detail(char *a, size_t an, char *b, size_t bn)
{
    a[0] = b[0] = '\0';
    switch (s_base) {
    case WIFIMIC_UI_SETUP:
        snprintf(a, an, "%s", s_ap_ssid);
        snprintf(b, bn, "PW %s", s_ap_pass);
        break;
    case WIFIMIC_UI_WIFI: {
        snprintf(a, an, "%s", s_cfg.ssid);
        uint8_t r = s_wifi_reason;
        const char *why = "";
        if (r == WIFI_REASON_NO_AP_FOUND || r == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
            r == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD || r == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD)
            why = "找不到這個 Wi-Fi";
        else if (r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT ||
                 r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_MIC_FAILURE || r == 15)
            why = "密碼可能錯誤";
        if (r && why[0]) snprintf(b, bn, "%s(%u)", why, r);
        else if (r)      snprintf(b, bn, "代碼 %u", r);
        break;
    }
    case WIFIMIC_UI_TAILNET:
    case WIFIMIC_UI_PEER:
        snprintf(a, an, "%s", s_cfg.host);
        break;
    default:
        snprintf(a, an, "%s", s_cfg.host);
        snprintf(b, bn, "LAN");
        break;
    }
}

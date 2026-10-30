#include "inbox.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "inbox";

#define SECTOR        4096U
#define SLOT_SIZE     0x40000U                     // 256 KB: 1 header sector + payload
#define FRAME_STORED  164U                         // pred(2) idx(1) pad(1) + 160 ADPCM
#define MAGIC_CLEAN   0x314E4C43U                  // "CLN1": slot erased and ready
#define MAGIC_USED    0x44455355U                  // "USED": recording started
#define MAGIC_DONE    0x454E4F44U                  // "DONE": recording sealed
#define MAX_SLOTS     16

// Header sector words: [0]=CLEAN [1]=USED [2]=DONE [3]=nframes [4]=seq.
// Words are programmed one after another without erasing, so a slot is
// - CLEAN   : word0 only
// - PENDING : word1 and word2 present
// - anything else needs a full erase (including a recording cut short by power loss).
enum { S_DIRTY = 0, S_CLEAN, S_RECORDING, S_PENDING };

static const esp_partition_t *s_part;
static int      s_slots;
static uint8_t  s_state[MAX_SLOTS];
static uint32_t s_seq[MAX_SLOTS];
static uint32_t s_nframes[MAX_SLOTS];
static uint16_t s_erase_pos[MAX_SLOTS];            // next sector to erase (0..64)
static uint32_t s_next_seq = 1;
static int      s_cur = -1;
static uint32_t s_cur_frames = 0;

static size_t slot_off(int s) { return (size_t)s * SLOT_SIZE; }

static bool prog32(size_t off, uint32_t v)
{
    return esp_partition_write(s_part, off, &v, sizeof(v)) == ESP_OK;
}

void inbox_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "inbox");
    if (!s_part) { ESP_LOGW(TAG, "no inbox partition: offline recording disabled"); return; }
    s_slots = (int)(s_part->size / SLOT_SIZE);
    if (s_slots > MAX_SLOTS) s_slots = MAX_SLOTS;
    for (int i = 0; i < s_slots; i++) {
        uint32_t h[5];
        s_erase_pos[i] = 0;
        s_state[i] = S_DIRTY;
        if (esp_partition_read(s_part, slot_off(i), h, sizeof(h)) != ESP_OK) continue;
        if (h[0] == MAGIC_CLEAN && h[1] == 0xFFFFFFFFU) {
            s_state[i] = S_CLEAN;
        } else if (h[0] == MAGIC_CLEAN && h[1] == MAGIC_USED && h[2] == MAGIC_DONE &&
                   h[3] >= 1 && h[3] <= INBOX_MAX_FRAMES) {
            s_state[i] = S_PENDING;
            s_nframes[i] = h[3];
            s_seq[i] = h[4];
            if (h[4] >= s_next_seq) s_next_seq = h[4] + 1;
        }
    }
    ESP_LOGI(TAG, "%d slots, %d pending", s_slots, inbox_pending());
}

int inbox_pending(void)
{
    int n = 0;
    for (int i = 0; i < s_slots; i++) if (s_state[i] == S_PENDING) n++;
    return n;
}

static int find_state(int st)
{
    for (int i = 0; i < s_slots; i++) if (s_state[i] == st) return i;
    return -1;
}

bool inbox_has_free(void) { return s_part && find_state(S_CLEAN) >= 0; }

bool inbox_begin(void)
{
    if (!s_part || s_cur >= 0) return false;
    int s = find_state(S_CLEAN);
    if (s < 0) return false;
    if (!prog32(slot_off(s) + 4, MAGIC_USED)) { s_state[s] = S_DIRTY; return false; }
    s_state[s] = S_RECORDING;
    s_cur = s;
    s_cur_frames = 0;
    return true;
}

bool inbox_append(int16_t pred, uint8_t idx, const uint8_t adpcm[160])
{
    if (s_cur < 0 || s_cur_frames >= INBOX_MAX_FRAMES) return false;
    uint8_t rec[FRAME_STORED];
    rec[0] = (uint16_t)pred & 0xFF; rec[1] = (uint16_t)pred >> 8; rec[2] = idx; rec[3] = 0;
    memcpy(rec + 4, adpcm, 160);
    size_t off = slot_off(s_cur) + SECTOR + (size_t)s_cur_frames * FRAME_STORED;
    if (esp_partition_write(s_part, off, rec, sizeof(rec)) != ESP_OK) return false;
    s_cur_frames++;
    return true;
}

void inbox_abort(void)
{
    if (s_cur >= 0) {
        s_state[s_cur] = S_DIRTY;     // payload is dirty; maintenance will erase it
        s_erase_pos[s_cur] = 0;
        s_cur = -1;
    }
}

bool inbox_finish(void)
{
    if (s_cur < 0) return false;
    int s = s_cur;
    s_cur = -1;
    if (s_cur_frames < INBOX_MIN_FRAMES) {
        s_state[s] = S_DIRTY; s_erase_pos[s] = 0;
        return false;
    }
    uint32_t meta[2] = { s_cur_frames, s_next_seq };
    if (esp_partition_write(s_part, slot_off(s) + 12, meta, sizeof(meta)) != ESP_OK ||
        !prog32(slot_off(s) + 8, MAGIC_DONE)) {
        s_state[s] = S_DIRTY; s_erase_pos[s] = 0;
        return false;
    }
    s_nframes[s] = s_cur_frames;
    s_seq[s] = s_next_seq++;
    s_state[s] = S_PENDING;
    ESP_LOGI(TAG, "slot %d sealed: %u frames, seq %u", s, (unsigned)s_nframes[s], (unsigned)s_seq[s]);
    return true;
}

bool inbox_oldest(int *slot, uint32_t *nframes)
{
    int best = -1;
    for (int i = 0; i < s_slots; i++) {
        if (s_state[i] == S_PENDING && (best < 0 || s_seq[i] < s_seq[best])) best = i;
    }
    if (best < 0) return false;
    *slot = best;
    *nframes = s_nframes[best];
    return true;
}

bool inbox_read(int slot, uint32_t i, int16_t *pred, uint8_t *idx, uint8_t adpcm[160])
{
    if (slot < 0 || slot >= s_slots || s_state[slot] != S_PENDING || i >= s_nframes[slot]) return false;
    uint8_t rec[FRAME_STORED];
    if (esp_partition_read(s_part, slot_off(slot) + SECTOR + (size_t)i * FRAME_STORED,
                           rec, sizeof(rec)) != ESP_OK) return false;
    *pred = (int16_t)(rec[0] | (rec[1] << 8));
    *idx = rec[2];
    memcpy(adpcm, rec + 4, 160);
    return true;
}

void inbox_delete(int slot)
{
    if (slot < 0 || slot >= s_slots || s_state[slot] != S_PENDING) return;
    // Erase the header first: from here on the slot can never read as PENDING.
    esp_partition_erase_range(s_part, slot_off(slot), SECTOR);
    s_state[slot] = S_DIRTY;
    s_erase_pos[slot] = 1;                       // header sector already erased
}

// Housekeeping target: keep three erased slots ready (all dirty ones eventually).
bool inbox_maintain_needed(void)
{
    if (!s_part || s_cur >= 0) return false;
    int clean = 0, dirty = 0;
    for (int i = 0; i < s_slots; i++) {
        if (s_state[i] == S_CLEAN) clean++;
        else if (s_state[i] == S_DIRTY) dirty++;
    }
    return dirty > 0 && clean < 3;
}

static bool sector_blank(size_t off)
{
    uint8_t buf[256];
    for (size_t o = 0; o < SECTOR; o += sizeof(buf)) {
        if (esp_partition_read(s_part, off + o, buf, sizeof(buf)) != ESP_OK) return false;
        for (size_t k = 0; k < sizeof(buf); k++) if (buf[k] != 0xFF) return false;
    }
    return true;
}

void inbox_maintain(void)
{
    if (!inbox_maintain_needed()) return;
    int s = find_state(S_DIRTY);
    if (s < 0) return;
    size_t base = slot_off(s);
    uint32_t sectors = SLOT_SIZE / SECTOR;
    uint16_t pos = s_erase_pos[s];
    if (pos == 0) {                              // header sector first
        if (!sector_blank(base)) esp_partition_erase_range(s_part, base, SECTOR);
        s_erase_pos[s] = 1;
        return;
    }
    if (pos < sectors) {
        size_t off = base + (size_t)pos * SECTOR;
        if (!sector_blank(off)) esp_partition_erase_range(s_part, off, SECTOR);
        s_erase_pos[s] = pos + 1;
        return;
    }
    if (prog32(base, MAGIC_CLEAN)) {
        s_state[s] = S_CLEAN;
        ESP_LOGI(TAG, "slot %d erased", s);
    } else {
        s_erase_pos[s] = 0;                      // try again from scratch
    }
}

#pragma once
// Offline recordings for BT REC: IMA ADPCM frames kept in the raw "inbox"
// partition until the Mac helper is reachable (see docs/BT-MIC.md).
#include <stdbool.h>
#include <stdint.h>

#define INBOX_MAX_FRAMES 1500          // 30 s at 20 ms per frame
#define INBOX_MIN_FRAMES 15            // shorter than 0.3 s is discarded

void inbox_init(void);                 // locate the partition, scan the slots
int  inbox_pending(void);              // recordings waiting to be uploaded
bool inbox_has_free(void);             // an erased slot is ready for recording

bool inbox_begin(void);                // claim an erased slot
bool inbox_append(int16_t pred, uint8_t idx, const uint8_t adpcm[160]);
bool inbox_finish(void);               // seal; false (and discarded) if too short
void inbox_abort(void);

bool inbox_oldest(int *slot, uint32_t *nframes);
bool inbox_read(int slot, uint32_t i, int16_t *pred, uint8_t *idx, uint8_t adpcm[160]);
void inbox_delete(int slot);

// Erases at most one flash sector of housekeeping per call. Flash erases stall
// the CPU for tens of ms, so call it only while the stick is idle.
bool inbox_maintain_needed(void);
void inbox_maintain(void);

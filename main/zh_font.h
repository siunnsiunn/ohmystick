#pragma once
// 12 px bitmap font (Fusion Pixel Font, zh-TW, OFL-1.1) with only the characters
// the screens use. Regenerate with tools/font/gen_zh_font.py after adding text.
#include <stdint.h>

typedef struct {
    uint16_t cp;         // Unicode code point (sorted)
    uint8_t  w;          // cell width: 6 (half width) or 12 (full width)
    uint16_t rows[12];   // row y, bit x = column x from the left
} zh_glyph_t;

extern const zh_glyph_t k_zh_glyphs[];
extern const int k_zh_glyph_count;

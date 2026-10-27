/* SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE. */
/* vibe_font.h – glyph tables baked by tools/mkfont.py (internal to the SDK) */
#ifndef VIBE_FONT_H
#define VIBE_FONT_H
#include <stdint.h>

typedef struct { uint16_t cp; uint8_t adv; int8_t x, y; uint8_t w, h; uint32_t off; } vf_glyph;  /* x/y from pen/top */
typedef struct { uint8_t height, ascent; uint16_t count; const vf_glyph *g; } vf_font;

extern const uint8_t vf_bits[];          /* 4-bit alpha, two pixels per byte, row by row */
extern const vf_font vf_fonts[];

#endif

/* SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE. */
/*
 * vibe_gfx.h – drawing into the 480 x 320 RGB565 framebuffer that vibe_on_draw() receives.
 * Text uses anti-aliased fonts baked from JetBrains Mono and Archivo (tools/mkfont.py -> sdk/src/vibe_font.c).
 */
#ifndef VIBE_GFX_H
#define VIBE_GFX_H
#include "vibe.h"

enum vg_font { VG_SMALL, VG_BOLD, VG_MED, VG_BIG, VG_HUGE, VG_LOGO, VG_FONT_COUNT };
/* VG_SMALL  JetBrains Mono 12 px      labels, hints
   VG_BOLD   JetBrains Mono Bold 12 px headers, soft keys
   VG_MED    JetBrains Mono Bold 15 px names
   VG_BIG    JetBrains Mono Bold 22 px values
   VG_HUGE   JetBrains Mono Bold 60 px tempo
   VG_LOGO   Archivo ExtraBold 58 px boot logo */

enum vg_align { VG_LEFT, VG_CENTER, VG_RIGHT };

void vg_begin(uint16_t *fb);                                  /* every vg_* call draws into this framebuffer */
void vg_clear(uint32_t rgb);
void vg_rect(int x, int y, int w, int h, uint32_t rgb);
void vg_rect_a(int x, int y, int w, int h, uint32_t rgb, float alpha);
void vg_round(int x, int y, int w, int h, int r, uint32_t rgb);
void vg_frame(int x, int y, int w, int h, int t, uint32_t rgb);   /* outline, thickness t */
void vg_line(float x0, float y0, float x1, float y1, float width, uint32_t rgb);
void vg_circle(float cx, float cy, float r, uint32_t rgb);
void vg_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgb);
int  vg_text(const char *utf8, int x, int y, int font, uint32_t rgb, int align); /* y = top; returns width */
int  vg_text_width(const char *utf8, int font);
int  vg_font_height(int font);

/* tiny formatting helpers (no libc in the browser build) */
char *vg_itoa(int v, char *buf);                              /* returns buf */
char *vg_ftoa(float v, int decimals, char *buf);
uint32_t vg_mix(uint32_t a, uint32_t b, float t);             /* blend two 0xRRGGBB colours */

#endif

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

/* ---- SDK 10: FOCUS – every control on the display reachable with ONE knob and ONE button ---------------------------
 * For controllers with few controls (an MPK mini: its knob K5 = DATA, the knob pressed = ENTER) and for anyone who
 * prefers it: DATA turns a frame from control to control, ENTER (or DATA pressed) presses a button or starts editing a
 * value – then DATA changes it, ENTER or RETURN ends. The firmware's own direct controls (pads, encoders, F keys) stay.
 * Only in the firmware (compiled into the .wasm): the host knows nothing of it, every host runs it.
 *
 *   vibe_on_draw:   vf_begin();  then for each control where it is drawn:
 *                   vf_button(id, x, y, w, h)  or  vf_value(id, x, y, w, h, &value, min, max, step_per_detent);
 *                   vf_end(0xRRGGBB);          (draws the frame; the order of the calls = the order DATA walks)
 *   vibe_on_event:  first  int id, r = vf_event(e, &id);
 *                   r == VF_NO      not focus' business – handle e as usual
 *                   r == VF_MOVED   taken (the frame moved, editing began or ended)
 *                   r == VF_PRESS   button id pressed
 *                   r == VF_CHANGED value id changed (already written – do what a change needs: a filter's coefficients …)
 * vf_event takes: DATA turned (moves, or changes while editing), ENTER and DATA pressed, RETURN while editing.
 * vf_enable(0) while a page of the firmware uses DATA / ENTER itself (a list): everything passes, no frame is drawn.
 *
 * Two modes (vf_mode, in vibe_on_init):
 *   VF_ALWAYS   (default) DATA always walks – for a firmware whose DATA has no job of its own (the PLAYER's DJ).
 *   VF_ON_HOLD  DATA and ENTER stay the firmware's (choose a sound, a pattern …). HOLDING ENTER or DATA pressed for
 *               0.6 s turns FOCUS on (the frame appears on the first control); holding again or RETURN turns it off.
 *               The SDK holds ENTER / DATA-pressed back until it is let go: a short press reaches the firmware as
 *               always (down and up together), a long one never does. The same gesture in every such firmware. */
enum { VF_NO, VF_MOVED, VF_PRESS, VF_CHANGED };
enum { VF_ALWAYS, VF_ON_HOLD };
void vf_mode(int mode);
/* A control that stands for one of the device's own (the least work for an existing firmware – nothing else changes):
 *   vf_encoder(VE_1, x, y, w, h)  where encoder 1's value is shown: editing it, DATA turns arrive as VE_1 turns
 *   vf_key(VB_F1, x, y, w, h)     where F1's caption is shown: ENTER on it = F1 pressed and let go
 * Both go straight to vibe_on_event (vf_event answers VF_MOVED); not for VE_DATA / ENTER themselves. */
void vf_encoder(int encoder, int x, int y, int w, int h);
void vf_key(int button, int x, int y, int w, int h);
int  vf_active(void);                                         /* 1 while FOCUS is on (VF_ALWAYS: always) */
void vf_begin(void);
void vf_button(int id, int x, int y, int w, int h);
void vf_value(int id, int x, int y, int w, int h, float *value, float min, float max, float step);
void vf_end(uint32_t rgb);
int  vf_event(const vibe_event *e, int *id);
void vf_enable(int on);
int  vf_focus(void);                                          /* the id in focus (-1: none yet) */
int  vf_editing(void);                                        /* 1 while a value is being changed */
void vf_set_focus(int id);

#endif

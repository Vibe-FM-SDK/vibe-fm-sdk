/* SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE. */
/*
 * vibe_gfx.h – drawing into the 480 x 320 RGB565 framebuffer that vibe_on_draw() receives.
 * Text uses anti-aliased fonts baked from Roboto Condensed and Archivo (tools/mkfont.py -> sdk/src/vibe_font.c).
 */
#ifndef VIBE_GFX_H
#define VIBE_GFX_H
#include "vibe.h"

enum vg_font { VG_SMALL, VG_BOLD, VG_MED, VG_BIG, VG_HUGE, VG_LOGO, VG_FONT_COUNT };
/* VG_SMALL  Roboto Condensed Medium 14 px    labels, hints
   VG_BOLD   Roboto Condensed SemiBold 14 px  headers, soft keys
   VG_MED    Roboto Condensed SemiBold 15 px  names
   VG_BIG    Roboto Condensed SemiBold 22 px  values (digits are all the same width in every size)
   VG_HUGE   Roboto Condensed SemiBold 60 px  tempo, time – digits, capitals and . , : ; - – + / % # only
   VG_LOGO   Archivo ExtraBold 58 px          boot logo – the letters of "VIBE FM Mk1" and digits only
   (29.10.2026: Roboto Condensed instead of JetBrains Mono, 14 px instead of 12 – see tools/mkfont.py) */

#define VG_X2 0x100   /* (SDK 23) or it into a font: the same face twice the size, pixel for pixel – for the stage (VG_BIG | VG_X2) */
enum vg_align { VG_LEFT, VG_CENTER, VG_RIGHT };

void vg_begin(uint16_t *vibe_fb);                                 /* every vg_* call draws into this framebuffer (480 x 320) */
void vg_begin_size(uint16_t *fb, int w, int h);                   /* (SDK 23) … into a picture of this size – the stage (vibe_on_stage gets it set already) */
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

/* ---- HOME = what every control does (3.11.2026, Protokoll 3.2: the same in every firmware) ------------------------
 * HOME tapped shows the controls of the page open now; HOME again or RETURN closes it. HOME held 1 s or SHIFT+HOME opens
 * the firmware library – the host does that, so never call vibe_request(VIBE_REQ_FIRMWARE_MENU) for HOME.
 * The easy way: in vibe_on_draw, while open, one call draws the standard overview (up to 22 rows, two columns from 12):
 *   static const char *const ROWS[] = { "KEYS", "play the synth", "PADS", "drums", "ENCODER 1", "cutoff", ... };
 *   if (help) { vg_controls("SYNTH PAGE", ROWS, sizeof ROWS / sizeof ROWS[0] / 2); return; }
 * (rows = name / what pairs, n = the number of pairs). A firmware may draw its own overview instead (OberVave: the device
 * laid out, with the second layer under SHIFT). */
void vg_controls(const char *title, const char *const *rows, int n);

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
 *   SDK 18, opt-in with vf_clicks(1) in vibe_on_init: and a click into the display (VIBE_EV_TOUCH: plugin, browser) on one
 *   of the controls – a button is pressed, a slider (three times as wide as high) takes the value where it is clicked, a
 *   smaller value is edited (DATA, dragging). A click on no control stays VF_NO.
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
/* SHIFT + DATA walks FOCUS (30.10.2026, opt-in, for a VF_ON_HOLD firmware whose DATA has a job of its own – VOX): instead
 * of SDK 12's soft-key frame, SHIFT held + DATA turned shows the frame and walks EVERY control of the page (the soft keys
 * too, when the firmware registers them with vf_key), as VIBE DJ's DECKS do with DATA alone. DATA / ENTER pressed = edit
 * or press, as always. The frame stays when SHIFT is let go (DATA then walks it alone, the MPK's knob press works); it
 * goes with RETURN, ENTER held, or 6 s without a touch. Call once in vibe_on_init. */
void vf_shift_walk(int on);
void vf_clicks(int on);                                       /* SDK 18: clicks on the controls (opt-in, see above) */

#endif

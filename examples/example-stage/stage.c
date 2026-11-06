/* SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE. */
/*
 * EXAMPLE STAGE – the smallest firmware with a stage (SDK 23, ≈ 90 lines). A sine tone on the keys, one knob: CUTOFF.
 *   display  -> the knob's value, as every firmware shows it (480 x 320)
 *   stage    -> a second, bigger picture the firmware draws itself (960 x 540): a big dial you drag, a level meter.
 *               VIBE_STAGE_REPLACES: a host that can (the Web Player) opens with the stage IN PLACE of the device – the
 *               device is one key away (STAGE / Esc), keys and knobs play on. The VIBE and an older host show no stage:
 *               everything still works with encoder 1 and the display. That is the rule: the stage is never the only way.
 *   encoder 1 / drag the dial / mouse wheel on it -> CUTOFF
 */
#include "vibe.h"
#include "vibe_gfx.h"
#include "vibe_dsp.h"

static float sr, phase, freq = 220, amp, target, cutoff = 0.5f, level, lp;
static int last_key = -1, help, dragging;
static const char *const HELP[] = { "KEYS", "a tone", "ENCODER 1", "CUTOFF – the big dial on the stage too", "HOME", "this overview" };

#define SW 960
#define SH 540
static uint16_t stage[SW * SH];                 /* the stage's picture is ours: RGB565, 1 MB – a firmware without one spends nothing */

void vibe_on_init(float sample_rate) {
  sr = sample_rate;
  vibe_stage(stage, SW, SH, 0x101418, VIBE_STAGE_REPLACES);   /* 1 = taken; 0 only for a size out of 480 x 270 … 1280 x 720 */
}

static void set_cutoff(float v) { cutoff = vd_clamp(v, 0, 1); }

void vibe_on_event(const vibe_event *e) {
  if (e->type == VIBE_EV_KEY) {
    if (e->down) { last_key = e->id; freq = vd_mtof(48 + e->id); target = 0.3f * e->value; }
    else if (e->id == last_key) target = 0;
  }
  if (e->type == VIBE_EV_ENCODER && e->id == VE_1) set_cutoff(cutoff + e->value * 0.02f);
  if (e->type == VIBE_EV_BUTTON && e->down) { if (e->id == VB_HOME) help = !help; else if (e->id == VB_RETURN) help = 0; }
  if (e->type == VIBE_EV_STAGE) {               /* the stage's input: x / y in stage pixels; never on the VIBE */
    int x = VIBE_STAGE_X(e), y = VIBE_STAGE_Y(e), dx = x - SW / 2, dy = y - 250;
    if (e->id == VIBE_STAGE_DOWN) dragging = dx * dx + dy * dy < 170 * 170;   /* the dial: 170 px around its centre */
    if ((e->id == VIBE_STAGE_DOWN || e->id == VIBE_STAGE_MOVE) && dragging) set_cutoff(0.5f - (float)dy / 300.0f);   /* up = more */
    if (e->id == VIBE_STAGE_UP) dragging = 0;
    if (e->id == VIBE_STAGE_WHEEL) set_cutoff(cutoff + e->value * 0.02f);
  }
}

void vibe_on_audio(const float *in, float *l, float *r, int n) {
  (void)in;
  float k = 0.002f + cutoff * cutoff * 0.3f;     /* a one-pole low pass, opened by CUTOFF */
  for (int i = 0; i < n; i++) {
    amp += (target - amp) * 0.001f;
    phase += freq / sr; if (phase >= 1) phase -= 1;
    float s = (phase < 0.5f ? 1.0f : -1.0f) * amp;   /* a square, so the filter has something to do */
    lp += (s - lp) * k;
    l[i] = r[i] = lp;
    float a = lp < 0 ? -lp : lp; level = a > level ? a : level * 0.9995f;
  }
}

void vibe_on_draw(uint16_t *fb) {            /* the display: as every firmware – the value, a bar */
  vg_clear(0x101418);
  if (help) { vg_controls("EXAMPLE STAGE", HELP, 3); return; }
  vg_text("EXAMPLE STAGE", 240, 20, VG_BIG, 0xEFE8E1, VG_CENTER);
  vg_text("CUTOFF", 240, 120, VG_BOLD, 0xA8B5BC, VG_CENTER);
  char v[8]; vg_itoa((int)(cutoff * 100 + 0.5f), v); vg_text(v, 240, 150, VG_HUGE, 0xFF60BE, VG_CENTER);
  vg_rect(40, 250, 400, 10, 0x1A2326); vg_rect(40, 250, (int)(400 * cutoff), 10, 0xFF60BE);
  vg_text("encoder 1 - or the big dial on the stage", 240, 280, VG_SMALL, 0x66767E, VG_CENTER);
  (void)fb;
}

void vibe_on_stage(uint16_t *fb, int w, int h) {   /* the stage: only while a host shows it; vg_* draw in w x h already */
  (void)fb; (void)w; (void)h;
  vg_clear(0x101418);
  vg_text("EXAMPLE STAGE", SW / 2, 24, VG_BIG | VG_X2, 0xEFE8E1, VG_CENTER);    /* VG_X2: every font twice the size */
  const float cx = SW / 2, cy = 250, R = 150;
  vg_circle(cx, cy, R + 10, 0x1A2326); vg_circle(cx, cy, R, 0x22292D);
  for (int i = 0; i <= 20; i++) { float t = 0.375f + 0.75f * i / 20, s = vd_sin(t), c = vd_sin(t + 0.25f);   /* the arc: 270 degrees, from 7 o'clock */
    vg_line(cx + c * (R + 14), cy + s * (R + 14), cx + c * (R + 24), cy + s * (R + 24), 3, (float)i / 20 <= cutoff ? 0xFF60BE : 0x3A444A); }
  { float t = 0.375f + 0.75f * cutoff, s = vd_sin(t), c = vd_sin(t + 0.25f); vg_line(cx + c * 40, cy + s * 40, cx + c * (R - 12), cy + s * (R - 12), 8, dragging ? 0xFFFFFF : 0xFF60BE); }
  char v[8]; vg_itoa((int)(cutoff * 100 + 0.5f), v); vg_text(v, SW / 2, 420, VG_HUGE, 0xEFE8E1, VG_CENTER);
  vg_text("CUTOFF  -  drag the dial, or encoder 1 on the device", SW / 2, 496, VG_MED | VG_X2, 0xA8B5BC, VG_CENTER);
  int segs = (int)(level * 3.0f * 24); if (segs > 24) segs = 24;        /* the level, a meter at the right */
  for (int i = 0; i < 24; i++) vg_rect(SW - 80, 480 - i * 18, 40, 14, i < segs ? (i > 20 ? 0xFF6A1A : 0x4DB6AC) : 0x1A2326);
}

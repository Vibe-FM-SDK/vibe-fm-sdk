/* SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE. */
/*
 * HELLO – the smallest useful VIBE firmware (≈ 60 lines). Start here when you write your own.
 *   keys  -> a sine tone (one voice), velocity = loudness
 *   pads  -> light up in their colour while held
 *   encoder 1 -> pitch bend, display shows what happens
 */
#include "vibe.h"
#include "vibe_gfx.h"
#include "vibe_dsp.h"

static float sr, phase, freq = 440, amp, target, bend;
static int last_key = -1;
static int held[VIBE_PADS];

void vibe_on_init(float sample_rate) { sr = sample_rate; vibe_log("HELLO firmware started"); }

void vibe_on_event(const vibe_event *e) {
  if (e->type == VIBE_EV_KEY) {
    if (e->down) { last_key = e->id; freq = vd_mtof(60 + e->id); target = 0.3f * e->value; }
    else if (e->id == last_key) target = 0;
  }
  if (e->type == VIBE_EV_PAD) {
    held[e->id] = e->down;
    vibe_led_pad(e->id, 0xFF6A1A, e->down ? 1.0f : 0.0f);
  }
  if (e->type == VIBE_EV_ENCODER && e->id == VE_1) bend = vd_clamp(bend + e->value * 0.25f, -12, 12);
  if (e->type == VIBE_EV_BUTTON && e->id == VB_HOME && e->down) vibe_request(VIBE_REQ_FIRMWARE_MENU);
}

void vibe_on_audio(const float *in, float *l, float *r, int n) {
  float f = freq * vd_exp2(bend / 12.0f);
  for (int i = 0; i < n; i++) {
    amp += (target - amp) * 0.002f;                 /* smooth on/off, no clicks */
    phase += f / sr; if (phase >= 1) phase -= 1;
    l[i] = r[i] = vd_sin(phase) * amp;
  }
  (void)in;
}

void vibe_on_draw(uint16_t *fb) {
  char buf[16];
  (void)fb;
  vg_clear(0x0B1220);
  vg_text("HELLO VIBE", 240, 70, VG_BIG, 0xFF6A1A, VG_CENTER);
  vg_text("play the keys · encoder 1 = pitch · HOME = firmware menu", 240, 112, VG_SMALL, 0x8A93A6, VG_CENTER);
  vg_text(last_key < 0 ? "–" : vg_itoa(60 + last_key, buf), 240, 150, VG_HUGE, 0xE6EAF2, VG_CENTER);
  vg_text(vg_ftoa(bend, 2, buf), 240, 230, VG_MED, 0x6EE7A8, VG_CENTER);
  for (int i = 0; i < VIBE_PADS; i++) vg_round(84 + i * 20, 270, 14, 14, 3, held[i] ? 0xFF6A1A : 0x24314A);
}

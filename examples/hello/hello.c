/* SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE. */
/*
 * HELLO – the smallest useful VIBE firmware (≈ 75 lines). Start here when you write your own.
 *   keys  -> a sine tone (one voice), velocity = loudness
 *   pads  -> light up in their colour while held
 *   encoder 1 -> pitch bend, display shows what happens (and says where the knob stands: vibe_led_ring)
 *   K1 / K2   -> SINE / SQUARE – and the same by a tap on the two tabs under the header (touch, TOUCH.md)
 *   HOME      -> what every control does (every firmware: HOME tapped = this overview; HOME held 1 s or SHIFT+HOME =
 *                the firmware library – the host does that, never call VIBE_REQ_FIRMWARE_MENU for HOME)
 */
#include "vibe.h"
#include "vibe_gfx.h"
#include "vibe_dsp.h"

/* Two flows (a must since 13 Nov 2026): vibe_on_audio runs in its own thread, beside vibe_on_event / vibe_on_draw.
   Shared here: freq, target, bend – each a 32-bit value only vibe_on_event writes and vibe_on_audio only reads. That is
   all the rules ask for such values (README "Two flows"); anything bigger goes through a queue (example-synth). */
VIBE_PARALLEL_AUDIO

static float sr, phase, amp;                        /* the audio flow's own */
static volatile float freq = 440, target, bend;     /* written by events only, read by the audio */
static volatile int wave;                           /* 0 SINE, 1 SQUARE – the two tabs */
static int last_key = -1;
static int held[VIBE_PADS];
static int help;                                    /* HOME: the overview is open */
static const char *const HELP[] = { "KEYS", "a sine tone, velocity = loudness", "PADS", "light up while held",
  "ENCODER 1", "pitch bend +/- 12", "K1 / K2", "SINE / SQUARE (or tap the tab)", "HOME", "this overview" };

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
  if (e->type == VIBE_EV_BUTTON && e->down) {        /* buttons: pressed is only e->down (value is always 0) */
    if (e->id == VB_HOME) help = !help;               /* HOME = what every control does; HOME again / RETURN = back */
    else if (e->id == VB_RETURN) help = 0;
    else if (e->id == VB_K1 || e->id == VB_K2) wave = e->id == VB_K2;   /* the tabs by key: touch is never the only way */
  }
  /* touch (plugin, web player, the VIBE's touch screen / iPad page): a tap on the tab row under the header (y 40-68)
     opens that tab. Act on DOWN – in the knob columns a tap arrives as DOWN + UP when the finger lifts. */
  if (e->type == VIBE_EV_TOUCH && e->id == VIBE_TOUCH_DOWN && !help && VIBE_TOUCH_Y(e) >= 40 && VIBE_TOUCH_Y(e) < 68)
    wave = VIBE_TOUCH_X(e) >= 240;
}

void vibe_on_audio(const float *in, float *l, float *r, int n) {
  float f = freq * vd_exp2(bend / 12.0f);
  for (int i = 0; i < n; i++) {
    amp += (target - amp) * 0.002f;                 /* smooth on/off, no clicks */
    phase += f / sr; if (phase >= 1) phase -= 1;
    l[i] = r[i] = (wave ? (phase < 0.5f ? 0.5f : -0.5f) : vd_sin(phase)) * amp;
  }
  (void)in;
}

void vibe_on_draw(uint16_t *fb) {
  char buf[16];
  (void)fb;
  vg_clear(0x0B1220);
  if (help) { vg_controls("HELLO", HELP, sizeof HELP / sizeof HELP[0] / 2); return; }   /* HOME: the standard overview */
  vg_text("HELLO VIBE", 12, 8, VG_MED, 0xFF6A1A, VG_LEFT);
  for (int i = 0; i < 2; i++) {                      /* the tab row: drawn where the touch looks for it (x 10-470, y 41-67) */
    vg_round(10 + i * 232, 41, 226, 26, 5, wave == i ? 0xFF6A1A : 0x24314A);
    vg_text(i ? "SQUARE" : "SINE", 10 + i * 232 + 113, 46, VG_MED, wave == i ? 0x000000 : 0x8A93A6, VG_CENTER);
  }
  vg_text("Keys play · Encoder 1 = pitch · HOME = controls", 240, 112, VG_SMALL, 0x8A93A6, VG_CENTER);
  vibe_led_ring(VE_1, (bend + 12.0f) / 24.0f, 1);   /* where encoder 1 stands (bipolar): a remote's knob turns to it */
  vg_text(last_key < 0 ? "–" : vg_itoa(60 + last_key, buf), 240, 150, VG_HUGE, 0xE6EAF2, VG_CENTER);
  vg_text(vg_ftoa(bend, 2, buf), 240, 230, VG_MED, 0x6EE7A8, VG_CENTER);
  for (int i = 0; i < VIBE_PADS; i++) vg_round(84 + i * 20, 270, 14, 14, 3, held[i] ? 0xFF6A1A : 0x24314A);
}

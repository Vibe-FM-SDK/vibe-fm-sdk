// SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE.
// example-seq – the SDK 9 example: a 16-step sequencer that plays OUT – MIDI to other gear or a DAW track
// (vibe_midi_send_at, sample-exact), with its own clock when no host leads (vibe_clock_out), following the DAW's
// position when one plays (vibe_transport). It also sounds a small voice of its own, so you hear it without routing.
// And the Editor (SDK 9): one card in our order, a slider, a menu, an accent colour, the pattern as a curve.
//   pads  = steps 1-16 (toggle)     keys = the root note     PLAY = run/stop (own clock)
//   encoders 1-4 = Gate, Velocity, Swing, Tempo (also the DAW's parameters)
#include "vibe.h"
#include "vibe_dsp.h"
#include "vibe_gfx.h"

enum { P_GATE, P_VEL, P_SWING, P_TEMPO, P_CHAN, P_COUNT };
static const char *const CHAN[16] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16" };
static const vibe_param PARAMS[P_COUNT] = {
  { "gate",   "Gate",     5.0f,  100.0f, 50.0f,  "%",   0,  0 },
  { "vel",    "Velocity", 1.0f,  127.0f, 100.0f, "",    0,  0 },
  { "swing",  "Swing",    0.0f,  75.0f,  0.0f,   "%",   0,  0 },
  { "tempo",  "Tempo",    40.0f, 240.0f, 120.0f, "BPM", 0,  0 },
  { "chan",   "Channel",  1.0f,  16.0f,  1.0f,   "",    16, CHAN },
};
static const char *const GROUPS[P_COUNT] = { "SEQUENCER", "SEQUENCER", "SEQUENCER", "SEQUENCER", "MIDI OUT" };
static const vibe_card CARDS[] = { { "SEQUENCER", 8, "16 steps · pads toggle them · the curve is the pattern" }, { "MIDI OUT", 4, "to your DAW track or other gear" } };

static float SR;
static uint8_t on[16] = { 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 0 };
static int root = 48, running, step = -1, host_led;
static double pos;                                 // position in 16ths (own clock)
static int note_out = -1, gate_left;               // the note sounding (and sent), frames until its off
static float phase, env;                           // the little voice
static int pattern_curve(const char *tab, const char *card, float *pts, int max)   // the Editor's graph: the 16 steps
{
  (void)tab; (void)card;
  for (int i = 0; i < 16 && i < max; i++) pts[i] = on[i] ? 1.0f : 0.05f;
  return 16;
}

void vibe_on_init(float sample_rate)
{
  SR = sample_rate;
  vibe_notes(VIBE_NOTES);                          // keys from any source: the root note
  vibe_params(PARAMS, P_COUNT);
  vibe_param_groups(GROUPS);
  vibe_editor_cards(CARDS, 2);                     // SDK 9: the Editor as we want it
  vibe_editor_control(P_GATE, VIBE_CTL_SLIDER);
  vibe_editor_control(P_CHAN, VIBE_CTL_MENU);
  vibe_editor_accent(0x40E0D0);
  vibe_editor_curve("SEQUENCER", pattern_curve);
  vibe_param_help(P_SWING, "Delays every second step by this much of a 16th");
  vibe_tempo_set(120.0f);
  vibe_clock_out(1);                               // we are the clock – unless a host leads
  for (int i = 0; i < 16; i++) vibe_led_pad(i, 0x40E0D0, on[i] ? 0.6f : 0.0f);
  vibe_label(VB_PLAY, "RUN");
}

static void note_off_now(int frame)
{
  if (note_out < 0) return;
  vibe_midi_send_at((uint8_t)(0x80 | ((int)vibe_param_get(P_CHAN) - 1)), (uint8_t)note_out, 0, frame);
  note_out = -1; env = 0.0f;
}

void vibe_on_event(const vibe_event *e)
{
  switch (e->type) {
  case VIBE_EV_PAD: if (e->down) { on[e->id] ^= 1; vibe_led_pad(e->id, 0x40E0D0, on[e->id] ? 0.6f : 0.0f); } break;
  case VIBE_EV_NOTE: if (e->down) root = e->id; break;
  case VIBE_EV_BUTTON:
    if (e->id == VB_PLAY && e->down && !host_led) { running = !running; pos = 0.0; step = -1; vibe_transport_out(running); if (!running) note_off_now(0); }
    if (e->id == VB_HOME && e->down) vibe_request(VIBE_REQ_FIRMWARE_MENU);
    break;
  case VIBE_EV_ENCODER:
    if (e->id <= VE_4) { int p = e->id; const vibe_param *d = &PARAMS[p]; vibe_param_set(p, vibe_param_get(p) + e->value * (d->max - d->min) / 100.0f); if (p == P_TEMPO) vibe_tempo_set(vibe_param_get(P_TEMPO)); }
    break;
  case VIBE_EV_PARAM: if (e->id == P_TEMPO) vibe_tempo_set(e->value); break;
  default: break;
  }
}

void vibe_on_audio(const float *in, float *l, float *r, int n)
{
  (void)in;
  const vibe_transport_info *tp = vibe_transport();
  int host = tp->has && tp->bpm > 0.0f;            // a DAW: its position, its tempo, its play
  host_led = host;
  float bpm = host ? tp->bpm : vibe_param_get(P_TEMPO);
  double per = (double)bpm / 60.0 * 4.0 / (double)SR;    // 16ths a frame
  int play = host ? tp->playing : running;
  if (host && !play && note_out >= 0) note_off_now(0);
  if (host) pos = tp->beat * 4.0;                  // (the block's first frame; loops and jumps come for free)
  double sixteenth = 1.0 / per, swing = vibe_param_get(P_SWING) / 100.0 * 0.5 * sixteenth;
  int chan = (int)vibe_param_get(P_CHAN) - 1;
  for (int f = 0; f < n; f++) {
    if (play) {
      int s = (int)pos;                            // this 16th; odd steps fire late by the swing
      double in16 = (pos - s) * sixteenth;
      int st = ((s % 16) + 16) % 16, late = (st & 1) ? (int)swing : 0;
      if (st != step && in16 >= late) {
        step = st;
        if (note_out >= 0) note_off_now(f);
        if (on[st]) {
          note_out = root; env = 1.0f; phase = 0.0f;
          vibe_midi_send_at((uint8_t)(0x90 | chan), (uint8_t)root, (uint8_t)vibe_param_get(P_VEL), f);
          gate_left = (int)(vibe_param_get(P_GATE) / 100.0f * (float)sixteenth);
        }
      }
      if (note_out >= 0 && --gate_left <= 0) note_off_now(f);
      pos += per;
    }
    float o = 0.0f;                                // the voice: a sine with a short decay
    if (env > 0.0005f) { phase += vd_mtof((float)root) / SR; if (phase >= 1.0f) phase -= 1.0f; o = vd_sin(phase) * env * 0.3f; env *= 0.9995f; }
    l[f] = r[f] = o;
  }
  if (!play) step = -1;
}

void vibe_on_draw(uint16_t *fb)
{
  (void)fb;
  char buf[16];
  vg_clear(0x101114);
  vg_text("EXAMPLE SEQ", 16, 12, VG_BIG, 0xF2F2F2, 0);
  vg_text(host_led ? "DAW leads" : running ? "own clock · MIDI clock out" : "stopped · PLAY runs it", 464, 18, VG_SMALL, host_led ? 0x40E0D0 : 0x8A93A6, VG_RIGHT);
  for (int i = 0; i < 16; i++) { int x = 16 + i * 28; vg_round(x, 70, 24, 40, 4, i == step ? 0xFFFFFF : on[i] ? 0x40E0D0 : 0x24314A); }
  vg_text(vg_itoa(root, buf), 16, 140, VG_HUGE, 0xE6EAF2, 0);
  vg_text("root note (keys / MIDI)", 16, 206, VG_SMALL, 0x8A93A6, 0);
  for (int p = 0; p < P_COUNT; p++) {
    const vibe_param *d = &PARAMS[p]; float v = vibe_param_get(p), t = (v - d->min) / (d->max - d->min); int y = 236 + p * 16;
    vg_text(d->name, 200, y, VG_SMALL, 0xB8BCC6, 0); vg_rect(290, y + 4, 160, 8, 0x2A2D35); vg_rect(290, y + 4, (int)(160.0f * t), 8, 0x40E0D0);
  }
}

// SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE.
// example-synth – the SDK 8 example: notes from every source (keys, MIDI, the DAW) and parameters the DAW automates.
// Eight voices, a saw through a resonant low-pass, an envelope. About 150 lines: everything a playable instrument needs.
// And the song position (vibe_transport): while the DAW plays, a click on every bar line, and BAR.BEAT on the screen.
#include "vibe.h"
#include "vibe_dsp.h"
#include "vibe_gfx.h"

// ---- the parameters: what the DAW automates, what encoders 1-4 + A turn ---------------------------------------
enum { P_CUTOFF, P_RESO, P_ATTACK, P_RELEASE, P_VOLUME, P_COUNT };
static const vibe_param PARAMS[P_COUNT] = {
  { "cutoff",  "Cutoff",  40.0f, 12000.0f, 1800.0f, "Hz", 0, 0 },
  { "reso",    "Reso",    0.5f,  8.0f,     1.2f,    "",   0, 0 },
  { "attack",  "Attack",  0.001f, 2.0f,    0.005f,  "s",  0, 0 },
  { "release", "Release", 0.01f, 4.0f,     0.4f,    "s",  0, 0 },
  { "volume",  "Volume",  0.0f,  1.0f,     0.7f,    "",   0, 0 },
};
// (optional) a group per parameter: the plugin's Editor view shows one card per group – "A · FILTER" would make a tab "A"
static const char *const GROUPS[P_COUNT] = { "FILTER", "FILTER", "ENVELOPE", "ENVELOPE", "OUTPUT" };

// ---- the voices ------------------------------------------------------------------------------------------------
#define VOICES 8
typedef struct { int note; float phase, dt, vel; vd_env env; vd_svf f; unsigned age; } voice;
static voice V[VOICES];
static float SR;
static unsigned age;
static vd_svf_c filt;
static int filt_dirty = 1;
static double click_bar = -1.0;                   // the bar line last clicked (a loop back, a jump: a new one)
static float click_env;

static void env_update(void)
{
  for (int i = 0; i < VOICES; i++)
    vd_env_set(&V[i].env, vibe_param_get(P_ATTACK), 0.3f, 0.7f, vibe_param_get(P_RELEASE), SR);
}

static void note_on(int note, float vel)
{
  int pick = 0;                                   // a free voice, else the oldest one
  for (int i = 0; i < VOICES; i++) {
    if (V[i].env.stage == 0) { pick = i; break; }
    if (V[i].age < V[pick].age) pick = i;
  }
  voice *v = &V[pick];
  v->note = note; v->vel = vel; v->age = ++age;
  v->dt = vd_mtof((float)note) / SR;
  vd_env_gate(&v->env, 1);
}

static void note_off(int note)
{
  for (int i = 0; i < VOICES; i++) if (V[i].note == note && V[i].env.stage && V[i].env.stage != 3) vd_env_gate(&V[i].env, 0);
}

// ---- the four functions ------------------------------------------------------------------------------------------
void vibe_on_init(float sample_rate)
{
  SR = sample_rate;
  vibe_notes(VIBE_NOTES);                         // keys, MIDI and the DAW's notes all come as VIBE_EV_NOTE
  vibe_params(PARAMS, P_COUNT);                   // the DAW sees Cutoff, Reso, Attack, Release, Volume
  vibe_param_groups(GROUPS);                      // the Editor view: cards FILTER, ENVELOPE, OUTPUT
  for (int i = 0; i < VOICES; i++) V[i].note = -1;
  env_update();
}

void vibe_on_event(const vibe_event *e)
{
  switch (e->type) {
  case VIBE_EV_NOTE:                              // from any source: e->midi[1] says which (keys, pads, MIDI, host)
    if (e->down) note_on(e->id, e->value); else note_off(e->id);
    break;
  case VIBE_EV_PARAM:                             // the DAW moved one (its value is already stored: vibe_param_get)
    if (e->id == P_CUTOFF || e->id == P_RESO) filt_dirty = 1;
    if (e->id == P_ATTACK || e->id == P_RELEASE) env_update();
    break;
  case VIBE_EV_ENCODER:                           // encoders 1-4 and A turn the five parameters; the DAW records it
    if (e->id <= VE_A) {
      int p = e->id == VE_A ? P_VOLUME : e->id;
      const vibe_param *d = &PARAMS[p];
      float step = (d->max - d->min) / 100.0f;
      vibe_param_set(p, vibe_param_get(p) + e->value * step);
      if (p == P_CUTOFF || p == P_RESO) filt_dirty = 1;
      if (p == P_ATTACK || p == P_RELEASE) env_update();
    }
    break;
  default: break;
  }
}

void vibe_on_audio(const float *in, float *out_l, float *out_r, int frames)
{
  (void)in;
  if (filt_dirty) { vd_svf_set(&filt, vibe_param_get(P_CUTOFF), vibe_param_get(P_RESO), SR); filt_dirty = 0; }
  float vol = vibe_param_get(P_VOLUME) * 0.25f;
  const vibe_transport_info *tp = vibe_transport();
  int bars = tp->has && tp->playing && tp->bpm > 0.0f;
  double per_frame = bars ? tp->bpm / 60.0 / SR : 0.0, bar_len = bars ? tp->sig_num * 4.0 / tp->sig_den : 4.0;
  for (int n = 0; n < frames; n++) {
    float mix = 0.0f;
    for (int i = 0; i < VOICES; i++) {
      voice *v = &V[i];
      if (!v->env.stage) continue;
      float s = vd_saw(v->phase, v->dt);
      v->phase += v->dt; if (v->phase >= 1.0f) v->phase -= 1.0f;
      mix += vd_svf_tick(&v->f, &filt, s).lp * vd_env_next(&v->env) * v->vel;
    }
    if (bars) {                                   // this frame's position: on a bar line? (a few frames' slack)
      double pos = tp->beat + n * per_frame, line = tp->bar_start;
      while (pos >= line + bar_len) line += bar_len;
      if (line != click_bar && pos - line < 4.0 * per_frame) { click_bar = line; click_env = 1.0f; }
    }
    if (click_env > 0.0001f) { mix += click_env * ((n & 8) ? 0.5f : -0.5f); click_env *= 0.995f; }
    float o = vd_tanh(mix * vol);
    out_l[n] = o; out_r[n] = o;
  }
}

void vibe_on_draw(uint16_t *fb)
{
  (void)fb;
  vg_clear(0x101114);
  vg_text("EXAMPLE SYNTH", 16, 12, VG_BIG, 0xF2F2F2, 0);
  const vibe_transport_info *tp = vibe_transport();
  if (tp->has) {                                  // the song position: bar.beat (1.1 = the song's start)
    double bar_len = tp->sig_num * 4.0 / tp->sig_den;
    int bar = (int)(tp->bar_start / bar_len + 0.5) + 1, beat = (int)((tp->beat - tp->bar_start) * tp->sig_den / 4.0) + 1;
    char s[24], *q = s;
    for (int v = bar, d = 1000; d; d /= 10) if (v >= d || d == 1 || q != s) *q++ = (char)('0' + v / d % 10);
    *q++ = '.'; *q++ = (char)('0' + beat % 10); *q++ = ' '; *q++ = tp->playing ? '>' : '|'; *q = 0;
    vg_text(s, 360, 12, VG_BIG, tp->playing ? 0xFF60BE : 0x6A6E78, 0);
  }
  for (int p = 0; p < P_COUNT; p++) {             // each parameter: its name, a bar, its value
    const vibe_param *d = &PARAMS[p];
    float v = vibe_param_get(p), t = (v - d->min) / (d->max - d->min);
    int y = 70 + p * 46;
    vg_text(d->name, 16, y, VG_MED, 0xB8BCC6, 0);
    vg_rect(140, y + 4, 300, 14, 0x2A2D35);
    vg_rect(140, y + 4, (int)(300.0f * t), 14, 0xFF60BE);
  }
}

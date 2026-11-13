/* SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE. */
/*
 * GRID – a pad grid of any shape (SDK 20). Ask the device once, then play on whatever grid it has:
 *   the VIBE's 4 x 4, a Launchpad as 8 x 8 (Web Player: Controllers ▾ → Launchpad as 8 × 8 pads), a 20 x 4 matrix.
 *   pads -> a note each, a fourth per row upwards (as an isomorphic keyboard), lit while held, dimly in their column's colour
 *   keys -> the same sound · HOME -> what every control does (HOME 1 s: the library, the host does it)
 */
#include "vibe.h"
#include "vibe_gfx.h"
#include "vibe_dsp.h"

/* Two flows (a must): vibe_on_audio runs beside vibe_on_event / vibe_on_draw. The voices belong to the audio alone;
   a pad or key only queues a note (a ring with one writer, the events, and one reader, the audio – README "Two flows") */
VIBE_PARALLEL_AUDIO

static float sr;
static vibe_device_info dev;
static int cols = 4, rows = 4, pads = 16;
static uint8_t held[VIBE_PADS_MAX];
#define VOICES 8
static struct { float f, ph, env; } v[VOICES];   /* the audio's alone */
static int next, help;                          /* next: the audio's; help: the control flow's */
#define QN 32                                   /* the note queue: events write q[] then w, the audio reads up to w then r */
static struct { float f, env; } q[QN];
static volatile uint32_t qw, qr;
static const char *const HELP[] = { "PADS", "a note each, a fourth per row up", "KEYS", "the same sound", "HOME", "this overview" };
static const uint32_t COLOUR[8] = { 0xFF6A1A, 0xFFC21A, 0x6EE7A8, 0x1AD6FF, 0x5B7CFF, 0xB65BFF, 0xFF5BC8, 0xE6EAF2 };

static int pad_note(int p) { int r = p / cols, c = p % cols; return 36 + c + (rows - 1 - r) * 5; }
static void pad_light(int p) { vibe_led_pad(p, held[p] ? 0xFFFFFF : COLOUR[(p % cols) & 7], held[p] ? 1.0f : 0.25f); }
static void play(int note, float vel)            /* (control flow) a note into the queue – full: dropped */
{
  uint32_t w = qw;
  if (w - qr >= QN) return;
  q[w % QN].f = vd_mtof((float)note); q[w % QN].env = 0.35f * vel;
  VIBE_RELEASE();                               /* the note's data before the counter that hands it over */
  qw = w + 1;
}

void vibe_on_init(float sample_rate)
{
  sr = sample_rate;
  vibe_device(&dev);                      /* 1 = the host described it; else the base (4 x 4) – both fill dev */
  cols = dev.pad_cols; rows = dev.pad_rows; pads = dev.pads;
  if (cols < 1 || rows < 1 || cols * rows != pads) { cols = 4; rows = (pads + 3) / 4; }
  for (int p = 0; p < pads; p++) pad_light(p);
}

void vibe_on_event(const vibe_event *e)
{
  if (e->type == VIBE_EV_PAD && e->id < pads) {
    held[e->id] = e->down; pad_light(e->id);
    if (e->down) play(pad_note(e->id), e->value > 0 ? e->value : 0.9f);
  }
  if (e->type == VIBE_EV_KEY && e->down) play(48 + e->id, e->value);
  if (e->type == VIBE_EV_BUTTON && e->down) {   /* pressed = e->down · HOME = what every control does (Protokoll 3.2) */
    if (e->id == VB_HOME) help = !help; else if (e->id == VB_RETURN) help = 0;
  }
}

void vibe_on_audio(const float *in, float *l, float *r, int n)
{
  uint32_t w = qw;                               /* (audio flow) the queued notes: the counter first … */
  VIBE_ACQUIRE();                                 /* … then their data */
  for (uint32_t k = qr; k != w; k++) { v[next].f = q[k % QN].f; v[next].ph = 0; v[next].env = q[k % QN].env; next = (next + 1) % VOICES; }
  VIBE_RELEASE(); qr = w;                         /* (read before the slots are given back) */
  float dec = vd_exp2(-1.0f / (0.35f * sr));   /* a pluck: half its level in 0.35 s */
  for (int i = 0; i < n; i++) {
    float s = 0;
    for (int k = 0; k < VOICES; k++) {
      if (v[k].env < 1e-5f) continue;
      v[k].ph += v[k].f / sr; if (v[k].ph >= 1) v[k].ph -= 1;
      s += vd_sin(v[k].ph) * v[k].env; v[k].env *= dec;
    }
    l[i] = r[i] = vd_clamp(s, -1, 1);
  }
  (void)in;
}

void vibe_on_draw(uint16_t *fb)
{
  char a[8], b[8], c[8];
  (void)fb;
  if (help) { vg_controls("GRID", HELP, sizeof HELP / sizeof HELP[0] / 2); return; }
  vg_clear(0x0B1220);
  vg_text("GRID", 16, 10, VG_BIG, 0xFF6A1A, VG_LEFT);
  vg_text(dev.name[0] ? dev.name : "VIBE FM", 464, 14, VG_SMALL, 0x8A93A6, VG_RIGHT);
  vg_itoa(pads, a); vg_itoa(cols, b); vg_itoa(rows, c);
  int x = vg_text(a, 16, 44, VG_MED, 0xE6EAF2, VG_LEFT);
  x += vg_text(" pads · ", 16 + x, 44, VG_MED, 0x8A93A6, VG_LEFT);
  x += vg_text(b, 16 + x, 44, VG_MED, 0xE6EAF2, VG_LEFT);
  x += vg_text(" × ", 16 + x, 44, VG_MED, 0x8A93A6, VG_LEFT);
  vg_text(c, 16 + x, 44, VG_MED, 0xE6EAF2, VG_LEFT);
  int cell = 220 / (rows > cols ? rows : cols); if (cell > 48) cell = 48; if (cols * cell > 440) cell = 440 / cols;
  int gx = 240 - cols * cell / 2, gy = 84;
  for (int p = 0; p < pads; p++) {
    int px = gx + (p % cols) * cell, py = gy + (p / cols) * cell;
    vg_round(px + 1, py + 1, cell - 2, cell - 2, 3, held[p] ? 0xFFFFFF : COLOUR[(p % cols) & 7] & 0x5F5F5F);
  }
}

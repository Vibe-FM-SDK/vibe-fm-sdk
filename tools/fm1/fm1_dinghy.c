/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * tools/fm1 – any FM-1 firmware of the Dinghy family (FM-106, FRODHES, SHINE ON, OberVave 2 …) on the VIBE, unchanged
 * (made by tools/fm1-convert.sh). GPL-3.0 because the fake FM-1 (dinghy/host.c) follows the firmwares' own tests/host.c.
 *
 * A Dinghy firmware is one C unit (FM1_MAIN, e.g. "fm106.c") that includes platform.c, which with DINGHY_HOST includes
 * the fake FM-1 (tools/fm1/dinghy/host.c, put next to the copy of its src/ as tests/host.c). The firmware supplies
 * app_audio (the audio ISR: 44.1 kHz, Q15) and app_main (never returns; calls platform_poll every few ms): app_main runs
 * as a coroutine (Asyncify), parked in platform_poll, one pass per simulated millisecond, driven by the audio.
 * The VIBE feeds the fake FM-1:
 *   keys, pads     = MIDI notes into the firmware's USB-MIDI in, with velocity (keys from C3, pads = the FM-1's white keys
 *                    F3 …; OCT −/+ move both) – as a DAW plays it, so the VIBE and the plugin play it the same
 *   MIDI in        = the firmware's USB-MIDI in (the DAW's notes, CCs …)
 *   encoders       = KNOB 1-4; DATA = PRESETS (SHIFT: SELECT); A = ALGORITHM; B, ◀ ▶ = SELECT
 *   six keys       = FM1_KEYS6 (default FX SEL ENV LFO ARP EDIT); F1-F6 = FM1_SOFT (default SEQ GLO SAVE PRESET− PRESET+ HOLD)
 *   HOME/RETURN, ENTER, PLAY, STOP, REC = HOME, SEL, PLAY, PLAY (while it plays), REC; MENU = SEQ
 *   TRANSPORT      = the DAW's / MIDI clock's play and stop press PLAY when it differs (Protokoll 7)
 *   pedal          = sustain (CC 64)
 *   screen         = only the FM-1's 240 x 240, enlarged to 320 x 320 in the middle (the captions are on the keys)
 *   settings       = the FM-1's settings sector, kept on the VIBE (vibe_store "settings")
 * A firmware may bring firmware/<id>/fm1map.h (other KEYS6 / soft keys), fm1_extra.c (extras: hooks extra_init,
 * extra_event (1 = taken), extra_draw, extra_leds) and dinghy_app.c (the rest of its platform, made by the converter).
 */
#include <stdint.h>
#include <stddef.h>

#include FM1_MAIN                                 /* (the firmware; its platform.c includes the fake FM-1) */

#include "vibe.h"
#include "vibe_gfx.h"

/* ---------------------------------------------------------------- the main loop as a coroutine (Asyncify) ---- */
__attribute__((import_module("asyncify"), import_name("start_unwind"))) void asyncify_start_unwind(void *p);
__attribute__((import_module("asyncify"), import_name("stop_unwind"))) void asyncify_stop_unwind(void);
__attribute__((import_module("asyncify"), import_name("start_rewind"))) void asyncify_start_rewind(void *p);
__attribute__((import_module("asyncify"), import_name("stop_rewind"))) void asyncify_stop_rewind(void);
static struct { void *pos, *end; } aco;
static uint8_t aco_stack[32768] __attribute__((aligned(8)));
static volatile uint32_t parked, v_booted;
__attribute__((noinline)) static void platform_poll(void)
{
  if (!parked) { parked = 1; asyncify_start_unwind(&aco); }
  else { parked = 0; asyncify_stop_rewind(); }
}
static void sim_app(void) { app_main(); }
static void (*volatile sim_app_p)(void) = sim_app;   /* (through a pointer: app_main never returns) */
__attribute__((noinline)) static void sim_pass(void)
{
  if (!v_booted) { v_booted = 1; aco.pos = aco_stack; aco.end = aco_stack + sizeof aco_stack; }
  else asyncify_start_rewind(&aco);
  sim_app_p();
  asyncify_stop_unwind();
}

/* ---------------------------------------------------------------- audio: the 44.1 kHz engine -> the runtime's rate */
#define RING 8192u
static float vb_l[RING], vb_r[RING];
static uint32_t sw, blk_pos = HALF_FRAMES, ms_acc;
static int32_t blk[2u * HALF_FRAMES];
static double spos, sstep;
static float slevel;
static uint32_t btn_rel_late;                     /* buttons let go before a pass saw them pressed: after that pass */
static void sim_ms(void)
{
  fm1_ms++;
  sim_pass();
  if (btn_rel_late) { fm1_in.buttons &= ~btn_rel_late; host_released |= btn_rel_late; btn_rel_late = 0; }
}
static void engine_frames(uint32_t n)
{
  for (uint32_t i = 0; i < n; i++) {
    if (blk_pos >= HALF_FRAMES) {
      memset(blk, 0, sizeof blk);
      app_audio(blk, HALF_FRAMES);
      blk_pos = 0;
    }
    int32_t l = blk[2u * blk_pos], r = blk[2u * blk_pos + 1u];
    l = l > 32767 ? 32767 : l < -32767 ? -32767 : l; r = r > 32767 ? 32767 : r < -32767 ? -32767 : r;
    vb_l[sw % RING] = (float)l * (1.0f / 32768.0f);
    vb_r[sw % RING] = (float)r * (1.0f / 32768.0f);
    sw++; blk_pos++;
    if ((ms_acc += 1000u) >= FS) { ms_acc -= FS; sim_ms(); }
  }
}

/* ---------------------------------------------------------------- controls -------------------------------------- */
enum { VS_NONE = 100, VS_PRESET_DOWN, VS_PRESET_UP, VS_HOLD };   /* soft keys that do something other than a button */
#ifdef FM1_MAP
#include FM1_MAP                                  /* (firmware/<id>/fm1map.h: its own six keys / soft keys) */
#else
static const struct { uint32_t btn, rgb; const char *name; } KEYS6[6] = {
  { BTN_FX, 0x00A5E1, "FX" }, { BTN_SEL, 0xB38CFF, "SEL" }, { BTN_ENV, 0xF5AF14, "ENV" },
  { BTN_LFO, 0x6EE7A8, "LFO" }, { BTN_ARP, 0xEB2D1E, "ARP" }, { BTN_EDIT, 0xE6EAF2, "EDIT" },
};
static const struct { uint32_t what; const char *name; } VSOFT[6] = {
  { BTN_SEQ, "SEQ" }, { BTN_GLO, "GLO" }, { BTN_SAVE, "SAVE" }, { VS_PRESET_DOWN, "PRE -" }, { VS_PRESET_UP, "PRE +" }, { VS_HOLD, "HOLD" },
};
#endif
static const uint8_t PAD_NOTE[16] = { 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79 };   /* the FM-1's white keys F3 … */
static int16_t key_sent[VIBE_KEYS], pad_sent[VIBE_PADS];   /* the note a held key / pad sent, -1 none */
static float enc_frac[VE_COUNT];
static int v_octave, v_shift, v_hold;
static uint8_t k6_held;

static void fm1_button(uint32_t b, int down)
{
  if (b >= 32u) return;
  if (down) { host_edges |= 1u << b; fm1_in.buttons |= 1u << b; btn_rel_late &= ~(1u << b); }
  else if (host_edges >> b & 1u) btn_rel_late |= 1u << b;   /* (a tap within one millisecond still counts) */
  else { host_released |= 1u << b; fm1_in.buttons &= ~(1u << b); }
}
static void fm1_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
  if (st < 0x80u || st >= 0xF0u || mq_w - mq_r >= 64u) return;
  mq[mq_w % 64u][0] = (uint8_t)st; mq[mq_w % 64u][1] = (uint8_t)(d1 & 0x7Fu); mq[mq_w % 64u][2] = (uint8_t)(d2 & 0x7Fu);
  RING_PUBLISH(); mq_w++;
}
static void fm1_enc(uint32_t e, float det, int vibe_enc)
{
  enc_frac[vibe_enc] += det;
  int s = (int)enc_frac[vibe_enc]; enc_frac[vibe_enc] -= (float)s;
  if (s && e < 8u) host_enc[e] += s;
}
static void note(int16_t *slot, int down, int n, float vel)
{
  if (down) { n = n < 0 ? 0 : n > 127 ? 127 : n; *slot = (int16_t)n; fm1_midi(0x90, (uint32_t)n, (uint32_t)(vel * 126.0f) + 1u); }
  else if (*slot >= 0) { fm1_midi(0x80, (uint32_t)*slot, 0); *slot = -1; }
}
static int v_playing(void) { return host_led[BTN_PLAY] != 0; }   /* (the PLAY light says it) */

static void settings_keep(void);

#ifdef FM1_EXTRA
#include FM1_EXTRA                                /* (firmware/<id>/fm1_extra.c) */
#else
static void extra_init(void) {}
static int extra_event(const vibe_event *e) { (void)e; return 0; }
static void extra_draw(uint16_t *out) { (void)out; }
static void extra_leds(void) {}
#endif

/* ---------------------------------------------------------------- the settings, kept on the VIBE ---------------- */
typedef struct {
  uint32_t magic;
  uint32_t prog;
  uint8_t  edited;
  uint8_t  pad[3];
  patch_t  live;
  int16_t  draw_table[16];
  uint16_t chop_mask[4];
} vibe_live_t;

static uint32_t last_saved_prog = 0xFFFFFFFFu;
static patch_t last_saved_live;
static int16_t last_saved_draw[16];
static uint16_t last_saved_chop[4];
static uint8_t live_has_saved;

static void settings_keep(void)                   /* (draw: the control flow - never from the audio) */
{
  if (host_store_dirty) {
    host_store_dirty = 0;
    uint32_t n = 4u + (host_store[1] | (uint32_t)host_store[2] << 8);
    vibe_store_write("settings", host_store, (int)n);
  }
  if (host_psect_dirty) {
    for (uint32_t i = 0; i < NPSECT; i++) {
      if ((host_psect_dirty >> i) & 1u) {
        char name[8] = "plob0"; name[4] = (char)('0' + i);
        uint32_t n = 4u + (host_psect[i][1] | (uint32_t)host_psect[i][2] << 8);
        vibe_store_write(name, host_psect[i], (int)n);
      }
    }
    host_psect_dirty = 0;
  }
  if (host_blob_dirty) {
    for (uint32_t i = 0; i < NBLOB; i++) {
      if ((host_blob_dirty >> i) & 1u) {
        char name[8] = "blob0"; name[4] = (char)('0' + i);
        uint32_t n = 4u + (host_blob[i][1] | (uint32_t)host_blob[i][2] << 8);
        vibe_store_write(name, host_blob[i], (int)n);
      }
    }
    host_blob_dirty = 0;
  }

  /* keep live patch, program, draw table and chop mask in project storage */
  int diff = !live_has_saved ||
             (prog != last_saved_prog) ||
             memcmp(&live, &last_saved_live, sizeof(patch_t)) != 0 ||
             memcmp(pat.draw_table, last_saved_draw, sizeof(pat.draw_table)) != 0 ||
             memcmp(pat.chop_mask, last_saved_chop, sizeof(pat.chop_mask)) != 0;

  if (diff) {
    vibe_live_t ls;
    memset(&ls, 0, sizeof(ls));
    ls.magic = 0x4556494Cu; /* 'LIVE' */
    ls.prog = prog;
    ls.edited = (uint8_t)edited;
    ls.live = live;
    memcpy(ls.draw_table, pat.draw_table, sizeof(ls.draw_table));
    memcpy(ls.chop_mask, pat.chop_mask, sizeof(ls.chop_mask));
    vibe_store_write("live", &ls, (int)sizeof(ls));

    last_saved_prog = prog;
    last_saved_live = live;
    memcpy(last_saved_draw, pat.draw_table, sizeof(last_saved_draw));
    memcpy(last_saved_chop, pat.chop_mask, sizeof(last_saved_chop));
    live_has_saved = 1;

    if (set.prog != (uint8_t)prog) {
      set.prog = (uint8_t)prog;
      settings_save(&set, sizeof set);
    }
  }
}

void vibe_on_init(float rate)
{
  for (int i = 0; i < VIBE_KEYS; i++) key_sent[i] = -1;
  for (int i = 0; i < VIBE_PADS; i++) pad_sent[i] = -1;
  sstep = (double)FS / (double)rate;

  /* restore settings, programs and patterns from VIBE project storage */
  if (vibe_store_read("settings", host_store, sizeof host_store) < 4) host_store[0] = 0;
  for (uint32_t i = 0; i < NPSECT; i++) {
    char name[8] = "plob0"; name[4] = (char)('0' + i);
    if (vibe_store_read(name, host_psect[i], sizeof host_psect[i]) < 4) host_psect[i][0] = 0;
  }
  for (uint32_t i = 0; i < NBLOB; i++) {
    char name[8] = "blob0"; name[4] = (char)('0' + i);
    if (vibe_store_read(name, host_blob[i], sizeof host_blob[i]) < 4) host_blob[i][0] = 0;
  }

  extra_init();
  sim_pass();                                     /* boot, up to the first poll */
  for (uint32_t i = 0; i < FS * 2u / 64u; i++) engine_frames(64);   /* the boot ahead: 2 s of FM-1 time, not in the audio */
  spos = (double)(sw - 2u);

  /* restore live tweaked patch and custom wave tables if project had saved state */
  vibe_live_t ls;
  if (vibe_store_read("live", &ls, sizeof ls) >= (int)sizeof(patch_t) + 8 && ls.magic == 0x4556494Cu) {
    prog = ls.prog;
    set.prog = (uint8_t)prog;
    live = ls.live;
    edited = ls.edited;
    if (sizeof(ls) == sizeof(vibe_live_t)) {
      memcpy(pat.draw_table, ls.draw_table, sizeof(pat.draw_table));
      memcpy(pat.chop_mask, ls.chop_mask, sizeof(pat.chop_mask));
    } else if (prog < NFACT) {
      for (uint32_t i = 0; i < 16u; i++) pat.draw_table[i] = FACT_WAVE[prog].draw[i];
      for (uint32_t i = 0; i < 4u; i++) pat.chop_mask[i] = FACT_WAVE[prog].chop[i];
    }
    last_saved_prog = prog;
    last_saved_live = live;
    memcpy(last_saved_draw, pat.draw_table, sizeof(last_saved_draw));
    memcpy(last_saved_chop, pat.chop_mask, sizeof(last_saved_chop));
    live_has_saved = 1;
  } else if (set.prog < NFACT) {
    prog = set.prog;
    for (uint32_t i = 0; i < 16u; i++) pat.draw_table[i] = FACT_WAVE[set.prog].draw[i];
    for (uint32_t i = 0; i < 4u; i++) pat.chop_mask[i] = FACT_WAVE[set.prog].chop[i];
  }

  vibe_log(FM1_NAME " " FM1_VERSION " on VIBE (tools/fm1, Dinghy)");
}

void vibe_on_event(const vibe_event *e)
{
  if (e->type == VIBE_EV_BUTTON && e->id == VB_SHIFT) v_shift = e->down;
  if (extra_event(e)) return;
  /* a key or pad the VIBE does not have (a host's stray id): nothing – the arrays below are per key and per pad */
  if ((e->type == VIBE_EV_KEY && (unsigned)e->id >= VIBE_KEYS) || (e->type == VIBE_EV_PAD && (unsigned)e->id >= VIBE_PADS)) return;
  switch (e->type) {
    case VIBE_EV_KEY: note(&key_sent[e->id], e->down, 48 + e->id + 12 * v_octave, e->value); break;
    case VIBE_EV_PAD: note(&pad_sent[e->id], e->down, PAD_NOTE[e->id & 15] + 12 * v_octave, e->value); break;
    case VIBE_EV_BUTTON: {
      int b = e->id, d = e->down;
      if (b >= VB_K1 && b <= VB_K6) {            /* (a mouse cannot hold a key and turn: HOLD keeps one held) */
        uint32_t m = 1u << (b - VB_K1), btn = KEYS6[b - VB_K1].btn;
        if (k6_held & m) { if (d) { k6_held &= (uint8_t)~m; fm1_button(btn, 0); } break; }
        if (d && v_hold) { v_hold = 0; k6_held |= (uint8_t)m; fm1_button(btn, 1); break; }
        fm1_button(btn, d); break;
      }
      if (b >= VB_F1 && b <= VB_F6) {
        uint32_t w = VSOFT[b - VB_F1].what;
        if (w == VS_PRESET_DOWN) { if (d) fm1_enc(ENC_PRESETS, -1, VE_DATA); }
        else if (w == VS_PRESET_UP) { if (d) fm1_enc(ENC_PRESETS, 1, VE_DATA); }
        else if (w == VS_HOLD) { if (d) v_hold = !v_hold; }
        else if (w < 32u) fm1_button(w, d);
        break;
      }
      switch (b) {
        case VB_MENU: fm1_button(BTN_SEQ, d); break;
        case VB_HOME: case VB_RETURN: fm1_button(BTN_HOME, d); break;
        case VB_ENTER: case VB_DATA_PUSH: fm1_button(BTN_SEL, d); break;
        case VB_PLAY: fm1_button(BTN_PLAY, d); break;
        case VB_STOP: if (v_playing() || !d) fm1_button(BTN_PLAY, d); break;
        case VB_REC: fm1_button(BTN_REC, d); break;
        case VB_OCT_DOWN: case VB_OCT_UP:
          if (d) { v_octave += b == VB_OCT_UP ? 1 : -1; v_octave = v_octave < -3 ? -3 : v_octave > 3 ? 3 : v_octave; }
          break;
        case VB_LEFT: if (d) fm1_enc(ENC_SELECT, -1, VE_B); break;
        case VB_RIGHT: if (d) fm1_enc(ENC_SELECT, 1, VE_B); break;
      }
      break; }
    case VIBE_EV_ENCODER:
      if (e->id <= VE_4) fm1_enc(ENC_KNOB1 + (uint32_t)e->id, e->value, e->id);
      else if (e->id == VE_DATA) fm1_enc(v_shift ? ENC_SELECT : ENC_PRESETS, e->value, e->id);
      else if (e->id == VE_A) fm1_enc(ENC_ALGORITHM, e->value, e->id);
      else if (e->id == VE_B) fm1_enc(ENC_SELECT, e->value, e->id);
      break;
    case VIBE_EV_MIDI: fm1_midi(e->midi[0], e->midi[1], e->midi[2]); break;
    case VIBE_EV_PEDAL: if (e->id == VIBE_PEDAL_SWITCH) fm1_midi(0xB0, 64, e->down ? 127 : 0); break;
    case VIBE_EV_TRANSPORT:                       /* the DAW's play / stop (Protokoll 7): the PLAY key when it differs */
      if (e->id == VIBE_TRANSPORT_PLAY && (e->down != 0) != v_playing()) { fm1_button(BTN_PLAY, 1); fm1_button(BTN_PLAY, 0); }
      break;
  }
}

void vibe_on_audio(const float *in, float *l, float *r, int n)
{
  (void)in;
  float pk = 0;
  for (int i = 0; i < n; i++) {                   /* 44.1 kHz -> the runtime's rate (linear) */
    uint32_t i0 = (uint32_t)spos;
    while (i0 + 2u > sw) engine_frames(64);
    float a = (float)(spos - (double)i0);
    l[i] = vb_l[i0 % RING] + (vb_l[(i0 + 1u) % RING] - vb_l[i0 % RING]) * a;
    r[i] = vb_r[i0 % RING] + (vb_r[(i0 + 1u) % RING] - vb_r[i0 % RING]) * a;
    float m = l[i] < 0 ? -l[i] : l[i]; if (m > pk) pk = m;
    spos += sstep;
  }
  slevel = pk > slevel ? pk : slevel * 0.97f;
}

/* ---------------------------------------------------------------- display + lights ---------------------------- */
#include "fm1_screen.h"                          /* (only the FM-1's screen, 320 x 320 in the middle) */
void vibe_on_draw(uint16_t *out)
{
  settings_keep();
  fm1_screen_draw(fb, out);
  for (int i = 0; i < 6; i++) { vibe_label(VB_F1 + i, VSOFT[i].name); vibe_label(VB_K1 + i, KEYS6[i].name); }   /* captions on the keys */
  extra_draw(out);

  for (int p = 0; p < VIBE_PADS; p++) {           /* pads: the keys that sound (the FM-1's key LEDs) */
    int k = PAD_NOTE[p] - 53, lit = host_led[KEY_LED(k)], glow = host_glow[KEY_LED(k)];
    vibe_led_pad(p, lit ? 0xE6D1B9 : 0x8A6A4A, lit ? 1.0f : glow ? 0.25f : 0.05f);
  }
  for (int i = 0; i < 6; i++) vibe_led_button(VB_K1 + i, KEYS6[i].rgb, host_led[KEYS6[i].btn] || (k6_held >> i & 1u) ? 1.0f : 0.16f);
  for (int i = 0; i < 6; i++) { uint32_t w = VSOFT[i].what; vibe_led_button(VB_F1 + i, 0xE6D1B9, (w < 32u && host_led[w]) || (w == VS_HOLD && (v_hold || k6_held)) ? 1.0f : 0.0f); }
  vibe_led_button(VB_PLAY, 0x6EE7A8, host_led[BTN_PLAY] ? 1.0f : 0.0f);
  vibe_led_button(VB_REC, 0xFF5A4D, host_led[BTN_REC] ? 1.0f : 0.0f);
  vibe_led_button(VB_HOME, 0xE6D1B9, host_led[BTN_HOME] ? 1.0f : 0.0f);
  vibe_led_button(VB_MENU, 0xE6D1B9, host_led[BTN_SEQ] ? 1.0f : 0.0f);
  vibe_led_button(VB_OCT_DOWN, 0xE6D1B9, v_octave < 0 ? 1.0f : 0.0f);
  vibe_led_button(VB_OCT_UP, 0xE6D1B9, v_octave > 0 ? 1.0f : 0.0f);
  extra_leds();
}

/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * tools/fm1 – any FM-1 firmware of the Felucca family on the VIBE, unchanged (made by tools/fm1-convert.sh).
 * GPL-3.0 because the fake FM-1 (hal/) comes from zp12's / SLOOP's simulators; the firmware keeps its own licence.
 *
 * The firmware (FM1_MAIN, e.g. "felucca.c", with all it includes) runs on a fake FM-1 (hal/: 240x240 screen, keys,
 * LEDs, 1 MiB flash in RAM, clock, I2S at 44.1 kHz). Its main loop (fm1_cstart) never returns: it runs as a coroutine
 * (Asyncify), parked whenever it feeds the watchdog or waits, one pass per simulated millisecond, driven by the audio.
 * The VIBE feeds the fake FM-1:
 *   keys       = the FM-1's 27 keys by colour order (VIBE white key n = FM-1 white key n), OCT -/+ = OCT -/+
 *   pads 1-16  = the FM-1's white keys 1-16
 *   encoders   = KNOB 1-4; DATA = PRESETS (SHIFT: SELECT); ◀ ▶ = SELECT; A = ALGORITHM
 *   six keys   = FM1_KEYS6 (default FX SEL ENV LFO ARP EDIT); F1-F6 = FM1_SOFT (default SEQ GLO SAVE ALGO- ALGO+ -)
 *   HOME, PLAY, REC = the same; STOP = PLAY while it plays; MIDI in = the firmware's MIDI queue (FM1_MIDI_Q)
 *   TRANSPORT  = the DAW's play / stop presses PLAY (a firmware's fm1_extra.c may do better: SLOOP's takes the tempo too)
 *   screen     = only the FM-1's 240x240, as large as the display is high (fm1_screen.h); captions on the keys
 *   flash      = kept on the VIBE: every 4 KB sector the firmware wrote is saved (vibe_store "nor/<nn>") and loaded
 *                at the next start – what was saved on the fake FM-1 stays
 * A firmware may bring firmware/<id>/fm1map.h (other KEYS6 / soft keys) and fm1_extra.c (its own extras, e.g. SLOOP's
 * step cursor): hooks extra_init, extra_event (1 = taken), extra_draw, extra_leds; with FM1_EXTRA_AUDIO also
 * extra_audio(in, n) – the input block (IN 1) before the firmware's audio; with FM1_EXTRA_BOOTED extra_booted() after
 * the boot (vibe_on_init).
 */
#include <stdint.h>
#include <stddef.h>

#define FELUCCA_OTA 0                            /* no updates, no web editor (no USB) */
#define FELUCCA_CDC 0
#define FELUCCA_UAC 0
#define FELUCCA_UART 0
#define FELUCCA_CPU1 0                           /* one core: the same sound, bit for bit (Felucca's own claim) */

/* fm1_cstart's linker symbols: empty ranges (C data is initialised by the wasm image) */
uint32_t sim_nil[1];
#define _data_start sim_nil
#define _data_end sim_nil
#define _data_load sim_nil
#define _bss_start sim_nil
#define _bss_end sim_nil
#define _pool_start sim_nil
#define _pool_end sim_nil
#define _rt_start sim_nil
#define _rt_end sim_nil
#define _rt_load sim_nil
char sim_nilc[4];
#define _ustack_lo sim_nilc
#define _ustack_top sim_nilc
#define _sstack_lo sim_nilc
#define _sstack_top sim_nilc
#define _lr_start sim_nil
#define _lr_end sim_nil
#define _c1_start sim_nil
#define _c1_end sim_nil
#define _c1_load sim_nil

#include FM1_MAIN                                 /* (the firmware: a copy of its src/ next to the fake hal/) */
#ifndef FS
#define FS 44100u                                 /* (the FM-1's rate; a firmware may call it otherwise) */
#endif

void isr_timer5(void) { fm1_timer5_irq(); }      /* (on the device: hal/fm1_isr.S wrappers) */
void isr_alnk0(void) { fm1_alnk0_irq(); }

/* ---------------------------------------------------------------- the main loop as a coroutine (Asyncify) ---- */
static void sim_boot(void) { fm1_cstart(); }
static void (*volatile sim_boot_p)(void) = sim_boot;   /* (through a pointer: fm1_cstart never returns) */
__attribute__((import_module("asyncify"), import_name("start_unwind"))) void asyncify_start_unwind(void *p);
__attribute__((import_module("asyncify"), import_name("stop_unwind"))) void asyncify_stop_unwind(void);
__attribute__((import_module("asyncify"), import_name("start_rewind"))) void asyncify_start_rewind(void *p);
__attribute__((import_module("asyncify"), import_name("stop_rewind"))) void asyncify_stop_rewind(void);
static struct { void *pos, *end; } aco;
static uint8_t aco_stack[32768] __attribute__((aligned(8)));
static volatile uint32_t parked, booted, halted;
__attribute__((noinline)) static void sim_yield(void)
{
  if (!parked) { parked = 1; asyncify_start_unwind(&aco); }
  else { parked = 0; asyncify_stop_rewind(); }
}
__attribute__((noinline)) static void sim_pass(void)
{
  if (!booted) { booted = 1; aco.pos = aco_stack; aco.end = aco_stack + sizeof aco_stack; }
  else asyncify_start_rewind(&aco);
  sim_boot_p();
  asyncify_stop_unwind();
}
static void sim_halt(uint32_t why) { halted = why; for (;;) sim_yield(); }   /* reboot / UBOOT: the firmware stops */

/* ---------------------------------------------------------------- the panel ----------------------------------- */
/* debounced state and edges as fm1_input_tick leaves them; a key or button let go before a pass of the main loop saw
 * it is let go after that pass (a tap within one audio quantum still counts) */
static uint32_t key_new, key_rel_late, btn_rel_late;
static void sim_key(uint32_t k, uint32_t down)   /* the 27 keys: 0 = F3 .. 26 = G5 */
{
  uint32_t bit = 1u << k;
  if (k >= 27u) return;
  if (down) { fm1_in.notes |= bit; fm1_in.notes_pressed |= bit; key_new |= bit; key_rel_late &= ~bit; }
  else if (key_new & bit) key_rel_late |= bit;
  else fm1_in.notes &= ~bit;
}
static void sim_button(uint32_t b, uint32_t down)   /* matrix ids: 0 OCT- 1 OCT+ 2 FX 3 SEL 4 ENV 5 LFO 6 EDIT
                                                       7 GLO 8 HOME 9 SAVE 10 ARP 11 SEQ 12 PLAY 13 REC */
{
  uint32_t bit = 1u << b;
  if (b >= 14u) return;
  if (down) { fm1_in.buttons |= bit; fm1_in.pressed |= bit; btn_rel_late &= ~bit; }
  else if (fm1_in.pressed & bit) btn_rel_late |= bit;
  else if (fm1_in.buttons & bit) { fm1_in.buttons &= ~bit; fm1_in.released |= bit; }
}
static void sim_enc(uint32_t e, int32_t steps)   /* 0 SELECT 1 ALGORITHM 2-5 KNOB 1-4 6 PRESETS; + = clockwise */
{
  if (e < FM1_NENC) fm1_in.enc_steps[e] = (int16_t)(fm1_in.enc_steps[e] + steps);
}
#ifdef FM1_MIDI_Q
#define FM1_HAS_MIDI_Q 1
#else
#define FM1_HAS_MIDI_Q 0
#endif
#define M_PLAY_ID 12u                             /* (the PLAY key's matrix id; M_PLAY below) */
static void sim_midi(uint32_t st, uint32_t d1, uint32_t d2)   /* into the firmware's MIDI in queue, as its USB puts it */
{
#ifdef FM1_MIDI_Q
  if (st < 0x80u || (st >= 0xF0u && (st < 0xF8u || st > 0xFCu)) || mi_w - mi_r >= MQ) return;   /* (channel messages; clock, start, continue, stop) */
  midi_in_q[mi_w % MQ] = (st >> 4) | st << 8 | (d1 & 0x7Fu) << 16 | (d2 & 0x7Fu) << 24;
  mi_w++;
#else
  (void)st; (void)d1; (void)d2;
#endif
}

/* ---------------------------------------------------------------- the clock and the audio --------------------- */
/* the DAW's transport, as an FM-1 hears a sequencer's: MIDI START / STOP and 24 clocks a beat at the DAW's tempo; if the
 * firmware did not follow (no MIDI sync) after 150 ms, its PLAY key, held 40 ms. Whether it plays: its PLAY light, lit
 * within the last 600 ms (many blink it with the beat). */
static int tr_on, tr_check_ms, tr_rel_ms, tr_key;   /* tr_key: the firmware needed its PLAY key (no MIDI sync): pressed at once */
static uint32_t play_dark_ms = 1u << 20, clk_us;
static float tr_bpm = 120.0f;
static uint32_t led_bits(const uint8_t *cols, uint32_t keys);
static void sim_ms_transport(void)
{
  if (tr_on && FM1_HAS_MIDI_Q) {
    clk_us += 1000u;
    uint32_t per = (uint32_t)(60000000.0f / (tr_bpm * 24.0f));
    while (clk_us >= per) { clk_us -= per; sim_midi(0xF8u, 0, 0); }
  }
  if (tr_rel_ms && !--tr_rel_ms) sim_button(M_PLAY_ID, 0);
  if (tr_check_ms && !--tr_check_ms && tr_on != (play_dark_ms < 600u)) { sim_button(M_PLAY_ID, 1); tr_rel_ms = 40; tr_key = 1; }
}
static void v_touch_tick(void);
static void sim_ms(void)                          /* a millisecond: TIMER4 + 24000, the TIMER5 ISR, a pass */
{
  sim_ticks += 1000u * FM1_TICKS_PER_US;
  sim_ms_transport();
  v_touch_tick();                                 /* (fm1-base's key sensing: the scan's readings) */
  if (sim_timer_isr) sim_timer_isr();
  if (halted) return;
  key_new = 0;
  sim_pass();
  if (key_rel_late) { fm1_in.notes &= ~key_rel_late; key_rel_late = 0; }
  if (btn_rel_late) { fm1_in.buttons &= ~btn_rel_late; fm1_in.released |= btn_rel_late; btn_rel_late = 0; }
  /* playing: PLAY's light, or its second, green one (column 8, row bit 1: no key there – JIANT lights only that one) */
  if ((led_bits(fm1_led, 0) >> M_PLAY_ID & 1u) || (fm1_led[8] >> 1 & 1u)) play_dark_ms = 0; else if (play_dark_ms < (1u << 20)) play_dark_ms++;
}
#define RING 8192u
static float vb_l[RING], vb_r[RING];
static uint32_t sw, half_pos = 1u << 30, ms_acc;
static double spos, sstep;
static float slevel;
static void engine_frames(uint32_t n)             /* n frames at 44.1 kHz into the ring */
{
  for (uint32_t i = 0; i < n; i++) {
    int32_t l = 0, r = 0;
    if (sim_aon && sim_abuf) {
      if (half_pos >= sim_ahalf_words / 2u) { sim_ahalf ^= 1u; sim_audio_isr(); half_pos = 0; }   /* the next half */
      l = sim_abuf[sim_ahalf * sim_ahalf_words + 2u * half_pos] >> 8;
      r = sim_abuf[sim_ahalf * sim_ahalf_words + 2u * half_pos + 1u] >> 8;
      half_pos++;
    }
    vb_l[sw % RING] = (float)l * (1.0f / 32768.0f);
    vb_r[sw % RING] = (float)r * (1.0f / 32768.0f);
    sw++;
    if ((ms_acc += 1000u) >= FS) { ms_acc -= FS; sim_ms(); }
  }
}

/* ================================================================ VIBE ======================================= */
#include "vibe.h"
#include "vibe_gfx.h"

/* fm1-base's key sensing (APP_TOUCH: FRASIL, CTL-1 …): its velocity and pressure come from the keys' analog readings,
 * which the VIBE has not – fm1-base's own simulator of them (lib/touch.c's host part, built in: VIBE_TOUCH_HOST) makes
 * the readings a key landing at the VIBE key's velocity gives */
#if defined(APP_TOUCH) && defined(VIBE_TOUCH_HOST)
static void v_touch(int k, float vel) { app_host_key((uint32_t)k, vel > 0.0f ? 1u + (uint32_t)(vel * 126.0f + 0.5f) : 0u, 0); }
static void v_touch_tick(void) { if (fm1_an.con) app_host_tick(); }   /* (while the app senses: plat_sense) */
#else
static void v_touch(int k, float vel) { (void)k; (void)vel; }
static void v_touch_tick(void) {}
#endif

enum { M_OCTD, M_OCTU, M_FX, M_SEL, M_ENV, M_LFO, M_EDIT, M_GLO, M_HOME, M_SAVE, M_ARP, M_SEQ, M_PLAY, M_REC };
enum { V_S_NONE = 100, V_S_ALGO_DOWN, V_S_ALGO_UP };   /* soft keys that turn ALGORITHM instead of pressing a button */

#ifdef FM1_MAP
#include FM1_MAP                                  /* (firmware/<id>/fm1map.h: its own six keys / soft keys) */
#endif
#ifndef FM1_KEYS6
#define FM1_KEYS6 { { M_FX, 0x3FA9F5, "FX" }, { M_SEL, 0xF5C93F, "SEL" }, { M_ENV, 0x6EE7A8, "ENV" }, \
                    { M_LFO, 0x58D3F7, "LFO" }, { M_ARP, 0xFF6A1A, "ARP" }, { M_EDIT, 0xB38CFF, "EDIT" } }
#endif
#ifndef FM1_SOFT
#define FM1_SOFT { { M_SEQ, "SEQ" }, { M_GLO, "GLO" }, { M_SAVE, "SAVE" }, { V_S_ALGO_DOWN, "ALGO -" }, { V_S_ALGO_UP, "ALGO +" }, { V_S_NONE, "" } }
#endif
static const struct { uint32_t btn; uint32_t rgb; const char *name; } KEYS6[6] = FM1_KEYS6;
static const struct { uint32_t what; const char *name; } VSOFT[6] = FM1_SOFT;

static int8_t white[16], black[11]; static int nwhite, nblack;
static int is_black(int note) { int n = note % 12; return n == 1 || n == 3 || n == 6 || n == 8 || n == 10; }
static int vibe_key_to_fm1(int id)                /* VIBE key (0 = C) -> FM-1 key by colour order */
{
  int w = 0, b = 0;
  for (int i = 0; i < id; i++) { if (is_black(48 + i)) b++; else w++; }
  if (is_black(48 + id)) return b < nblack ? black[b] : -1;
  return w < nwhite ? white[w] : -1;
}
static int16_t key_sent[VIBE_KEYS], pad_sent[VIBE_PADS];
static float enc_frac[VE_COUNT];
static int v_shift;
static void vibe_enc(uint32_t e, float det, int ve)
{
  enc_frac[ve] += det; int s = (int)enc_frac[ve]; enc_frac[ve] -= (float)s;
  if (s) sim_enc(e, s);
}
static uint32_t led_bits(const uint8_t *cols, uint32_t keys)   /* the LED columns back to ids (FM1_KEYMAP) */
{
  uint32_t v = 0;
  for (uint32_t c = 0; c < FM1_NCOL; c++)
    for (uint32_t r = 1; r < 5u; r++) {
      int32_t id = FM1_KEYMAP[r][c];
      if (id < 0 || !((cols[c] >> r) & 1u)) continue;
      if (keys && id >= 14) v |= 1u << (id - 14);
      if (!keys && id < 14) v |= 1u << id;
    }
  return v;
}
static int v_playing(void) { return play_dark_ms < 600u; }   /* (the PLAY light says it: lit within 600 ms – it may blink) */

/* ---------------------------------------------------------------- the flash, kept on the VIBE ------------------ */
static uint32_t vnor_t;                            /* draws since the last write (saved ~1.5 s after it) */
static uint32_t vnor_seen;
static void vnor_name(char *n, uint32_t s) { static const char H[] = "0123456789abcdef"; n[0] = 'n'; n[1] = 'o'; n[2] = 'r'; n[3] = '/'; n[4] = H[s >> 4 & 15]; n[5] = H[s & 15]; n[6] = 0; }
static void vnor_load(void)                        /* (init: the sectors saved before) */
{
  char n[8]; uint32_t got = 0;
  for (uint32_t s = 0; s < 256u; s++) { vnor_name(n, s); if (vibe_store_read(n, sim_flash + s * 0x1000u, 0x1000) == 0x1000) got++; }
  if (got) { char m[48] = "flash: "; vg_itoa((int)got, m + 7); vibe_log(m); }
}
static void vnor_save(void)                        /* (draw: the control flow – never from the audio) */
{
  char n[8];
  for (uint32_t s = 0; s < 256u; s++) {
    if (!(sim_dirty[s >> 5] >> (s & 31u) & 1u)) continue;
    sim_dirty[s >> 5] &= ~(1u << (s & 31u));
    const uint8_t *p = sim_flash + s * 0x1000u; uint32_t i = 0;
    while (i < 0x1000u && p[i] == 0xFF) i++;
    vnor_name(n, s);
    if (i == 0x1000u) vibe_store_delete(n); else vibe_store_write(n, p, 0x1000);   /* (erased: no file) */
  }
}

#include "fm1_screen.h"                          /* (only the FM-1's screen, 320 x 320 in the middle; FS_DX for extras) */
#ifdef FM1_EXTRA
#include FM1_EXTRA                                /* (firmware/<id>/fm1_extra.c) */
#else
static void extra_init(void) {}
static int extra_event(const vibe_event *e) { (void)e; return 0; }
static void extra_draw(uint16_t *out) { (void)out; }
static void extra_leds(void) {}
#endif

void vibe_on_init(float rate)
{
  for (int k = 0; k < 27; k++) { if (is_black(53 + k)) black[nblack++] = (int8_t)k; else white[nwhite++] = (int8_t)k; }
  for (int i = 0; i < VIBE_KEYS; i++) key_sent[i] = -1;
  for (int i = 0; i < VIBE_PADS; i++) pad_sent[i] = -1;
  sstep = (double)FS / (double)rate;
  for (uint32_t i = 0; i < sizeof sim_flash; i++) sim_flash[i] = 0xFF;   /* the flash: erased … */
  vnor_load();                                     /* … but for what was saved on it before */
  extra_init();
  sim_pass();                                     /* boot, up to the first wait */
  /* the boot ahead, here and not in the audio: 2 s of the FM-1's time (its tables, kit, flash, splash) – one block of it
     took 17 times its time in the audio (zp12), a crackle from the sound card at every load; the sound starts after it */
  for (uint32_t i = 0; i < FS * 2u / 64u; i++) engine_frames(64);
  spos = (double)(sw - 2u);
#ifdef FM1_EXTRA_BOOTED
  extra_booted();                                 /* (fm1_extra.c: the boot is over, e.g. a key held through it let go) */
#endif
  for (uint32_t i = 0; i < sizeof sim_dirty / sizeof sim_dirty[0]; i++) sim_dirty[i] = 0;   /* (the boot's own writes: kept) */
  vnor_seen = sim_flash_writes;
  vibe_log(FM1_NAME " " FM1_VERSION " on VIBE (tools/fm1)");
}

void vibe_on_event(const vibe_event *e)
{
  if (e->type == VIBE_EV_BUTTON && e->id == VB_SHIFT) v_shift = e->down;
  if (extra_event(e)) return;
  /* a key or pad the VIBE does not have (a host's stray id): nothing – the arrays below are per key and per pad */
  if ((e->type == VIBE_EV_KEY && (unsigned)e->id >= VIBE_KEYS) || (e->type == VIBE_EV_PAD && (unsigned)e->id >= VIBE_PADS)) return;
  switch (e->type) {
    case VIBE_EV_KEY:
      if (e->down) { int k = vibe_key_to_fm1(e->id); key_sent[e->id] = (int16_t)k; if (k >= 0) { v_touch(k, e->value); sim_key((uint32_t)k, 1); } }
      else { if (key_sent[e->id] >= 0) { v_touch(key_sent[e->id], 0); sim_key((uint32_t)key_sent[e->id], 0); } key_sent[e->id] = -1; }
      break;
    case VIBE_EV_PAD:                             /* pad n = FM-1 white key n */
      if (e->down) { int k = e->id < nwhite ? white[e->id] : -1; pad_sent[e->id] = (int16_t)k; if (k >= 0) { v_touch(k, e->value); sim_key((uint32_t)k, 1); } }
      else { if (pad_sent[e->id] >= 0) { v_touch(pad_sent[e->id], 0); sim_key((uint32_t)pad_sent[e->id], 0); } pad_sent[e->id] = -1; }
      break;
    case VIBE_EV_BUTTON: {
      int b = e->id, d = e->down;
      if (b >= VB_K1 && b <= VB_K6) { sim_button(KEYS6[b - VB_K1].btn, (uint32_t)d); break; }
      if (b >= VB_F1 && b <= VB_F6) {
        uint32_t w = VSOFT[b - VB_F1].what;
        if (w == V_S_ALGO_DOWN) { if (d) sim_enc(1, -1); } else if (w == V_S_ALGO_UP) { if (d) sim_enc(1, 1); }
        else if (w < 14u) sim_button(w, (uint32_t)d);
        break;
      }
      switch (b) {
        case VB_MENU: sim_button(M_SEQ, (uint32_t)d); break;
        case VB_HOME: case VB_RETURN: sim_button(M_HOME, (uint32_t)d); break;
        case VB_PLAY: sim_button(M_PLAY, (uint32_t)d); break;
        case VB_STOP: if (v_playing()) sim_button(M_PLAY, (uint32_t)d); else if (!d) sim_button(M_PLAY, 0); break;
        case VB_REC: sim_button(M_REC, (uint32_t)d); break;
        case VB_OCT_DOWN: sim_button(M_OCTD, (uint32_t)d); break;
        case VB_OCT_UP: sim_button(M_OCTU, (uint32_t)d); break;
        case VB_ENTER: case VB_DATA_PUSH: sim_button(M_SEL, (uint32_t)d); break;
        case VB_LEFT: if (d) sim_enc(0, -1); break;   /* SELECT */
        case VB_RIGHT: if (d) sim_enc(0, 1); break;
      }
      break; }
    case VIBE_EV_ENCODER:
      if (e->id <= VE_4) vibe_enc(2u + e->id, e->value, e->id);
      else if (e->id == VE_DATA) vibe_enc(v_shift ? 0u : 6u, e->value, e->id);   /* PRESETS; SHIFT: SELECT */
      else if (e->id == VE_A) vibe_enc(1u, e->value, e->id);                    /* ALGORITHM */
      else if (e->id == VE_B) vibe_enc(0u, e->value, e->id);                    /* SELECT */
      break;
    case VIBE_EV_MIDI: sim_midi(e->midi[0], e->midi[1], e->midi[2]); break;
    case VIBE_EV_TRANSPORT:                       /* the DAW's play / stop (Protokoll 7): the PLAY key when it differs */
      if (e->id == VIBE_TRANSPORT_PLAY) {
        tr_on = e->down != 0; clk_us = 0;
        if (FM1_HAS_MIDI_Q) sim_midi(tr_on ? 0xFAu : 0xFCu, 0, 0);
        /* (a firmware that needed the key before – JIANT with CLOCK INT – gets it at once, in time with the DAW; the
         *  check after 150 ms still puts it right if it followed the MIDI START meanwhile, e.g. CLOCK set to USB) */
        if (tr_key && tr_on != v_playing()) { sim_button(M_PLAY_ID, 1); tr_rel_ms = 40; }
        /* (the check: 150 ms after a start; after a stop 700 ms – the light may blink, "playing" is lit within 600 ms –
         *  without MIDI in at all, the key at once) */
        tr_check_ms = !FM1_HAS_MIDI_Q ? 1 : tr_on ? 150 : 700;
      } else if (e->id == VIBE_TRANSPORT_TEMPO) tr_bpm = e->value >= 20.0f && e->value <= 400.0f ? e->value : 120.0f;
      break;
  }
}

void vibe_on_audio(const float *in, float *l, float *r, int n)
{
  (void)in;
#ifdef FM1_EXTRA_AUDIO
  extra_audio(in, n);                             /* (fm1_extra.c: the input, e.g. zp12's sampler) */
#endif
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
void vibe_on_draw(uint16_t *out)
{
  if (sim_flash_writes != vnor_seen) { vnor_seen = sim_flash_writes; vnor_t = 0; }   /* written: wait for quiet */
  else if (++vnor_t == 45u) vnor_save();            /* ~1.5 s after the last write: onto the card */
  fm1_screen_draw(sim_fb, out);
  uint32_t bl = led_bits(fm1_led, 0), kl = led_bits(fm1_led, 1), kg = led_bits(FM1_LED_DIM_ROW, 1);
  for (int i = 0; i < 6; i++) { vibe_label(VB_F1 + i, VSOFT[i].name); vibe_label(VB_K1 + i, KEYS6[i].name); }   /* captions on the keys */
  extra_draw(out);

  for (int p = 0; p < VIBE_PADS; p++) {
    int k = p < nwhite ? white[p] : -1, lit = k >= 0 && (kl >> k & 1u), glow = k >= 0 && (kg >> k & 1u);
    vibe_led_pad(p, lit ? 0xE6D1B9 : 0x8A6A4A, lit ? 1.0f : glow ? 0.25f : 0.05f);
  }
  for (int i = 0; i < 6; i++) vibe_led_button(VB_K1 + i, KEYS6[i].rgb, (bl >> KEYS6[i].btn & 1u) ? 1.0f : 0.16f);
  for (int i = 0; i < 6; i++) vibe_led_button(VB_F1 + i, 0xE6D1B9, VSOFT[i].what < 14u && (bl >> VSOFT[i].what & 1u) ? 1.0f : 0.0f);
  vibe_led_button(VB_PLAY, 0x6EE7A8, (bl >> M_PLAY & 1u) ? 1.0f : 0.0f);
  vibe_led_button(VB_REC, 0xFF5A4D, (bl >> M_REC & 1u) ? 1.0f : 0.0f);
  vibe_led_button(VB_HOME, 0xE6D1B9, (bl >> M_HOME & 1u) ? 1.0f : 0.0f);
  vibe_led_button(VB_MENU, 0xE6D1B9, (bl >> M_SEQ & 1u) ? 1.0f : 0.0f);
  vibe_led_button(VB_OCT_DOWN, 0xE6D1B9, (bl >> M_OCTD & 1u) ? 1.0f : 0.0f);
  vibe_led_button(VB_OCT_UP, 0xE6D1B9, (bl >> M_OCTU & 1u) ? 1.0f : 0.0f);
  extra_leds();
}

/* OberVave 3 – OberVave as a native VIBE firmware (docs/obervave-3-nativ.md). GPL-3.0 (see LICENSE): the sound
 * engine, effects, sounds and sequencer come from OberVave; controls, display and timing are VIBE's own.
 *
 *   obv_*.h       OberVave's core (engine, effects, parameters, sounds, sequencer), at the device's sample rate
 *   o3_core.h     settings, programs, the tracks' sounds, mixer, master, value texts
 *   o3_draw.h     drawing helpers (dials, header, keys, soft keys)
 *   o3_store.h    saving: OberVave 2.0's files
 *   o3_splash.h   the start: 2.0's logo strike, twice the size
 *   o3_pages.h    the sound pages, their knobs, SAVE, the dice
 *   o3_mixer.h    MIXER (7 rows, meters, mute / solo), RESET ALL
 *   o3_seq.h      the sequencer: recording, steps, tracks on the bar, patterns
 *   o3_seqview.h  its views: TRACKS, STEPS, TRACK, PATTERNS
 *   o3_menu.h     MENU: what every control does now
 *   main.c        the SDK's four functions: audio, controls (2.0's), MIDI, drawing
 *
 * The controls are 2.0's (OberVave 2.0's, git tag vor-aufraeumen-2026-10-22): F1 MIXER, F2 TRACKS, F3/F4 PRG -/+, F5 SAVE (SHIFT: HOLD), F6 GLOBAL
 * (SHIFT: UNDO); the colour keys SOUND ENV LFO ARP FX SEQ (SOUND in STEPS: clear the step); A track (STEPS: the step), B sound (STEPS: the note, TRACK: transpose), DATA
 * sound (STEPS: a bar; MIXER and the sequencer: the pattern; SHIFT: tempo); HOME PERFORM; MENU the overview; OCT -/+
 * the keys' octave; REC (held / SHIFT: clear the track), STOP, PLAY; pads: row 1 start/stop, row 2 the track, rows
 * 3-4 notes (MIXER: rows 2-3 mute, SHIFT solo, row 4 the track; STEPS: the 16 steps).
 */
#include "vibe.h"
#include "vibe_gfx.h"
#include "vibe_dsp.h"

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
static uint32_t obv_fs = 44100u, fx_fs = 44100u;   /* the device's rate; the effects' lines are sized up to 48 kHz */
#define FS obv_fs
#define HALF_FRAMES 256u                        /* the engine's largest block */
#define NBLOB 8u                                /* (the 8 patterns) */
#include "obv_dsp.h"
#include "obv_params.h"
#include "obv_fx.h"
#include "obv_engine.h"
#include "obv_seq.h"
#include "obv_patches.h"
#include "o3_core.h"
#include "o3_draw.h"
#include "o3_store.h"
#include "o3_splash.h"
#include "o3_pages.h"
#include "o3_mixer.h"
#include "o3_seq.h"
#include "o3_seqview.h"
#include "o3_daw.h"

static int shift, octave, menu, f5_down, f5_used;
#include "o3_menu.h"

/* Protokoll 5 (6.2): the audio may run beside events and the display (VIBE: its own realtime thread, 4 ms buffer).
 * What OberVave shares between the two: single-writer controls (ctl_* / ui_*), the note and arp rings, the pattern,
 * sound and mix swap (pat_q, snd_q, mx_q – flag after a RING_PUBLISH, the ISR swaps on the downbeat), the tracks'
 * snapshot for REC (a seqlock). The audio flow never logs, saves, draws or touches the panel's state. */
VIBE_PARALLEL_AUDIO

static float enc_acc[VE_COUNT];
static int enc_steps(int e, float v)            /* whole detents from fractional turns (the mouse, a slow knob) */
{
    enc_acc[e] += v;
    int s = (int)enc_acc[e]; enc_acc[e] -= (float)s;
    return s;
}
static int coarse(int s)                        /* DATA for jumps (pattern, bar): one per turn, at most every 150 ms */
{
    static uint32_t t0;
    if (!s || o3_ms - t0 < 150u) return 0;
    t0 = o3_ms; return s > 0 ? 1 : -1;
}

/* ---- audio: the engine renders straight into the runtime's block (in pieces of up to 64 frames, a multiple of its
 * control block) – nothing rendered ahead, so a key changes the very next block (lowest latency). Only a block that is
 * no multiple of 16 frames goes through a small queue. After each piece REC takes the arp's notes (on time). */
#define EB 64u
static int32_t eng_out[2u * EB];
static float eng_l[EB], eng_r[EB];
static uint32_t eng_pos, eng_end, ms_acc;      /* eng_l/r[eng_pos..eng_end): rendered, not yet played */
static uint32_t eng_frames;                     /* frames rendered (the MIDI clock is measured in them) */
static uint32_t clk_last;
static void engine_piece(uint32_t n)            /* n frames (a multiple of 16) into eng_l / eng_r */
{
    memset(eng_out, 0, 2u * n * sizeof eng_out[0]);
    engine_audio(eng_out, n);
    for (uint32_t j = 0; j < n; j++) { eng_l[j] = (float)eng_out[2u * j] * (1.0f / 32768.0f); eng_r[j] = (float)eng_out[2u * j + 1u] * (1.0f / 32768.0f); }
    for (ms_acc += n * 1000u; ms_acc >= obv_fs; ms_acc -= obv_fs) o3_ms++;
    eng_frames += n;
    if (ctl_ext_quarter && o3_ms - clk_last > 1000u) ctl_ext_quarter = 0;   /* the clock stopped: our own tempo again */
}
/* (SDK 8) the DAW's song position: when it starts playing, or its place is not where the last block said it would be (a
 * loop going round, a jump), the sequencer chases it. `ahead`: frames already made (the queue) – they come first. */
static double daw_expect = -1.0;
static void daw_follow(uint32_t ahead, uint32_t frames)
{
    const vibe_transport_info *tp = vibe_transport();
    double per;
    if (!tp->has || !tp->playing || tp->bpm <= 0.0f || set.sync_int) { daw_expect = -1.0; return; }
    per = (double)tp->bpm / 60.0 / (double)obv_fs;
    if (tp->beat >= 0.0 && (daw_expect < 0.0 || tp->beat - daw_expect > 0.02 || daw_expect - tp->beat > 0.02)) {
        seq_chase_beat = tp->beat + ahead * per; seq_chase_bar = tp->bar_start;
        if (seq_chase_beat - seq_chase_bar >= (double)BAR_BEATS) seq_chase_bar += BAR_BEATS;
        if (daw_expect < 0.0) { ctl_seq_chase = 1; ctl_seq_restart = 1; ctl_seq_run = 1; ctl_arp_restart = 1; }   /* it starts */
        else if (ctl_seq_run) ctl_seq_chase = 1;  /* it jumped (stopped here by hand: stays stopped) */
    }
    daw_expect = tp->beat + frames * per;
    if (tp->loop && tp->loop_end > tp->loop_start && tp->beat < tp->loop_end && tp->loop_start >= 0.0) {
        double k = (tp->loop_end - tp->beat) / per; /* frames to the loop's end: inside this block → to the frame (whatever */
        if (k < (double)frames) {               /* the host's blocks), and the next block is expected back at its start */
            uint64_t bars = (uint64_t)(tp->loop_start / BAR_BEATS);
            seq_loop_at = eng_t - ahead + (uint32_t)(k + 0.5); seq_loop_on = ctl_seq_run;
            seq_loop_beat = tp->loop_start; seq_loop_bar = (double)(bars * BAR_BEATS);
            daw_expect = tp->loop_start + ((double)frames - k) * per;
        }
    }
}
/* 3.1 (SDK 9): MIDI OUT – the tracks' notes on the global channel and the next three (OMNI: 1-4); nothing while it is off */
static void mo_note(uint32_t part, uint32_t note, uint32_t vel, uint32_t on)
{
    if (!set.midi_out) return;
    uint32_t ch = ((set.midi_ch ? set.midi_ch - 1u : 0u) + part) & 15u;
    vibe_midi_send_at((uint8_t)((on ? 0x90u : 0x80u) | ch), (uint8_t)(note & 127u), (uint8_t)(on ? (vel ? vel & 127u : 1u) : 0u), (int)(mo_base + mo_tick));
}
static void mo_all_off(uint32_t part)           /* a panic, a track's notes off: All Notes Off on its channel */
{
    if (!set.midi_out) return;
    vibe_midi_send_at((uint8_t)(0xB0u | (((set.midi_ch ? set.midi_ch - 1u : 0u) + part) & 15u)), 123u, 0u, (int)(mo_base + mo_tick));
}
static uint32_t mo_ch(void) { return set.midi_ch ? set.midi_ch : 1u; }   /* track A's channel as shown (1..16) */

static uint16_t dump_cursor = 0xFFFFu;

void vibe_on_audio(const float *in, float *l, float *r, int n)
{
    int i = 0;
    (void)in;
    /* (3.1) the clock out: ours, while MIDI OUT is on and no clock comes in (a DAW that leads silences it in the SDK) */
    vibe_clock_out(set.midi_out && !(clk_last && o3_ms - clk_last < 2000u && ctl_ext_quarter));
    daw_sync();                                 /* (SDK 8) what changed in a sound: to the DAW */
    if (dump_cursor < NPARAM) {
        uint32_t dp = dump_cursor++;
        uint8_t cc = cc_of_param(dp);
        if (cc) vibe_midi_send_at((uint8_t)(0xB0u | (set.midi_ch ? (set.midi_ch - 1u) & 15u : 0u)), cc, (uint8_t)((live_p[sel].v[dp] * 127u + PDEF[dp].max / 2) / PDEF[dp].max), 0);
    }
    daw_follow((uint32_t)(eng_end - eng_pos), (uint32_t)n);   /* (SDK 8) the DAW starts or jumps: the steps where it is */
    while (i < n && eng_pos < eng_end) { l[i] = eng_l[eng_pos]; r[i] = eng_r[eng_pos]; eng_pos++; i++; }   /* (what a queue still holds) */
    while (n - i >= 16) {                       /* the direct way */
        uint32_t m = (uint32_t)(n - i) >= EB ? EB : (uint32_t)(n - i) & ~15u;
        mo_base = (uint32_t)i;
        engine_piece(m);
        for (uint32_t j = 0; j < m; j++) { l[i + j] = eng_l[j]; r[i + j] = eng_r[j]; }
        i += (int)m;
    }
    if (i < n) {                                /* a remainder under 16: one piece, the rest kept for the next block */
        mo_base = (uint32_t)i;
        engine_piece(16u);
        eng_pos = 0; eng_end = 16u;
        while (i < n) { l[i] = eng_l[eng_pos]; r[i] = eng_r[eng_pos]; eng_pos++; i++; }
    }
    master(l, r, n);
}

uint8_t cc_map[128];                     /* MIDI CC -> parameter + 1 (2.0's: fixed ones, the rest from CC 20 up) */
void cc_map_init(void)
{
    uint32_t i, next = 20;
    for (i = 0; i < NPARAM; i++) if (PDEF[i].cc) cc_map[PDEF[i].cc] = (uint8_t)(i + 1u);
    for (i = 0; i < NPARAM; i++) {
        if (PDEF[i].cc) continue;
        while (next < 119u && (cc_map[next] || next == 64u)) next++;
        if (next < 119u) cc_map[next++] = (uint8_t)(i + 1u);
    }
}

void vibe_on_init(float sr)
{
    uint32_t t;
    obv_fs = (uint32_t)(sr + 0.5f); fx_fs = obv_fs > 48000u ? 48000u : obv_fs; o3_sr = sr;
    vf_mode(VF_ON_HOLD);                         /* (SDK 10 FOCUS: DATA stays the sound / pattern; ENTER held 0.6 s = FOCUS) */
    obv_tables_init(sr);
    engine_init();
    cc_map_init();
    store_boot();
    for (t = 0; t < NTRK; t++) trk_load(t, set.tprog[t]);
    mix_apply();
    daw_init(); daw_sync();                     /* (SDK 8) the DAW's parameters, at the sounds just loaded */
    daw_editor();                               /* (SDK 9) the Editor as OberVave wants it: cards, XY filter, curves */
    seq_boot();
    scope_state = 1;
    splash_unpack();
    vibe_log("OberVave 3.1 (native VIBE)");
}

static void leave_seq_edit(void) { step_held = -1; audition_off(); entry_reset(); }
static void go(uint32_t pg) { if (pg != page) leave_seq_edit(); page = pg; menu = 0; }
static void prog_step(int s)                    /* the selected track's program -/+ */
{
    trk_load(sel, (uint32_t)(((int32_t)trk_prog[sel] + s % (int32_t)NPROG + (int32_t)NPROG) % (int32_t)NPROG));
    set_touch();
}
static uint32_t pad_note(uint32_t pad)          /* pads 9-16 = 2.0's white keys 9-16 from F3: G4 .. G5 */
{
    static const uint8_t W[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
    return (uint32_t)clampi(53 + 12 * set.oct + W[pad % 16u], 0, 127);
}
static uint8_t pad_sent[VIBE_PADS];             /* the note a held pad plays + 1 */
static uint8_t key_sent[VIBE_KEYS];

static void seq_key(void)                       /* SEQ: into the sequencer (TRACKS); there, the next view */
{
    if (!in_seq()) { seq_back = page; go(PG_SEQ); seq_view = SV_TRACKS; return; }
    menu = 0; leave_seq_edit();
    seq_view = (seq_view + 1u) % NSV;
    if (seq_view == SV_MORE) seq_view = SV_TRACK;   /* (STEP+ is SHIFT in STEPS) */
    if (seq_view == SV_SLOTS) slot_sel = pat_queue ? pat_queue - 1u : set.pat;
}
static void tracks_key(void)                    /* F2: TRACKS; there, back */
{
    if (in_seq() && seq_view == SV_TRACKS && !menu) { go(seq_back == PG_SEQ ? PG_PERFORM : seq_back); return; }
    if (!in_seq()) seq_back = page;
    go(PG_SEQ); seq_view = SV_TRACKS;
}

static void button(int id, int down)
{
    if (id == VB_SHIFT) { shift = down; seq_shift = down; return; }
    if (id == VB_REC) { if (!reset_ask) rec_press(down, shift); return; }
    if (id == VB_F5) {                          /* SAVE: tapped asks / saves (in the sequencer: the pattern now); held + ENTER: the dice; SHIFT: HOLD */
        if (down) { f5_down = 1; f5_used = 0; if (shift) { hold ^= 1u << sel; ctl_hold = hold; f5_used = 1; say(hold >> sel & 1u ? "HOLD ON" : "HOLD OFF"); } }
        else {
            if (f5_down && !f5_used) {
                if (in_seq()) {                 /* the pattern: asks first (− / + of a controller pass over F5), tapped again: saved */
                    if (!pat_save_armed) { pat_save_armed = 1; save_t = o3_ms; }
                    else { pat_save_armed = 0; pats_dirty |= 1u << set.pat; store_t = o3_ms - 2000u; say("PATTERN SAVED"); }
                }
                else save_tap();
            }
            f5_down = 0;
        }
        return;
    }
    if (!down) return;
    if (reset_ask) { reset_ask = 0; if (id == VB_ENTER || id == VB_DATA_PUSH) reset_all(); return; }   /* the question: ENTER = yes */
    if (id != VB_DATA_PUSH && id != VB_ENTER) save_armed = pat_save_armed = 0;
    switch (id) {
    case VB_K1 + CK_SOUND: if (in_seq() && SV_ENTRY(seq_view) && !menu) seq_clear_cursor(); else go(cycle_page(PG_OSC1, 5)); break;
    case VB_K1 + CK_ENV: go(cycle_page(PG_FENV, 2)); break;
    case VB_K1 + CK_LFO: go(cycle_page(PG_LFO, 3)); break;
    case VB_K1 + CK_ARP: if (page == PG_ARP && !menu) { hold ^= 1u << sel; ctl_hold = hold; say(hold >> sel & 1u ? "HOLD ON" : "HOLD OFF"); } else go(PG_ARP); break;
    case VB_K1 + CK_FX: go(cycle_page(PG_CHORUS, 4)); break;
    case VB_K1 + CK_SEQ: seq_key(); break;
    case VB_F1: if (page == PG_MIXER && !menu) mix_row = (mix_row + 1u) % 7u; else { go(PG_MIXER); mix_row = 0; } break;
    case VB_F2: tracks_key(); break;
    case VB_F3: case VB_LEFT: prog_step(-1); break;
    case VB_F4: case VB_RIGHT: prog_step(1); break;
    case VB_F6: if (shift) seq_undo(); else go(page == PG_GLOBAL ? PG_VOICE : page == PG_VOICE ? PG_OUTPUT : PG_GLOBAL); break;
    case VB_HOME: case VB_RETURN: go(PG_PERFORM); break;
    case VB_MENU: menu = !menu; break;
    case VB_ENTER: case VB_DATA_PUSH:
        if (f5_down) { randomize(); f5_used = 1; save_armed = 0; say("NEW SOUND"); }
        else if (shift && page == PG_GLOBAL) reset_ask = 1;
        else if (page == PG_GLOBAL && !menu) {  /* SYNC: AUTO (a clock / the DAW leads when there is one) <-> INTERNAL */
            set.sync_int = !set.sync_int; set_touch();
            if (set.sync_int) { ctl_daw_quarter = 0; ctl_ext_quarter = 0; }
            say(set.sync_int ? "SYNC INTERNAL" : "SYNC AUTO");
        }
        break;
    case VB_OCT_DOWN: if (octave > -3) octave--; break;
    case VB_OCT_UP: if (octave < 3) octave++; break;
    case VB_PLAY: play_press(); break;
    case VB_STOP: if (ctl_seq_run) play_press(); break;
    default: break;
    }
}

static void pad(uint32_t p, int down, float vel)
{
    if (!down) {
        if (in_seq() && SV_ENTRY(seq_view)) { step_pad_up(p); return; }
        if (pad_sent[p]) { note_in(0, pad_sent[p] - 1u, 0); pad_sent[p] = 0; }
        return;
    }
    if (in_seq() && SV_ENTRY(seq_view)) { step_pad_down(p, (uint32_t)(vel * 126.0f) + 1u); return; }   /* STEPS: the 16 steps of the bar (a new one as hard as hit) */
    if (page == PG_MIXER) {                   /* row 1 start / stop – and that track is the chosen one at once (Sven 23.10.) */
        if (p < 4u) { if (p != sel) { sel = p; step_held = -1; audition_off(); entry_reset(); } perf_pad(p, shift); }
        else mixer_pad(p, shift);
        return;
    }
    if (in_seq() && seq_view == SV_TRACKS) { perf_pad(p & 7u, shift); return; }   /* TRACKS: rows 3-4 = rows 1-2 */
    if (p < 8u) { perf_pad(p, shift); return; } /* row 1 start / stop (REC: record), row 2 the track */
    pad_sent[p] = (uint8_t)(pad_note(p) + 1u);
    note_in(1, pad_sent[p] - 1u, (uint32_t)(vel * 126.0f) + 1u);
}

static void encoder(int id, float v)
{
    int s = enc_steps(id, v);
    if (!s) return;
    if (id <= VE_4) {
        if (page == PG_MIXER) mixer_turn(id, s);
        else if (in_seq()) seq_turn((uint32_t)id, s);
        else knob_turn(id, s);
    } else if (id == VE_A) {
        if (in_seq() && (SV_ENTRY(seq_view) || seq_view == SV_SLOTS)) seq_turn(0, s);   /* the step, the slot */
        else trk_select((uint32_t)clampi((int32_t)sel + s, 0, NTRK - 1));
    } else if (id == VE_B) {
        if (in_seq() && seq_view == SV_STEPS && !shift) seq_turn(1, s);   /* the cursor's note up / down */
        else if (in_seq() && seq_view == SV_TRACK) seq_turn(3, s);   /* the track transposed */
        else prog_step(s);
    } else if (id == VE_DATA) {
        if (shift) { set.tempo = (uint8_t)clampi(set.tempo + s, 40, 240); mix_apply(); set_touch(); }
        else if (save_armed) { save_to = (uint32_t)(((int32_t)save_to + s + (int32_t)NPROG * 4) % (int32_t)NPROG); save_t = o3_ms; }
        else if (in_seq() && SV_ENTRY(seq_view)) { int c = coarse(s); if (c && !entry_n) seq_turn(0, c * 16); }   /* a bar */
        else if (in_seq() || page == PG_MIXER) { int c = coarse(s); if (c) pat_go((uint32_t)(((int)(pat_queue ? pat_queue - 1u : set.pat) + c + (int)NPAT) % (int)NPAT)); }
        else if (page == PG_GLOBAL) {           /* (3.1) MIDI OUT on / off */
            uint32_t was = set.midi_out; set.midi_out = s > 0;
            if (was != set.midi_out) { set_touch(); say(set.midi_out ? "MIDI OUT ON" : "MIDI OUT OFF"); if (!set.midi_out) { ctl_panic = 1; } }
        }
        else prog_step(s);
    }
}

/* MIDI as 2.0: the global channel (GLOBAL) = track A, the next three = B-D; OMNI: every channel plays the chosen track.
 * Notes, program changes, CCs (2.0's map), bend, sustain, volume; clock (the tempo follows), start / continue / stop */
static uint32_t clk_n, clk_t0;
static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t ch = (st & 15u) + 1u, kind = st & 0xF0u;
    if (set.sync_int && (st == 0xF8u || st == 0xFAu || st == 0xFBu || st == 0xFCu)) return;   /* (SYNC: INTERNAL) */
    if (st == 0xF8u) {
        /* the tempo from 4 quarters (96 clocks) in rendered frames, rounded to 48 like our own (quarter_len); taken
         * only when it changes by more than 0.3 % – the clock's jitter (USB, the loop) must not wobble the steps */
        if (!clk_n || o3_ms - clk_last > 250u) { clk_n = 0; clk_t0 = eng_frames; }
        clk_last = o3_ms;
        if (++clk_n == 97u) {
            uint32_t q = ((eng_frames - clk_t0) / 4u + 24u) / 48u * 48u, d;
            if (q > obv_fs / 5u && q < obv_fs * 8u / 5u) {
                d = q > ctl_ext_quarter ? q - ctl_ext_quarter : ctl_ext_quarter - q;
                if (!ctl_ext_quarter || d * 1000u > 3u * q) ctl_ext_quarter = q;
            }
            clk_n = 1; clk_t0 = eng_frames;
        }
        return;
    }
    if (st == 0xFAu || st == 0xFBu) { ctl_arp_restart = 1; clk_n = 0; ctl_seq_restart = 1; ctl_seq_run = 1; return; }
    if (st == 0xFCu) { ctl_seq_run = 0; return; }
    if (st >= 0xF0u) return;
    if (set.midi_ch && ch > set.midi_ch && ch < set.midi_ch + NTRK) {   /* the next three channels: tracks B-D */
        uint32_t t = ch - set.midi_ch;
        if (kind == 0x90u && d2) note_event_p(t, 1, d1, d2);
        else if (kind == 0x80u || kind == 0x90u) note_event_p(t, 0, d1, 0);
        else if (kind == 0xC0u && d1 < NPROG) { trk_load(t, d1); set_touch(); }
        else if (kind == 0xB0u && cc_map[d1]) { uint32_t p = cc_map[d1] - 1u; if (!is_a_only(p)) live_p[t].v[p] = (uint8_t)(PDEF[p].max == 127 ? d2 : (d2 * PDEF[p].max + 63u) / 127u); }
        return;
    }
    if (set.midi_ch && ch != set.midi_ch) return;
    if (kind == 0x90u && d2) note_in(1, d1, d2);
    else if (kind == 0x80u || kind == 0x90u) note_in(0, d1, 0);
    else if (kind == 0xE0u) ctl_bend = (int32_t)(d1 | d2 << 7) - 8192;
    else if (kind == 0xC0u && d1 < NPROG) { trk_load(set.midi_ch ? 0u : sel, d1); set_touch(); }
    else if (kind == 0xB0u) {
        if (d1 == 7u) ctl_cc7 = d2;
        else if (d1 == 64u) ctl_sustain = d2 >= 64u;
        else if (d1 == 119u) {
            if (d2 == 127u) {
                prog_put(trk_prog[sel], &live_p[sel]);
                st_save(trk_prog[sel] / PS_N == 0 ? "plob0" : "plob1", &psect[trk_prog[sel] / PS_N], sizeof psect[0]);
                trk_edited[sel] = 0;
                daw_update_preset_names();
            } else {
                dump_cursor = 0;
            }
        }
        else if (d1 == 120u || d1 == 123u) ctl_panic = 1;
        else if (cc_map[d1]) { uint32_t p = cc_map[d1] - 1u; set_param(p, PDEF[p].max == 127 ? (int32_t)d2 : ((int32_t)d2 * PDEF[p].max + 63) / 127); auto_rec(p); }
    }
}

/* SDK 10 FOCUS (ENTER or DATA held): the area (the title), the area's pages (the chips), the track and the sound (the
 * header), the four knobs (as encoders 1-4), F1-F6 – everything with one knob and one button */
#define FO_AREA 900
#define FO_CHIP 800
static void focus_press(int id)
{
    if (id == FO_AREA) { int c = in_seq() ? CK_SEQ : key_lit(); c = c < 0 ? 0 : (c + 1) % 6; button(VB_K1 + c, 1); button(VB_K1 + c, 0); return; }
    if (id >= FO_CHIP && id < FO_CHIP + 8) {
        static const uint8_t F6P[3] = {PG_GLOBAL, PG_VOICE, PG_OUTPUT};
        const group_t *g = page_group(page); uint32_t i = (uint32_t)(id - FO_CHIP); go(g->key == 6 ? F6P[i % 3] : g->first + i);
    }
}
static void focus_items(void)                   /* (in the draw: where each one is shown) */
{
    vf_button(FO_AREA, 8, 4, 150, 26);
    vf_encoder(VE_A, 174, 4, 56, 26);           /* the track (A-D) */
    vf_encoder(VE_B, 236, 4, 160, 26);          /* the sound */
    if (page == PG_MIXER || in_seq()) { for (int i = 0; i < 4; i++) vf_encoder(VE_1 + i, colx(i) - COLW / 2 + 4, 40, COLW - 8, 236); }
    else if (page == PG_GLOBAL) { for (int i = 0; i < 4; i++) vf_encoder(VE_1 + i, CX0 + 2, 126 + i * 36, 300, 30); }
    else {
        const group_t *grp = page_group(page);
        uint32_t n = grp->key == 6 ? 3u : grp->count;
        if (n >= 2) { int w = (CW + 6) / (int)n; for (uint32_t i = 0; i < n; i++) vf_button(FO_CHIP + (int)i, CX0 + (int)i * w, 41, w - 6, 26); }
        int y = grp->count > 1 || grp->key == 6 ? 74 : 50;
        for (int i = 0; i < 4; i++) vf_encoder(VE_1 + i, colx(i) - COLW / 2 + 4, y - 2, COLW - 8, 128);
    }
    for (int i = 0; i < 6; i++) vf_key(VB_F1 + i, 40 + i * 80 - 37, 286, 74, 30);
}
void vibe_on_event(const vibe_event *e)
{
    { int id = -1, r = vf_event(e, &id); if (r == VF_PRESS) focus_press(id); if (r != VF_NO) return; }
    arp_rec_drain();                            /* (what the arp played so far is in before this key) */
    switch (e->type) {
    case VIBE_EV_KEY: {
        uint32_t k = e->id % VIBE_KEYS;
        if (e->down) {
            int n = 48 + (int)k + 12 * octave;
            if (n < 0 || n > 127) break;
            key_sent[k] = (uint8_t)(n + 1);
            note_in(1, (uint32_t)n, (uint32_t)(e->value * 126.0f) + 1u);
        } else if (key_sent[k]) { note_in(0, key_sent[k] - 1u, 0); key_sent[k] = 0; }
        break; }
    case VIBE_EV_PAD: pad(e->id % VIBE_PADS, e->down, e->value); break;
    case VIBE_EV_BUTTON: if (splash_on && e->down) { splash_on = 0; break; } button(e->id, e->down); break;   /* (a button ends the start screen) */
    case VIBE_EV_ENCODER: encoder(e->id, e->value); break;
    case VIBE_EV_MIDI: midi(e->midi[0], e->midi[1], e->midi[2]); break;
    case VIBE_EV_PARAM: daw_from_host(e->id, e->value); break;   /* (SDK 8) the DAW's automation */
    case VIBE_EV_PEDAL: if (e->id == VIBE_PEDAL_SWITCH) ctl_sustain = e->down; break;
    case VIBE_EV_TRANSPORT:                     /* (SDK 7) the DAW / a MIDI clock: its tempo exactly, play from the top / stop */
        if (set.sync_int) break;                /* (GLOBAL SYNC: INTERNAL) */
        if (e->id == VIBE_TRANSPORT_TEMPO) ctl_daw_quarter = e->value >= 20.0f && e->value <= 400.0f ? (uint32_t)(obv_fs * 60.0f / e->value + 0.5f) : 0u;
        else if (e->id == VIBE_TRANSPORT_PLAY) {
            if (e->down) { ctl_arp_restart = 1; ctl_seq_restart = 1; ctl_seq_run = 1; }
            else ctl_seq_run = 0;
        }
        break;
    default: break;
    }
}

/* ---- the display */
static const group_t *grp;
static void group_chips(void)                   /* the colour key's pages, the open one bright (F6: GLOBAL, VOICE, OUTPUT) */
{
    static const uint8_t F6P[3] = {PG_GLOBAL, PG_VOICE, PG_OUTPUT};
    uint32_t n = grp->key == 6 ? 3u : grp->count;
    if (n < 2) return;
    int w = (CW + 6) / (int)n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t pg = grp->key == 6 ? F6P[i] : grp->first + i, on = pg == page, col = grp->key < 6 ? KEY_RGB[grp->key] : 0xE8423A;
        vg_round(CX0 + (int)i * w, 41, w - 6, 26, 5, on ? col : CELL);
        vg_text(PAGE[pg].title, CX0 + (int)i * w + (w - 6) / 2, 46, w > 100 ? VG_MED : VG_BOLD, on ? 0x111214 : DIM, VG_CENTER);
    }
}
static void env_graph(void)                     /* FILTER ENV / AMP ENV: the shape */
{
    const patch_t *pv = &live_p[sel];
    int a = page == PG_FENV ? P_FA : P_AA;
    float x0 = 22, y0 = 270, h = 60, w = 436;
    float ta = (float)pv->v[a] / 127.0f, td = (float)pv->v[a + 1] / 127.0f, s = (float)pv->v[a + 2] / 127.0f, tr = (float)pv->v[a + 3] / 127.0f;
    float tot = ta + td + 0.35f + tr + 0.01f, k = w / tot;
    float xa = x0 + ta * k, xd = xa + td * k, xs = xd + 0.35f * k, xr = xs + tr * k;
    uint32_t col = page == PG_FENV ? KEY_RGB[CK_ENV] : TRK_RGB[sel];
    vg_rect(CX0, 204, CW, 72, CELL);
    vg_line(x0, y0, xa, y0 - h, 2.5f, col); vg_line(xa, y0 - h, xd, y0 - h * s, 2.5f, col);
    vg_line(xd, y0 - h * s, xs, y0 - h * s, 2.5f, col); vg_line(xs, y0 - h * s, xr, y0, 2.5f, col);
}
static void sound_tags(void)
{
    const patch_t *pv = &live_p[sel];
    char b[48]; b[0] = 0;
    scat(b, MODE_NAME[pv->v[P_MODE] & 3u]); scat(b, " · "); scat(b, FTYPE_NAME[pv->v[P_FTYPE] % NFTYPE]);
    if (pv->v[P_ARP_MODE]) { scat(b, " · ARP "); scat(b, ARP_NAME[pv->v[P_ARP_MODE] % NARPMODE]); }
    if (hold >> sel & 1u) scat(b, " · HOLD");
    if (octave) { char o[4]; scat(b, " · OCT "); if (octave > 0) scat(b, "+"); vg_itoa(octave, o); scat(b, o); }
    vg_text(b, 240, 214, VG_MED, octave ? ACCENT : DIM, VG_CENTER);
}
static void scope_view(void)
{
    static int16_t shown[SCOPE_N];
    if (scope_state == 3u) { memcpy(shown, scope, sizeof shown); scope_state = 1; }
    else if (!scope_state) scope_state = 1;
    float px = CX0, py = 256;
    for (uint32_t i = 1; i < SCOPE_N; i++) {
        float x = CX0 + (float)i * (float)CW / (SCOPE_N - 1), y = 256 - vd_clamp((float)shown[i] / 32768.0f, -0.9f, 0.9f) * 20.0f;
        vg_line(px, py, x, y, 1.6f, vg_mix(BG, TRK_RGB[sel], 0.8f)); px = x; py = y;
    }
}
static void global_view(void)
{
    knob_t k[4]; page_knobs(k);
    uint32_t xq = ctl_daw_quarter ? ctl_daw_quarter : clk_last && o3_ms - clk_last < 2000u ? ctl_ext_quarter : 0u;
    int ext = xq != 0u;
    char t[8]; fmt_u(t, ext ? (60u * obv_fs + xq / 2u) / xq : set.tempo);
    vg_text(t, 160, 48, VG_HUGE, ext ? GREEN : FG, VG_CENTER); vg_text("BPM", 244, 82, VG_MED, DIM, VG_LEFT);
    vg_text(set.sync_int ? "SYNC: INTERNAL" : ext ? "SYNC: AUTO \xC2\xB7 CLOCK" : "SYNC: AUTO", 466, 48, VG_MED, set.sync_int ? ACCENT : ext ? GREEN : DIM, VG_RIGHT);
    {   /* (3.1) MIDI OUT: the tracks' channels */
        char m[40]; m[0] = 0;
        if (set.midi_out) { char c[8]; scat(m, "MIDI OUT: A-D ch "); fmt_u(c, mo_ch()); scat(m, c); scat(m, "-"); fmt_u(c, (mo_ch() + 3u - 1u) % 16u + 1u); scat(m, c); }
        else scat(m, "MIDI OUT: OFF");
        vg_text(m, 466, 74, VG_MED, set.midi_out ? GREEN : DIM, VG_RIGHT);
    }
    vg_text("ENTER: sync", 466, 100, VG_MED, MUTED, VG_RIGHT);
    for (int i = 0; i < 4; i++) {
        int y = 128 + i * 36; char n[2] = {(char)('1' + i), 0};
        vg_round(CX0 + 6, y, 26, 26, 4, CELL2); vg_text(n, CX0 + 19, y + 4, VG_MED, FG, VG_CENTER);
        vg_text(k[i].label, CX0 + 44, y + 4, VG_MED, FG, VG_LEFT);
        vg_text(k[i].value, CX0 + 170, y, VG_BIG, i == 1 && set.click % NCLICK ? ACCENT : 0x3FA9F5, VG_LEFT);
    }
    vg_text("F6: VOICE, OUTPUT", 466, 136, VG_MED, MUTED, VG_RIGHT);
    vg_text("SHIFT+DATA: tempo", 466, 172, VG_MED, MUTED, VG_RIGHT);
    vg_text("SHIFT+ENTER: RESET", 466, 208, VG_MED, MUTED, VG_RIGHT);
    vg_text("DATA: MIDI out", 466, 244, VG_MED, MUTED, VG_RIGHT);
}
static void save_banner(void)
{
    char b[40], n[PATCH_NAME + 1];
    if (pat_save_armed) {
        if (o3_ms - save_t > 3000u) { pat_save_armed = 0; return; }
        vg_round(70, 190, 340, 84, 8, 0x2A1A0E);
        vg_text("SAVE PATTERN?", 240, 196, VG_MED, ACCENT, VG_CENTER);
        b[0] = 'P'; fmt_u(b + 1, set.pat + 1u); vg_text(b, 240, 218, VG_BIG, FG, VG_CENTER);
        vg_text("F5 (TAP): save · any other key: no", 240, 252, VG_MED, DIM, VG_CENTER);
        return;
    }
    if (!save_armed) return;
    if (o3_ms - save_t > 3000u) { save_armed = 0; return; }
    vg_round(70, 190, 340, 84, 8, 0x2A1A0E);
    vg_text("SAVE HERE?", 240, 196, VG_MED, ACCENT, VG_CENTER);
    fmt_u(b, save_to + 1u); scat(b, "  "); prog_name(save_to, n); scat(b, n);
    vg_text(b, 240, 218, VG_BIG, FG, VG_CENTER);
    vg_text("DATA: another slot · F5 (TAP): save", 240, 252, VG_MED, DIM, VG_CENTER);
}
static void perf_leds(void)                     /* sound pages: row 1 start / stop (REC: red), row 2 the tracks, rows 3-4 notes */
{
    for (uint32_t c = 0; c < 4u; c++) {
        uint32_t s = trk_state(c);
        vibe_led_pad((int)c, rec ? RED : GREEN, s == 1u ? 1.0f : s >= 2u ? (blink() ? 1.0f : 0.2f) : 0.12f);
        vibe_led_pad((int)(4u + c), TRK_RGB[c], c == sel ? 1.0f : 0.25f);
    }
    for (int p = 8; p < 16; p++) vibe_led_pad(p, TRK_RGB[sel], pad_sent[p] ? 1.0f : 0.12f);
}

void vibe_on_draw(uint16_t *fb)
{
    static const char *const SOFT[6] = {"MIXER", "TRACKS", "PRG -", "PRG +", "SAVE", "GLOBAL"};
    static const char *const SOFT_SH[6] = {"MIXER", "TRACKS", "PRG -", "PRG +", "HOLD", "UNDO"};
    knob_t k[4];
    store_poll();
    auto_poll();
    seq_poll();
    meters_take();
    vg_begin(fb);
    if (splash_on) { splash_draw(fb); return; }
    vg_clear(BG);
    if (menu) { overview(); vf_begin(); vf_end(ACCENT); }   /* (FOCUS: nothing to frame in the overview) */
    else {
        grp = page_group(page);
        header(page == PG_MIXER ? "MIXER" : in_seq() ? "SEQUENCER" : PAGE[page].title,
               in_seq() ? KEY_RGB[CK_SEQ] : grp->key < 6 ? KEY_RGB[grp->key] : grp->key == 6 ? 0xE8423A : ACCENT);
        {   /* the pattern (→ the next one, blinking) and REC */
            char b[12]; b[0] = 'P'; fmt_u(b + 1, set.pat + 1u);
            if (pat_queue) { scat(b, ">"); fmt_u(b + slen(b), pat_queue); }
            vg_text(b, 160, 8, VG_MED, pat_queue && blink() ? ACCENT : DIM, VG_RIGHT);
            if (rec) vg_circle(130, 17, 6, rec == REC_ON || blink() ? RED : 0x7A2A24);
        }
        if (page == PG_MIXER) mixer_view();
        else if (in_seq()) seq_view_draw();
        else if (page == PG_GLOBAL) global_view();
        else {
            group_chips();
            page_knobs(k);
            knobs_row(k, grp->count > 1 || grp->key == 6 ? 74 : 50, page == PG_DRIVE || (page >= PG_CHORUS && page <= PG_REVERB) ? KEY_RGB[CK_FX] : TRK_RGB[sel]);
            if (page == PG_FENV || page == PG_AENV) env_graph(); else { sound_tags(); scope_view(); }
        }
        softkeys(shift ? SOFT_SH : SOFT, (global_lit() ? 32u : 0u) | (save_armed || f5_down ? 16u : 0u) | (page == PG_MIXER ? 1u : 0u)
                 | (in_seq() && seq_view == SV_TRACKS ? 2u : 0u));
        vf_begin(); focus_items(); vf_end(ACCENT);
        save_banner();
        if (reset_ask) reset_view();
        toast_draw();
    }
    if (page == PG_MIXER) mixer_leds(); else if (in_seq()) seq_leds(); else perf_leds();
    for (int i = 0; i < 6; i++) vibe_led_button(VB_K1 + i, KEY_RGB[i], (in_seq() ? CK_SEQ : key_lit()) == i ? 1.0f : 0.15f);
    vibe_led_button(VB_OCT_DOWN, 0xE6D1B9, octave < 0 ? 0.4f + 0.2f * (float)(-octave) : 0.0f);   /* the keys moved down / up: */
    vibe_led_button(VB_OCT_UP, 0xE6D1B9, octave > 0 ? 0.4f + 0.2f * (float)octave : 0.0f);        /* brighter the further */
    vibe_led_button(VB_PLAY, GREEN, ctl_seq_run ? 1.0f : 0.0f);
    vibe_led_button(VB_REC, RED, rec == REC_ON ? 1.0f : rec == REC_READY ? (blink() ? 1.0f : 0.2f) : 0.0f);
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* OberVave 3 – the sequencer as the panel sees it: recording, step editing, the views, the tracks' start / stop on the
 * bar, the 8 patterns. The logic is OberVave 2.0's (obervave.c), without the FM-1's layers and flash: all 8 patterns
 * live in memory and are saved ~1.5 s after a change, also while playing. Playing them is obv_seq.h's (the "ISR"). */
enum { REC_OFF, REC_READY, REC_ON };            /* REC: off, armed (the first note starts the loop), recording */
enum { SV_TRACKS, SV_STEPS, SV_MORE, SV_TRACK, SV_SLOTS, NSV };   /* STEP+ (SV_MORE) = SHIFT held in STEPS */
static const char *const SV_NAME[NSV] = {"TRACKS", "STEPS", "STEP+", "TRACK", "PATTERNS"};
#define SV_ENTRY(v) ((v) == SV_STEPS || (v) == SV_MORE)
static const char *const RQ_NAME[NRQ] = {"OFF", "1/32", "1/16", "1/8", "1/4"};
static const uint8_t RQ_PER_QUARTER[NRQ] = {0, 8, 4, 2, 1};

static uint32_t seq_view, seq_back = PG_PERFORM, seq_cur[NTRK], seq_pg[NTRK], rec, rec_wait, rec_from_arp;
static int seq_shift;                           /* SHIFT held: STEPS shows STEP+ */
#define SV_EFF (seq_view == SV_STEPS && seq_shift ? (uint32_t)SV_MORE : seq_view)
static struct { uint16_t step; uint32_t t; } rec_open[128];
static uint32_t rec_on_t[NTRK][SEQ_STEPS];      /* a note's start written into the step in this recording (o3_ms + 1) */
static void undo_save(void);
static void rec_begin(void) { rec = REC_ON; undo_save(); memset(rec_on_t, 0, sizeof rec_on_t); }
static pattern_t pats[NPAT], undo_pat;          /* every pattern in memory; one level of undo */
static uint32_t undo_ok, pat_edited, pats_dirty, pat_used, pat_queue, slot_sel, copy_arm, copy_arm_t, swap_seen;
static uint32_t pen = 60, pen_n, notes_down;
static uint8_t pen_chord[4];
static int32_t step_held = -1;                  /* the step whose pad is held in STEPS, -1 none */
static uint32_t step_turned, step_midi;
static uint8_t audition[SEQ_CHORD], audition_part;
static uint32_t entry_n;
static uint8_t entry_on_note[128];

static int in_seq(void) { return page == PG_SEQ; }
static uint32_t seq_pages(uint32_t t) { return pat.trk[t].len ? (pat.trk[t].len + 15u) / 16u : 1u; }
static void seq_cursor_set(uint32_t t, uint32_t n) { seq_cur[t] = n; seq_pg[t] = n / 16u; }
static void undo_save(void) { undo_pat = pat; undo_ok = 1; }
static void edited(void) { pat_edited = 1; pats_dirty |= 1u << set.pat; store_t = o3_ms; }
/* a knob turned while REC records (the sequencer playing, the track on): its value onto the step the track plays now, in
 * the lane of that parameter (a new one if there is none yet, NLANE per track). While it turns, the player leaves the
 * lane to the hand (ctl_auto_hold, let go 250 ms after the last turn – auto_poll) */
static uint32_t auto_hold_t[NTRK][NLANE];
static void say_trk(const char *what, uint32_t t);
static void auto_rec(uint32_t p)
{
    uint32_t t = sel % NTRK, l, f = NLANE, len = pat.trk[t].len;
    if (rec != REC_ON || !ctl_seq_run || !len || !(ctl_trk_on >> t & 1u) || is_a_only(p) || p >= NPARAM) return;
    for (l = 0; l < NLANE; l++) { if (pat.lane[t][l].param == p) break; if (f == NLANE && pat.lane[t][l].param == AUTO_NONE) f = l; }
    if (l == NLANE) {
        if (f == NLANE) { say_trk(": 4 KNOBS RECORDED ALREADY", t); return; }
        l = f; lane_clear(&pat.lane[t][l]); pat.lane[t][l].param = (uint8_t)p;
    }
    pat.lane[t][l].v[ui_seq_pos[t] % len] = live_p[t].v[p];
    ctl_auto_hold[t] |= 1u << l; auto_hold_t[t][l] = o3_ms;
    edited();
}
static void auto_poll(void)                     /* (main loop) a lane let go: the player has it again */
{
    for (uint32_t t = 0; t < NTRK; t++) for (uint32_t l = 0; l < NLANE; l++)
        if ((ctl_auto_hold[t] >> l & 1u) && o3_ms - auto_hold_t[t][l] > 250u) ctl_auto_hold[t] &= ~(1u << l);
}
static void say_trk(const char *what, uint32_t t)
{
    static char m[32]; m[0] = 0; scat(m, "TRACK "); { char l[2] = {(char)('A' + t % NTRK), 0}; scat(m, l); } scat(m, what); say(m);
}

/* ---- recording: where on track t the note was HEARD, on the track's REC grid (OFF: as played).
 * A key: the host says which frame was audible when it came (Protokoll 5) and the ISR's snapshot gives the place of
 * that frame – exact, whatever the buffer, the USB, the thread. A host without it (SDK 4, the browser today): now,
 * ~11 ms back. An arp note: where the ISR played it (rec_arp_q8). */
static uint32_t rec_arp_q8 = 0xFFFFFFFFu;
static uint32_t rec_where(uint32_t t, uint32_t *nudge)
{
    uint32_t len = pat.trk[t].len, dur = ui_seq_dur[t] ? ui_seq_dur[t] : 1u, back = 529u * 256u / dur, rq = set.rec_qt[t % NTRK] % NRQ;
    uint32_t q8 = ui_seq_pos[t] * 256u + (ui_seq_frac[t] > 256u ? 256u : ui_seq_frac[t]), g8;
    uint64_t heard;
    if (rec_from_arp) {
        rq = RQ_OFF;
        if (rec_arp_q8 != 0xFFFFFFFFu) q8 = rec_arp_q8 % (len * 256u);
    } else if (vibe_event_heard(&heard) && snap_q8(t, (uint32_t)heard, &q8)) ;
    else q8 = q8 >= back ? q8 - back : q8 + len * 256u - back;
    if (rq != RQ_OFF) {
        g8 = 256u * DIV_PER_QUARTER[pat.trk[t].div % NDIV] / RQ_PER_QUARTER[rq];
        if (!g8) g8 = 1;
        q8 = (q8 + g8 / 2u) / g8 * g8;
    }
    {                                           /* on a step's line (an arp note, or a key within ~1 ms): on it */
        uint32_t snapw = rec_from_arp ? 24u : 3u;
        if ((q8 & 255u) < snapw) q8 &= ~255u;
        else if ((q8 & 255u) > 255u - snapw) q8 = (q8 | 255u) + 1u;
    }
    q8 %= len * 256u;
    *nudge = (q8 & 255u) >> 1;
    return q8 >> 8;
}
static void step_add(step_t *st, uint32_t note, uint32_t vel)   /* a note into a step, the ones there stay */
{
    uint32_t k;
    if (!st->note[0]) { step_clear(st); st->gate = 96; }
    for (k = 0; k < SEQ_CHORD && st->note[k] && st->note[k] != note; k++) ;
    if (k < SEQ_CHORD && !st->note[k]) st->note[k] = (uint8_t)note;
    st->vel = (uint8_t)vel;
    st->ratch &= (uint8_t)~STEP_OFF;
}
/* an arp take: from its first note while the keys (or HOLD) keep the arp going. When the loop comes round, what it
 * writes replaces what it wrote the round before (else every step got a second note – the arp goes on with its own
 * pattern – and the take turned into chords); the track's old steps are not played meanwhile (ctl_rec_mute) */
static uint32_t take_on, take_trk, take_pass, take_last;
static uint8_t take_mark[SEQ_STEPS];            /* the round (1..255) that wrote each step, 0 = not in this take */
static void take_step(uint32_t t, uint32_t s0, step_t *st)
{
    if (!take_on || take_trk != t) { take_on = 1; take_trk = t; take_pass = 1; take_last = s0; memset(take_mark, 0, sizeof take_mark); ctl_rec_mute = 1u << t; }
    if (s0 < take_last) take_pass = take_pass >= 255u ? 1u : take_pass + 1u;   /* (round again) */
    take_last = s0;
    if (take_mark[s0 % SEQ_STEPS] && take_mark[s0 % SEQ_STEPS] != take_pass) st->note[0] = 0;   /* (step_add: anew) */
    take_mark[s0 % SEQ_STEPS] = (uint8_t)take_pass;
}
static void take_check(void)                    /* the take is over: keys up and no HOLD, or REC off */
{
    if (take_on && (rec != REC_ON || (!notes_down && !(hold >> take_trk & 1u)) || !live_p[take_trk].v[P_ARP_MODE])) { take_on = 0; ctl_rec_mute = 0; }
}
static void rec_note(uint32_t on, uint32_t note, uint32_t vel)
{
    uint32_t t = sel, len = pat.trk[t].len, s0, n, k, held, sms, nudge = 0, sounding;
    note &= 127u; sounding = note;
    if (!len) return;
    if (ctl_count || rec_wait) return;          /* counting in, waiting for the bar: notes only sound */
    if (on && rec == REC_READY && !ctl_seq_run) {   /* the first note: from the top */
        ctl_seq_restart = 1; ctl_seq_run = 1; rec_begin(); s0 = 0;
    } else if (rec != REC_ON) return;
    else if (on) s0 = ctl_seq_restart ? 0u : rec_where(t, &nudge);
    if (on) {
        step_t *st = seq_step(t, s0);
        uint32_t was;
        if (rec_from_arp) take_step(t, s0, st);
        was = st->note[0];
        note = (uint32_t)clampi((int32_t)note - pat.trk[t].transp, 1, 127);
        step_add(st, note, vel);
        if (!was) st->nudge = (uint8_t)nudge;
        TRACE('R', s0, was ? 999u : nudge);
        rec_open[note].step = (uint16_t)s0; rec_open[note].t = o3_ms + 1u;
        rec_on_t[t % NTRK][s0 % SEQ_STEPS] = o3_ms + 1u;
        if (!rec_from_arp) ctl_live_hold[t % NTRK][sounding >> 5] |= 1u << (sounding & 31u);   /* (the key owns it now) */
        edited();
        return;
    }
    note = (uint32_t)clampi((int32_t)note - pat.trk[t].transp, 1, 127);
    if (!rec_open[note].t) return;
    held = o3_ms + 1u - rec_open[note].t;
    s0 = rec_open[note].step;
    {
        rec_open[note].t = 0;
        sms = ui_seq_dur[t] ? ui_seq_dur[t] * 1000u / obv_fs : 125u;   /* a step in ms */
        if (!sms) sms = 1;
        n = (held + sms / 2u) / sms;
        if (n > len) n = len;
        /* legato (a pad's chord let go after the next one came; or over the loop's end): the note lasts up to the next
         * step where this recording started a note, not into it – else both piled up there (4 notes a step at most: notes fell out, others hung) */
        for (k = 1; k < n; k++)
            if (rec_on_t[t % NTRK][(s0 + k) % len % SEQ_STEPS]) {
                if (k == 1u) { seq_step(t, s0)->gate = 120; return; }
                n = k; break;
            }
    }
    if (n <= 1u) { seq_step(t, s0)->gate = (uint8_t)clampi((int32_t)(held * 127u / sms), 24, 120); return; }
    for (k = 0; k < n; k++) {                   /* tied on: the note in each step it lasts (with the played velocity) */
        step_t *st = seq_step(t, (s0 + k) % len);
        step_add(st, note, vel ? vel : seq_step(t, s0)->vel);
        st->gate = k + 1u < n ? GATE_TIE : 110;
    }
}
static int rec_keys(void) { return !live_p[sel % NTRK].v[P_ARP_MODE]; }   /* (feeding an arp: its notes are recorded) */

/* ---- STEPS: a pad picks its step (tapped: on / off), held + a later one: a long note; keys write into the cursor */
static void audition_off(void)
{
    for (uint32_t k = 0; k < SEQ_CHORD; k++) if (audition[k]) { note_event_p(audition_part, 0, audition[k], 0); audition[k] = 0; }
}
static void step_pad_down(uint32_t pad, uint32_t vel)   /* vel: how hard the pad was hit (a new step gets it) */
{
    static char msg[24];
    uint32_t t = sel, len = pat.trk[t].len, n = seq_pg[t] * 16u + pad, i;
    if (n >= len) { say(len ? "PAST THE TRACK'S LENGTH" : "TRACK OFF: TURN LENGTH"); return; }
    if (step_held >= 0 && n > (uint32_t)step_held) {   /* hold one step, tap a later one: a long note to there */
        step_t *a = seq_step(t, (uint32_t)step_held);
        undo_save();
        if (!a->note[0]) { step_clear(a); a->note[0] = (uint8_t)pen; }
        a->ratch &= (uint8_t)~STEP_OFF;
        for (uint32_t m = (uint32_t)step_held; m <= n; m++) {
            step_t *st = seq_step(t, m);
            if (m != (uint32_t)step_held) { uint8_t r = (uint8_t)(a->ratch & 3u); *st = *a; st->ratch = r; }
            st->gate = m < n ? GATE_TIE : 110;
        }
        step_turned = 1; edited();
        msg[0] = 0; scat(msg, "LONG NOTE "); fmt_u(msg + slen(msg), (uint32_t)step_held + 1u); scat(msg, "-"); fmt_u(msg + slen(msg), n + 1u);
        say(msg);
        return;
    }
    step_held = (int32_t)n; step_turned = step_midi = 0;
    seq_cur[t] = n;
    {
        step_t *st = seq_step(t, n);
        if (!st->note[0] || st->ratch & STEP_OFF) {   /* an empty step is set at once, with the last note or chord */
            undo_save(); step_clear(st);
            for (uint32_t q = 0; q < (pen_n ? pen_n : 1u) && q < SEQ_CHORD; q++) st->note[q] = pen_n ? pen_chord[q] : (uint8_t)pen;
            st->vel = (uint8_t)clampi((int32_t)vel, 1, 127);   /* (as hard as the pad was hit) */
            step_turned = 1; edited();
        }
    }
    if (!ctl_seq_run) {                         /* stopped: you hear the step you hold */
        const step_t *st = seq_step(t, n);
        audition_off(); audition_part = (uint8_t)(t % NTRK);
        for (i = 0; i < SEQ_CHORD && st->note[i]; i++) {
            audition[i] = (uint8_t)clampi(st->note[i] + pat.trk[t].transp, 1, 127);
            note_event_p(audition_part, 1, audition[i], st->vel ? st->vel : 100u);
        }
    }
}
static void step_pad_up(uint32_t pad)
{
    uint32_t t = sel, n = seq_pg[t] * 16u + pad;
    if (step_held != (int32_t)n) return;
    if (!step_turned && !step_midi) { undo_save(); step_clear(seq_step(t, n)); edited(); }   /* tapped, not edited: off */
    step_held = -1;
    audition_off();
}
static void pen_add(uint32_t note)              /* the last note or chord played: what an empty step gets */
{
    if (!notes_down) pen_n = 0;
    notes_down++;
    if (pen_n < 4u) pen_chord[pen_n++] = (uint8_t)note;
    pen = note;
}
static int entry_active(void) { return in_seq() && SV_ENTRY(seq_view) && !rec && step_held < 0 && pat.trk[sel].len; }
static void entry_reset(void) { entry_n = 0; memset(entry_on_note, 0, sizeof entry_on_note); }
static void entry_note(uint32_t on, uint32_t note, uint32_t vel)
{
    uint32_t t = sel, len = pat.trk[t].len, k;
    step_t *st;
    note &= 127u;
    if (!on) {
        if (!entry_on_note[note]) return;
        entry_on_note[note] = 0;
        if (entry_n && !--entry_n && len) seq_cursor_set(t, (seq_cur[t] + 1u) % len);   /* all let go: the next step */
        return;
    }
    if (!entry_active() || entry_on_note[note]) return;
    if (seq_cur[t] >= len) seq_cursor_set(t, 0);
    st = seq_step(t, seq_cur[t]);
    if (!entry_n) {                             /* the first key: the step's notes are replaced */
        undo_save();
        if (!st->note[0] || st->ratch & STEP_OFF) step_clear(st);
        for (k = 0; k < SEQ_CHORD; k++) st->note[k] = 0;
        if (st->gate >= GATE_TIE) st->gate = 96;
        st->ratch &= (uint8_t)~STEP_OFF;
        st->vel = (uint8_t)(vel ? vel : 100u);
    }
    step_add(st, (uint32_t)clampi((int32_t)note - pat.trk[t].transp, 1, 127), st->vel);
    entry_on_note[note] = 1; entry_n++;
    edited();
}

/* a note from the keys, the pads or MIDI on the selected track: it sounds, REC takes it, the sequencer's views use it */
static void note_in(uint32_t on, uint32_t note, uint32_t vel)
{
    play_note(on, note, vel);
    if (!on) for (uint32_t t = 0; t < NTRK; t++) ctl_live_hold[t][note >> 5 & 3u] &= ~(1u << (note & 31u));   /* (let go: the sequencer's again) */
    if (rec_keys()) rec_note(on, note, vel);
    if (on) {
        pen_add(note);
        if (step_held < 0) entry_note(1, note, vel);
        else if (!rec) {                        /* a held step takes what is played: the first note starts the chord */
            step_t *sp = seq_step(sel, (uint32_t)step_held);
            uint32_t q;
            if (!step_midi++) { for (q = 0; q < SEQ_CHORD; q++) sp->note[q] = 0; sp->ratch &= (uint8_t)~STEP_OFF; }
            for (q = 0; q < SEQ_CHORD && sp->note[q] && sp->note[q] != note; q++) ;
            if (q < SEQ_CHORD && !sp->note[q]) { sp->note[q] = (uint8_t)note; sp->vel = (uint8_t)vel; }
            edited();
        }
    } else { if (notes_down) notes_down--; entry_note(0, note, 0); }
}

/* ---- each pattern's instruments and mixer (Sven 20.10./21.10.: a pattern keeps its sounds, effects and every mixer
 * setting): the four tracks' sounds with their edits and program numbers, the mixer (level, pan, the four sends, mute,
 * solo), the effects (chorus, delay, reverb, drive) and MASTER – in "psnd0".."psnd7" (2.0's files untouched). A pattern
 * without its own set takes what is playing. */
#define PSND_MAGIC 0x444E5350u                  /* "PSND": the first sets (sounds only, 20.10.) */
#define PSN2_MAGIC 0x324E5350u                  /* "PSN2": sounds + mixer + effects + master */
typedef struct {
    uint8_t lvl[NTRK], cho[NTRK], dly[NTRK], rev[NTRK], drv[NTRK]; int8_t pan[NTRK];
    uint8_t mute, solo, fx[NFXP], dv_type, dv_amt, dv_tone, dv_mix, m_vol, m_dj, m_drive, m_comp;
    uint8_t tempo;                              /* (3.2) the pattern's tempo, 40..240; 0 (an older set): the one playing stays */
} mixset_t;
typedef struct { uint32_t magic; uint8_t prog[NTRK], edited[NTRK]; patch_t p[NTRK]; mixset_t mx; } psnd_t;
static psnd_t psnd[NPAT];
static uint32_t snd_dirty;                      /* bit n: psnd[n] to save */
static const char *const PS_FILE[NPAT] = {"psnd0", "psnd1", "psnd2", "psnd3", "psnd4", "psnd5", "psnd6", "psnd7"};
static int bytes_same(const void *a, const void *b, uint32_t n) { const uint8_t *x = a, *y = b; while (n--) if (*x++ != *y++) return 0; return 1; }
static void mx_from_set(mixset_t *m)
{
    for (uint32_t t = 0; t < NTRK; t++) { m->lvl[t] = set.mix_lvl[t]; m->cho[t] = set.mix_cho[t]; m->dly[t] = set.mix_dly[t]; m->rev[t] = set.mix_rev[t]; m->drv[t] = set.mix_drv[t]; m->pan[t] = set.mix_pan[t]; }
    m->mute = set.mix_mute; m->solo = set.mix_solo;
    for (uint32_t i = 0; i < NFXP; i++) m->fx[i] = set.fx[i];
    m->dv_type = set.dv_type; m->dv_amt = set.dv_amt; m->dv_tone = set.dv_tone; m->dv_mix = set.dv_mix;
    m->m_vol = set.m_vol; m->m_dj = set.m_dj; m->m_drive = set.m_drive; m->m_comp = set.m_comp;
    m->tempo = set.tempo;
}
static void mx_to_set(const mixset_t *m)        /* into the settings and the engine */
{
    for (uint32_t t = 0; t < NTRK; t++) { set.mix_lvl[t] = m->lvl[t]; set.mix_cho[t] = m->cho[t]; set.mix_dly[t] = m->dly[t]; set.mix_rev[t] = m->rev[t]; set.mix_drv[t] = m->drv[t]; set.mix_pan[t] = m->pan[t]; }
    set.mix_mute = m->mute; set.mix_solo = m->solo;
    for (uint32_t i = 0; i < NFXP; i++) set.fx[i] = m->fx[i];
    set.fx_ok = FX_OK;
    set.dv_type = m->dv_type; set.dv_amt = m->dv_amt; set.dv_tone = m->dv_tone; set.dv_mix = m->dv_mix;
    set.m_vol = m->m_vol; set.m_dj = m->m_dj; set.m_drive = m->m_drive; set.m_comp = m->m_comp;
    if (m->tempo >= 40u && m->tempo <= 240u) set.tempo = m->tempo;   /* (3.2) each pattern its tempo */
    mix_apply();
}
static void mx_prepare(uint32_t n)              /* pattern n's mixer, drive, master as the ISR takes them (mx_q) */
{
    const psnd_t *s = &psnd[n % NPAT];
    uint32_t t, solo, mute;
    if (s->magic != PSN2_MAGIC) {               /* (no own set: the mix stays as it is) */
        for (t = 0; t < NTRK; t++) { mx_q.lvl[t] = ctl_mix_lvl[t]; mx_q.cho[t] = ctl_mix_cho[t]; mx_q.dly[t] = ctl_mix_dly[t]; mx_q.rev[t] = ctl_mix_rev[t]; mx_q.drv[t] = ctl_mix_drv[t]; mx_q.pan[t] = ctl_mix_pan[t]; }
        mx_q.off = ctl_mix_off; mx_q.dv_type = ctl_dv_type; mx_q.dv_amt = ctl_dv_amt; mx_q.dv_tone = ctl_dv_tone; mx_q.dv_mix = ctl_dv_mix;
        mx_q.m_vol = ctl_m_vol; mx_q.m_dj = ctl_m_dj; mx_q.m_drive = ctl_m_drive; mx_q.m_comp = ctl_m_comp;
        mx_q.tempo = 0;
        return;
    }
    for (t = 0; t < NTRK; t++) {
        mx_q.lvl[t] = s->mx.lvl[t] > 127u ? 127u : s->mx.lvl[t]; mx_q.pan[t] = (int8_t)clampi(s->mx.pan[t], -64, 64);
        mx_q.cho[t] = s->mx.cho[t] > 127u ? 127u : s->mx.cho[t]; mx_q.dly[t] = s->mx.dly[t] > 127u ? 127u : s->mx.dly[t];
        mx_q.rev[t] = s->mx.rev[t] > 127u ? 127u : s->mx.rev[t]; mx_q.drv[t] = s->mx.drv[t] > 127u ? 127u : s->mx.drv[t];
    }
    solo = s->mx.solo & ((1u << NTRK) - 1u); mute = s->mx.mute & ((1u << NTRK) - 1u);
    mx_q.off = solo ? ~solo & ((1u << NTRK) - 1u) : mute;
    mx_q.dv_type = s->mx.dv_type; mx_q.dv_amt = s->mx.dv_amt; mx_q.dv_tone = s->mx.dv_tone; mx_q.dv_mix = s->mx.dv_mix;
    mx_q.m_vol = s->mx.m_vol; mx_q.m_dj = s->mx.m_dj; mx_q.m_drive = s->mx.m_drive; mx_q.m_comp = s->mx.m_comp;
    mx_q.tempo = s->mx.tempo >= 40u && s->mx.tempo <= 240u ? s->mx.tempo : 0;
}
static void snd_capture(uint32_t n)             /* what plays now into pattern n's set */
{
    psnd_t *s = &psnd[n % NPAT];
    s->magic = PSN2_MAGIC;
    for (uint32_t t = 0; t < NTRK; t++) { s->p[t] = live_p[t]; s->prog[t] = (uint8_t)trk_prog[t]; s->edited[t] = (uint8_t)trk_edited[t]; }
    mx_from_set(&s->mx);
    snd_dirty |= 1u << (n % NPAT); store_t = o3_ms;
}
static void snd_target(uint32_t n, patch_t *out)   /* pattern n's four sounds (its effects in track A's, where the engine reads them) */
{
    const psnd_t *s = &psnd[n % NPAT];
    for (uint32_t t = 0; t < NTRK; t++) out[t] = s->magic == PSN2_MAGIC ? s->p[t] : live_p[t];
    for (uint32_t i = 0; i < NFXP; i++) out[0].v[P_CHO_RATE + i] = s->magic == PSN2_MAGIC ? s->mx.fx[i] : set.fx[i];
}
static void snd_meta(uint32_t n)                /* pattern n's program numbers and edit marks, its mixer, effects, master */
{
    const psnd_t *s = &psnd[n % NPAT];
    if (s->magic != PSN2_MAGIC) { snd_capture(n); return; }
    for (uint32_t t = 0; t < NTRK; t++) { trk_prog[t] = s->prog[t] % NPROG; trk_edited[t] = s->edited[t]; set.tprog[t] = (uint8_t)trk_prog[t]; }
    set.prog = set.tprog[0];
    mx_to_set(&s->mx);
}

/* ---- the patterns: all in memory; playing, the next one comes on the bar (the ISR swaps pat_q in) */
static void pat_switch(uint32_t n)              /* stopped: at once */
{
    static char msg[20];
    n %= NPAT;
    if (n != set.pat) {
        patch_t nx[NTRK];
        pats[set.pat] = pat; ctl_panic = 1;
        snd_capture(set.pat); snd_target(n, nx);   /* (its instruments go with it, the new one's come) */
        for (uint32_t t = 0; t < NTRK; t++) live_p[t] = nx[t];
        pat = pats[n]; set.pat = (uint8_t)n; snd_meta(n);
        memset(seq_cur, 0, sizeof seq_cur); memset(seq_pg, 0, sizeof seq_pg); pat_edited = 0; set_touch();
    }
    slot_sel = n;
    msg[0] = 0; scat(msg, "PATTERN "); fmt_u(msg + slen(msg), n + 1u);
    if (!(pat_used >> n & 1u)) scat(msg, " (NEW)");
    say(msg);
}
static uint32_t pat_requeue;                    /* n + 1: asked for while the ISR was swapping the last one in */
static void pat_go(uint32_t n)
{
    static char msg[20];
    n %= NPAT;
    if (pat_queue) {                            /* another one was next: take it back – unless the ISR is swapping it in */
        uint32_t one = 1u;
        if (!__atomic_compare_exchange_n(&ctl_pat_swap, &one, 0u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { pat_requeue = n + 1u; return; }
        ctl_snd_swap = 0; ctl_mx_swap = 0; pat_queue = 0;
    }
    if (!ctl_seq_run) { pat_switch(n); return; }
    if (n == set.pat) { say("STAYS"); return; }
    ctl_pat_keep = 1;                           /* (the one left comes back in pat_old, with its edits) */
    pats[set.pat] = pat;
    pat_q = pats[n];
    snd_target(n, snd_q); ctl_snd_swap = 1;     /* (its instruments, swapped in on the same downbeat) */
    mx_prepare(n); ctl_mx_swap = 1;             /* (and its mix) */
    pat_queue = n + 1u;
    RING_PUBLISH();                             /* (all of it before the flag: the ISR may be on another core) */
    ctl_pat_swap = 1;
    msg[0] = 0; scat(msg, "PATTERN "); fmt_u(msg + slen(msg), n + 1u); scat(msg, " NEXT");
    say(msg);
}
static void pat_copy(uint32_t n)                /* this pattern into slot n (a used one: turn again) */
{
    static char msg[24];
    n %= NPAT;
    if (n == set.pat) { say("THAT IS THIS ONE"); return; }
    if (pat_used >> n & 1u && (copy_arm != n + 1u || o3_ms - copy_arm_t > 3000u)) {
        copy_arm = n + 1u; copy_arm_t = o3_ms;
        msg[0] = 0; scat(msg, "AGAIN: OVERWRITE "); fmt_u(msg + slen(msg), n + 1u); say(msg);
        return;
    }
    copy_arm = 0;
    pats[n] = pat; pat_used |= 1u << n; pats_dirty |= 1u << n; store_t = o3_ms;
    snd_capture(n);                             /* (with this pattern's instruments) */
    msg[0] = 0; scat(msg, "COPIED TO "); fmt_u(msg + slen(msg), n + 1u); say(msg);
}
static void seq_clear_cursor(void)              /* EDIT in STEPS: the cursor's step empty, the cursor on */
{
    uint32_t t = sel, len = pat.trk[t].len;
    if (!len) return;
    undo_save(); step_clear(seq_step(t, seq_cur[t] % len));
    seq_cursor_set(t, (seq_cur[t] + 1u) % len);
    edited(); say("STEP CLEARED");
}
static void seq_undo(void)                      /* SHIFT + F6: one level, a swap (again: redo) */
{
    if (!undo_ok) { say("NOTHING TO UNDO"); return; }
    pattern_t tmp = pat; pat = undo_pat; undo_pat = tmp; edited(); say("UNDO");
}

/* ---- the tracks: start / stop on the bar, record, choose, clear (2.0's) */
static uint32_t trk_state(uint32_t t)           /* 0 stopped, 1 playing, 2 starts on the bar, 3 stops on the bar */
{
    uint32_t on = ctl_trk_on >> t & 1u, pend = trk_pending() >> t & 1u;
    return pend ? (on ? 3u : 2u) : on;
}
static void trk_select(uint32_t t)
{
    t %= NTRK;
    if (t != sel) { sel = t; step_held = -1; audition_off(); entry_reset(); }
    say_trk(pat.trk[t].len ? "" : in_seq() && seq_view == SV_TRACKS ? " OFF: KNOB 3 = ON" : " OFF: KNOB 1 = ON", t);
}
static void trk_launch(uint32_t t)              /* start / stop on the next bar (stopped: it starts, and the transport) */
{
    uint32_t will;
    t %= NTRK;
    if (!ctl_seq_run) {
        if (!((ctl_trk_on ^ trk_pending()) >> t & 1u)) ctl_trk_req ^= 1u << t;
        ctl_seq_restart = 1; ctl_seq_run = 1;
        say_trk(" PLAYS", t);
        return;
    }
    ctl_trk_req ^= 1u << t;
    will = (ctl_trk_on ^ trk_pending()) >> t & 1u;
    say_trk(trk_pending() >> t & 1u ? (will ? " STARTS ON THE BAR" : " STOPS ON THE BAR") : (will ? " PLAYS" : " STOPPED"), t);
}
static void trk_record(uint32_t t)              /* REC lit + a pad of row 1: that track records (session record) */
{
    uint32_t will;
    t %= NTRK;
    if (rec == REC_ON && sel == t) {
        say_trk(rec_wait ? " NOT RECORDED" : " RECORDED", t);
        rec = REC_READY; rec_wait = 0; memset(rec_open, 0, sizeof rec_open);
        return;
    }
    memset(rec_open, 0, sizeof rec_open);
    rec_wait = 0;
    if (!pat.trk[t].len) { pat.trk[t].len = 16; edited(); }
    trk_select(t);
    will = (ctl_trk_on ^ trk_pending()) >> t & 1u;
    if (!will) ctl_trk_req ^= 1u << t;
    if (!ctl_seq_run) {
        rec_begin(); ctl_count = set.click % NCLICK == CLICK_COUNT;
        ctl_seq_restart = 1; ctl_seq_run = 1;
        say_trk(" RECORDING", t);
    } else if (trk_pending() >> t & 1u) { rec_begin(); rec_wait = t + 1u; say_trk(" RECORDS ON THE BAR", t); }
    else { rec_begin(); say_trk(" RECORDING", t); }
}
static void trk_clear(void)                     /* the chosen track emptied (REC held 1.2 s, or SHIFT + REC); one undo */
{
    undo_save();
    for (uint32_t n = 0; n < pat.trk[sel].len; n++) step_clear(seq_step(sel, n));
    for (uint32_t l = 0; l < NLANE; l++) lane_clear(&pat.lane[sel % NTRK][l]);   /* (its recorded knobs too) */
    edited();
    if (rec == REC_ON) { rec = ctl_seq_run ? REC_READY : REC_OFF; rec_wait = 0; }
    memset(rec_open, 0, sizeof rec_open);
    say_trk(" CLEARED", sel);
}
static void perf_pad(uint32_t pad, int shift_held)   /* row 1: start / stop (REC lit: record; SHIFT: always start/stop), row 2: the track */
{
    pad &= 7u;
    if (pad < 4u) { if (rec && !shift_held) trk_record(pad); else trk_launch(pad); }
    else trk_select(pad - 4u);
}

/* ---- the transport: PLAY, STOP, REC (2.0's) */
static uint32_t rec_down, rec_down_t;
static void play_press(void)
{
    if (ctl_seq_run) { ctl_seq_run = 0; rec = REC_OFF; rec_wait = 0; }
    else {
        if (rec == REC_READY) { rec_begin(); ctl_count = set.click % NCLICK == CLICK_COUNT; }
        ctl_seq_restart = 1; ctl_seq_run = 1;
    }
    if (set.midi_out) vibe_transport_out(ctl_seq_run);   /* (3.1) START / STOP out, the clock lined up */
}
static void rec_press(int down, int shift_held)
{
    if (down && shift_held) { trk_clear(); return; }
    if (down) { rec_down = 1; rec_down_t = o3_ms; return; }
    if (!rec_down) return;
    rec_down = 0;
    if (o3_ms - rec_down_t < 500u) {            /* a press: playing, record now / stop recording; stopped, arm */
        if (rec) rec = REC_OFF;
        else if (ctl_seq_run) { rec_begin(); }
        else rec = REC_READY;
        memset(rec_open, 0, sizeof rec_open);
    }
}

/* ---- the views' knobs (2.0's seq_view_knob / seq_view_turn) */
static void fmt_note(char *s, uint32_t n)
{
    static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    s[0] = 0; scat(s, NN[n % 12u]); if (n < 12u) scat(s, "-");
    fmt_u(s + slen(s), n < 12u ? 1u : n / 12u - 1u);
}
static void seq_knobs(knob_t *k)
{
    static const char *const L[NSV][4] = {{"CUTOFF", "LEVEL", "LENGTH", "QUANT"}, {"STEP", "NOTE", "GATE", "VELO"},
                                          {"STEP", "RATCHET", "CHANCE", "ON/OFF"}, {"LENGTH", "DIV", "SWING", "TRANSP"},
                                          {"SLOT", "", "GO", "COPY"}};
    uint32_t t = sel, len = pat.trk[t].len, n;
    seqtrk_t *tk = &pat.trk[t];
    const step_t *st = seq_step(t, len ? seq_cur[t] % len : 0u);
    for (uint32_t i = 0; i < 4u; i++) {
        char *v = k[i].value;
        v[0] = 0; k[i].v = 0; k[i].bip = 0;
        k[i].label = L[SV_EFF][i][0] ? L[SV_EFF][i] : 0;
        if (!k[i].label) continue;
        if (seq_view == SV_SLOTS) {
            if (i == 0) { fmt_u(v, slot_sel + 1u); k[i].v = slot_sel / (float)(NPAT - 1); }
            else if (i == 2u) scat(v, slot_sel == set.pat ? "HERE" : "TURN");
            else scat(v, copy_arm == slot_sel + 1u && o3_ms - copy_arm_t <= 3000u ? "AGAIN" : "TURN");
            continue;
        }
        if (seq_view == SV_TRACKS) {                /* (CUTOFF on knob 1, LENGTH on 3: Sven's order) */
            uint32_t j = i == 0u ? 2u : i == 2u ? 0u : i;
            if (j == 0) { if (tk->len) fmt_u(v, tk->len); else scat(v, "OFF"); k[i].v = tk->len / (float)SEQ_STEPS; }
            else if (j == 1) { fmt_u(v, set.mix_lvl[t]); k[i].v = set.mix_lvl[t] / 127.0f; }
            else if (j == 2) {                  /* (without "Hz": the column is narrow, Sven 21.10.) */
                param_value(v, P_CUTOFF, live_p[t].v[P_CUTOFF]); k[i].v = live_p[t].v[P_CUTOFF] / 127.0f;
                { uint32_t l = slen(v); if (l > 2u && v[l - 2u] == 'H' && v[l - 1u] == 'z') v[l - 2u] = 0; }
            }
            else { scat(v, RQ_NAME[set.rec_qt[t] % NRQ]); k[i].v = (float)(set.rec_qt[t] % NRQ) / (NRQ - 1); }
            continue;
        }
        if (seq_view == SV_TRACK) {
            if (i == 0) { if (tk->len) fmt_u(v, tk->len); else scat(v, "OFF"); k[i].v = tk->len / (float)SEQ_STEPS; }
            else if (i == 1) { scat(v, DIV_NAME[tk->div % NDIV]); k[i].v = tk->div / (float)(NDIV - 1); }
            else if (i == 2) { fmt_u(v, tk->swing); scat(v, "%"); k[i].v = (tk->swing - 50) / 25.0f; }
            else { if (tk->transp) scat(v, tk->transp < 0 ? "-" : "+"); fmt_u(v + slen(v), (uint32_t)(tk->transp < 0 ? -tk->transp : tk->transp)); k[i].v = (tk->transp + 24) / 48.0f; k[i].bip = 1; }
            continue;
        }
        if (!len) { if (i == 0) { k[i].label = "LENGTH"; scat(v, "OFF"); } else k[i].label = 0; continue; }
        if (i == 0) { fmt_u(v, seq_cur[t] % len + 1u); scat(v, "/"); fmt_u(v + slen(v), len); k[i].v = (seq_cur[t] % len) / (float)(len > 1 ? len - 1 : 1); continue; }
        if (!st->note[0]) { scat(v, "--"); continue; }
        if (SV_EFF == SV_STEPS) {
            if (i == 1) { for (n = 0; n < SEQ_CHORD && st->note[n]; n++) continue; fmt_note(v, st->note[0]); if (n > 1u) { scat(v, "+"); fmt_u(v + slen(v), n - 1u); } k[i].v = st->note[0] / 127.0f; }
            else if (i == 2) { if (st->gate >= GATE_TIE) scat(v, "TIE"); else { fmt_u(v, (st->gate * 100u + 63u) / 127u); scat(v, "%"); } k[i].v = st->gate / 127.0f; }
            else { fmt_u(v, st->vel); k[i].v = st->vel / 127.0f; }
        } else {
            if (i == 1) { fmt_u(v, (st->ratch & 3u) + 1u); scat(v, "x"); k[i].v = (st->ratch & 3u) / 3.0f; }
            else if (i == 2) { fmt_u(v, st->chance); scat(v, "%"); k[i].v = st->chance / 100.0f; }
            else { scat(v, st->ratch & STEP_OFF ? "OFF" : "ON"); k[i].v = st->ratch & STEP_OFF ? 0.0f : 1.0f; }
        }
    }
}
static void seq_turn(uint32_t knob, int32_t e)
{
    uint32_t t = sel, len = pat.trk[t].len, k;
    seqtrk_t *tk = &pat.trk[t];
    int32_t sg = e > 0 ? 1 : -1;
    step_t *st;
    if (seq_view == SV_SLOTS) {
        if (knob == 0) slot_sel = (uint32_t)clampi((int32_t)slot_sel + sg, 0, NPAT - 1);
        else if (knob == 2u && e > 0) pat_go(slot_sel);
        else if (knob == 3u && e > 0) pat_copy(slot_sel);
        return;
    }
    if (seq_view == SV_TRACKS && (knob == 0 || knob == 2u)) knob ^= 2u;   /* TRACKS: CUTOFF on knob 1, LENGTH on 3 */
    if (seq_view == SV_TRACKS && knob > 0) {    /* the track's mixer LEVEL, its sound's CUTOFF; REC's QUANTISE */
        if (knob == 1) { set.mix_lvl[t] = (uint8_t)clampi((int32_t)set.mix_lvl[t] + e * 2, 0, 127); mix_apply(); }
        else if (knob == 2) { patch_t *d = &live_p[t]; d->v[P_CUTOFF] = (uint8_t)clampi(d->v[P_CUTOFF] + e * 2, 0, 127); trk_edited[t] = 1; }
        else set.rec_qt[t] = (uint8_t)clampi((int32_t)(set.rec_qt[t] % NRQ) + sg, 0, NRQ - 1);
        set_touch();
        return;
    }
    if (seq_view == SV_TRACK || seq_view == SV_TRACKS || (!len && knob == 0)) {   /* LENGTH (0 = off), DIV, SWING, TRANSPOSE */
        if (knob == 0) {
            int32_t l = (int32_t)tk->len;
            if (!l && e > 0) l = 16;
            else if (e > 1 || e < -1) l = e > 0 ? (l / 16 + 1) * 16 : (l - 1) / 16 * 16;
            else l += e;
            tk->len = (uint16_t)clampi(l, 0, (int32_t)SEQ_STEPS);
            if (tk->len && seq_cur[t] >= tk->len) seq_cursor_set(t, tk->len - 1u);
        } else if (knob == 1) tk->div = (uint8_t)clampi((int32_t)tk->div + sg, 0, NDIV - 1);
        else if (knob == 2) tk->swing = (uint8_t)clampi((int32_t)tk->swing + e, 50, 75);
        else tk->transp = (int8_t)clampi(tk->transp + sg, -24, 24);
        edited();
        return;
    }
    if (!len) return;
    if (knob == 0) {                            /* the cursor; keys held: the note goes on into the next step */
        if (entry_n && e > 0) {
            uint32_t n = seq_cur[t] % len;
            if (n + 1u >= len) return;
            st = seq_step(t, n); st->gate = GATE_TIE;
            *seq_step(t, n + 1u) = *st; seq_step(t, n + 1u)->gate = 110; seq_step(t, n + 1u)->ratch = 0;
            seq_cursor_set(t, n + 1u); edited();
            return;
        }
        seq_cursor_set(t, (uint32_t)(((int32_t)(seq_cur[t] % len) + e % (int32_t)len + (int32_t)len) % (int32_t)len));
        return;
    }
    st = seq_step(t, seq_cur[t] % len);
    if (SV_EFF == SV_STEPS && knob == 1) {      /* NOTE: the chord moves; an empty step gets the last notes played */
        if (!st->note[0]) { step_clear(st); for (k = 0; k < (pen_n ? pen_n : 1u) && k < SEQ_CHORD; k++) st->note[k] = pen_n ? pen_chord[k] : (uint8_t)pen; }
        else for (k = 0; k < SEQ_CHORD && st->note[k]; k++) st->note[k] = (uint8_t)clampi(st->note[k] + sg, 1, 127);
        st->ratch &= (uint8_t)~STEP_OFF;
    } else if (!st->note[0]) { say("EMPTY STEP: PLAY A NOTE"); return; }
    else if (SV_EFF == SV_STEPS && knob == 2) st->gate = (uint8_t)clampi((int32_t)st->gate + e * 2, 1, GATE_TIE);
    else if (SV_EFF == SV_STEPS) st->vel = (uint8_t)clampi((int32_t)st->vel + e * 2, 1, 127);
    else if (knob == 1) st->ratch = (uint8_t)((st->ratch & STEP_OFF) | (uint32_t)clampi((int32_t)(st->ratch & 3u) + sg, 0, 3));
    else if (knob == 2) st->chance = (uint8_t)clampi((int32_t)st->chance + sg * 5, 0, 100);
    else st->ratch = (uint8_t)(e > 0 ? st->ratch & ~STEP_OFF : st->ratch | STEP_OFF);
    edited();
}

/* ---- now and then (from the draw): the arp's notes into REC, the swap on the bar, REC waiting, saving */
/* the arp's notes into REC, with the key's velocity. Each carries the place the ISR played it at (arpev_t.q8), so it
 * lands exactly there however late it is taken (from events and the draw – no UI work in the audio flow, 6.2) */
static void arp_rec_drain(void)
{
    take_check();
    while (arp_qr != arp_qw) {
        RING_TAKE();
        arpev_t e = arp_q[arp_qr % AQ];
        arp_qr++;
        if (e.part == sel && live_p[sel].v[P_ARP_MODE]) {
            rec_from_arp = 1; rec_arp_q8 = e.q8;
            rec_note(e.on, e.note, e.vel ? e.vel : 100u);
            rec_from_arp = 0; rec_arp_q8 = 0xFFFFFFFFu;
        }
    }
}
/* the ISR swapped the next pattern in (its sounds and its mix already, on the downbeat): the panel follows */
static void seq_swap_check(void)
{
    if (ui_pat_swapped == swap_seen) return;
    swap_seen = ui_pat_swapped;
    RING_TAKE();
    if (ctl_pat_keep) pats[set.pat] = pat_old;  /* (the one left, with its edits) */
    {   /* the one left keeps its instruments as they sounded to the end, and its mixer */
        psnd_t *s = &psnd[set.pat]; s->magic = PSN2_MAGIC;
        for (uint32_t t = 0; t < NTRK; t++) { s->p[t] = snd_old[t]; s->prog[t] = (uint8_t)trk_prog[t]; s->edited[t] = (uint8_t)trk_edited[t]; }
        mx_from_set(&s->mx);
        snd_dirty |= 1u << set.pat;
    }
    set.pat = (uint8_t)(pat_queue ? pat_queue - 1u : set.pat);
    snd_meta(set.pat);
    pat_queue = 0;
    memset(seq_cur, 0, sizeof seq_cur); memset(seq_pg, 0, sizeof seq_pg);
    pat_edited = 0; slot_sel = set.pat; set_touch();
    { static char msg[16]; msg[0] = 0; scat(msg, "PATTERN "); fmt_u(msg + slen(msg), set.pat + 1u); say(msg); }
}
static void seq_poll(void)
{
    static const char *const PB[NPAT] = {"blob0", "blob1", "blob2", "blob3", "blob4", "blob5", "blob6", "blob7"};
    arp_rec_drain();                            /* (the arp's notes into REC – each carries where it played) */
    seq_swap_check();                           /* (a pattern swapped in on the downbeat: the panel follows) */
    if (pat_requeue && ui_pat_swapped == swap_seen && ctl_pat_swap == 0u) { uint32_t n = pat_requeue - 1u; pat_requeue = 0; pat_go(n); }
    if (pat_queue && !ctl_seq_run) { uint32_t n = pat_queue - 1u; ctl_pat_swap = 0; pat_queue = 0; pat_switch(n); }
    if (rec_wait) {                             /* REC waits for a track's bar: there it is */
        uint32_t w = rec_wait - 1u;
        if (rec != REC_ON || sel != w) rec_wait = 0;
        else if (ctl_trk_on >> w & 1u && !(trk_pending() >> w & 1u)) { rec_wait = 0; memset(rec_open, 0, sizeof rec_open); }
    }
    if (rec_down && o3_ms - rec_down_t > 1200u) { trk_clear(); rec_down = 0; }   /* REC held: the track cleared */
    ctl_click = set.click % NCLICK == CLICK_ON || (rec == REC_ON && set.click % NCLICK != CLICK_OFF);
    if (!pat_queue && o3_ms - store_t > 1500u) {   /* the sounds now changed (a program, a knob): this pattern's set follows */
        const psnd_t *s = &psnd[set.pat];
        mixset_t now;
        int same = s->magic == PSN2_MAGIC;
        mx_from_set(&now);
        for (uint32_t t = 0; t < NTRK && same; t++) same = s->prog[t] == trk_prog[t] && s->edited[t] == trk_edited[t] && bytes_same(&s->p[t], &live_p[t], sizeof(patch_t));
        if (same) same = bytes_same(&s->mx, &now, sizeof now);
        if (!same) snd_capture(set.pat);
    }
    if (snd_dirty && o3_ms - store_t > 1500u) {
        for (uint32_t n = 0; n < NPAT; n++) if (snd_dirty >> n & 1u) st_save(PS_FILE[n], &psnd[n], sizeof psnd[n]);
        snd_dirty = 0;
    }
    if (pats_dirty && o3_ms - store_t > 1500u && step_held < 0 && !entry_n) {   /* saved by themselves, also while playing */
        pats[set.pat] = pat;
        for (uint32_t n = 0; n < NPAT; n++)
            if (pats_dirty >> n & 1u) { pats[n].magic = PAT_MAGIC; if (!st_save(PB[n], &pats[n], sizeof pats[n])) pat_used |= 1u << n; }
        pats_dirty = 0; pat_edited = 0;
    }
}
static void seq_boot(void)                      /* the 8 patterns as they were, else empty */
{
    static const char *const PB[NPAT] = {"blob0", "blob1", "blob2", "blob3", "blob4", "blob5", "blob6", "blob7"};
    for (uint32_t n = 0; n < NPAT; n++)
        if (st_load(PB[n], &pats[n], sizeof pats[n]) || pats[n].magic != PAT_MAGIC) pat_factory(n, &pats[n]);
        else if (pats[n].auto_magic != AUTO_MAGIC) auto_init(&pats[n]);   /* (saved before the knob lanes) */
        else pat_used |= 1u << n;
    for (uint32_t n = 0; n < NPAT; n++)
        if (st_load(PS_FILE[n], &psnd[n], sizeof psnd[n])) psnd[n].magic = 0;
        else if (psnd[n].magic == PSND_MAGIC) { mx_from_set(&psnd[n].mx); psnd[n].magic = PSN2_MAGIC; snd_dirty |= 1u << n; }   /* (a first set: the mixer now joins it) */
        else if (psnd[n].magic != PSN2_MAGIC) psnd[n].magic = 0;
    if (set.pat >= NPAT) set.pat = 0;
    pat = pats[set.pat];
    slot_sel = set.pat;
    if (psnd[set.pat].magic == PSN2_MAGIC) {    /* the pattern's own instruments (else the settings' sounds stay, and become its) */
        patch_t nx[NTRK]; snd_target(set.pat, nx);
        for (uint32_t t = 0; t < NTRK; t++) live_p[t] = nx[t];
    }
    snd_meta(set.pat);
}

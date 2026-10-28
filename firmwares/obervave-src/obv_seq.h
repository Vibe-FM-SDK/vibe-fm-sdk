/* SPDX-License-Identifier: GPL-3.0-only */
/* The step sequencer (VIBE edition): four tracks A-D, each with its own 256 steps, length, note value, swing and
 * transpose, each playing its own sound. A step holds a chord of up to four notes, a velocity, a gate (or a tie into
 * the next step), a ratchet (1-4 hits), a chance and a nudge (how late in the step it sounds, for unquantised
 * recording). The tracks start and stop on the bar (as clips in Ableton Live): a request waits for the next downbeat;
 * the next pattern comes in on a downbeat too.
 * The player runs in the audio ISR per control block, as the arpeggiator does; the main loop edits `pat`. */
#define NTRK 4u
#define SEQ_STEPS 256u
#define SEQ_CHORD 4u
#define GATE_TIE 127u                           /* a gate of 127: the notes hold into the next step */
#define NPAT NBLOB                              /* patterns, one blob each */
#define PAT_MAGIC 0x34514553u                   /* "SEQ4" */
#define BAR_BEATS 4u

typedef struct { uint8_t note[SEQ_CHORD], vel, gate, ratch, chance, nudge; } step_t;   /* note 0 = none; ratch 0..3 = 1..4 hits;
                                                                                         nudge 0..127: that much of the step later */
#define STEP_OFF 0x80u                          /* in ratch: the step is switched off, its notes kept */
typedef struct { uint16_t len; uint8_t div, swing; int8_t transp; uint8_t mute, pad[2]; } seqtrk_t;   /* swing 50..75 % */
/* (OberVave 3) the knobs recorded: per track up to NLANE lanes, each one parameter of the track's sound with a value per
 * step (AUTO_NONE: that step leaves it as it is). REC running + a knob turned = the value written on the step playing;
 * the player sets it on its step. After the steps, behind auto_magic: a pattern saved before has none (cleared at load) */
#define NLANE 4u
#define AUTO_NONE 0xFFu
#define AUTO_MAGIC 0x4F545541u                  /* "AUTO" */
typedef struct { uint8_t param, pad[3]; uint8_t v[SEQ_STEPS]; } lane_t;
typedef struct { uint32_t magic; seqtrk_t trk[NTRK]; step_t st[NTRK][SEQ_STEPS]; uint32_t auto_magic; lane_t lane[NTRK][NLANE]; } pattern_t;

static pattern_t pat;                           /* the pattern now: the main loop edits, the ISR plays */
static step_t *seq_step(uint32_t t, uint32_t i) { return &pat.st[t % NTRK][i % SEQ_STEPS]; }

/* main loop -> ISR */
static volatile uint32_t ctl_seq_run, ctl_seq_restart;
/* the tracks' start / stop: the main loop flips a bit of ctl_trk_req, the ISR takes it on the next downbeat (stopped:
 * at once) and flips the same bit of trk_req_seen; ctl_trk_on is the ISR's: which tracks play */
static volatile uint32_t ctl_trk_req, ctl_trk_on = (1u << NTRK) - 1u;
static uint32_t trk_req_seen;
static uint32_t trk_pending(void) { return (ctl_trk_req ^ trk_req_seen) & ((1u << NTRK) - 1u); }
/* the next pattern: the main loop reads it into pat_q and sets ctl_pat_swap; the ISR swaps it in on the next downbeat
 * (the one it leaves goes to pat_old, so its edits can be saved at the stop) */
static pattern_t pat_q, pat_old;
static volatile uint32_t ctl_pat_swap, ctl_pat_keep, ui_pat_swapped;   /* ctl_pat_swap: 1 waiting, 2 the ISR swaps, 0 */   /* keep: the one left goes to pat_old */
/* (OberVave 3) every pattern has its own four sounds: they change with it on the same downbeat (snd_q in, the ones
 * left to snd_old), so the new pattern's first note already plays its own instrument */
static patch_t snd_q[NTRK], snd_old[NTRK];
static volatile uint32_t ctl_snd_swap;
/* ISR -> main loop: where each track is (step, and how far into it, Q8) */
static volatile uint32_t ui_seq_pos[NTRK], ui_seq_frac[NTRK], ui_seq_dur[NTRK], ui_seq_ticks, ui_seq_beat, ui_seq_bar_beat;

/* ---- a fresh pattern */
static void step_clear(step_t *s)
{
    uint32_t i;
    for (i = 0; i < SEQ_CHORD; i++) s->note[i] = 0;
    s->vel = 100;
    s->gate = 96;                               /* 76 % */
    s->ratch = 0;
    s->chance = 100;
    s->nudge = 0;
}
static void lane_clear(lane_t *l) { uint32_t i; l->param = AUTO_NONE; for (i = 0; i < SEQ_STEPS; i++) l->v[i] = AUTO_NONE; }
static void auto_init(pattern_t *p)
{
    uint32_t t, l;
    p->auto_magic = AUTO_MAGIC;
    for (t = 0; t < NTRK; t++) for (l = 0; l < NLANE; l++) lane_clear(&p->lane[t][l]);
}
static void pat_init(pattern_t *p)
{
    uint32_t i, t;
    p->magic = PAT_MAGIC;
    auto_init(p);
    for (t = 0; t < NTRK; t++) {
        for (i = 0; i < SEQ_STEPS; i++) step_clear(&p->st[t][i]);
        p->trk[t].len = 16;                     /* a bar of sixteenths each */
        p->trk[t].div = D_16;
        p->trk[t].swing = 50;
        p->trk[t].transp = 0;
        p->trk[t].mute = 0;
        p->trk[t].pad[0] = p->trk[t].pad[1] = 0;
    }
}
static void pat_factory(uint32_t n, pattern_t *p)   /* a pattern never saved: empty, four bars of sixteenths */
{
    (void)n;
    pat_init(p);
}

/* ---- the player (ISR, per CTL block) */
#define NO_WAIT 0xFFFFFFFFu
typedef struct { uint32_t cnt, dur, step, sub, rat_n, rat_i, gate_at, nsnd, play, tie, wait, on_at, hold_end, frac; uint8_t snd[SEQ_CHORD]; } seqrun_t;   /* hold_end: a late note's end in the next step */
static seqrun_t sr[NTRK];
static uint32_t seq_was, seq_beat_cnt, seq_beat_n, count_left;

/* a note held on the keys while REC takes it (main loop -> ISR): the key owns it, the sequencer neither plays nor ends
 * it on that track (else the step just recorded, or the loop coming round, cut the long note being played) */
static volatile uint32_t ctl_live_hold[NTRK][4];
static int seq_live(uint32_t t, uint32_t n) { return ctl_live_hold[t % NTRK][n >> 5 & 3u] >> (n & 31u) & 1u; }
/* an arp take (REC, the arp on the track playing into it): the track's steps are not played meanwhile – you hear the arp
 * that is being written, not the last round of it on top */
static volatile uint32_t ctl_rec_mute;

static void seq_release_t(uint32_t t)
{
    seqrun_t *r = &sr[t];
    uint32_t i;
    for (i = 0; i < r->nsnd; i++) if (!seq_live(t, r->snd[i])) voice_release(t, r->snd[i], 1);
    r->nsnd = 0;
}
static uint32_t seq_note(const step_t *s, uint32_t i, int32_t transp)
{
    return (uint32_t)clampi((int32_t)s->note[i] + transp, 1, 127);
}
static void seq_sound(uint32_t t, const step_t *s, int32_t transp)
{
    seqrun_t *r = &sr[t];
    uint32_t i;
    for (i = 0; i < SEQ_CHORD && s->note[i] && r->nsnd < SEQ_CHORD; i++) {
        uint32_t n = seq_note(s, i, transp);
        if (seq_live(t, n) || ctl_rec_mute >> t & 1u) continue;
        TRACE('S', t, n);
        voice_trigger(t, n, s->vel ? s->vel : 100u);
        r->snd[r->nsnd++] = (uint8_t)n;
    }
}
static uint32_t seq_has(const step_t *s, uint32_t m, int32_t transp)   /* the step plays note m */
{
    uint32_t i;
    for (i = 0; i < SEQ_CHORD && s->note[i]; i++)
        if (seq_note(s, i, transp) == m) return 1;
    return 0;
}

/* a tie, note by note: what the new step plays again keeps sounding, the rest lets go, the new ones start */
static void seq_tie_into(uint32_t t, const step_t *s, int32_t transp, uint32_t play)
{
    seqrun_t *r = &sr[t];
    uint32_t i, j, keep = 0;
    for (i = j = 0; i < r->nsnd; i++) {
        if (play && seq_has(s, r->snd[i], transp)) { r->snd[j++] = r->snd[i]; keep++; }
        else if (!seq_live(t, r->snd[i])) voice_release(t, r->snd[i], 1);
    }
    r->nsnd = j;
    if (!play) return;
    for (i = 0; i < SEQ_CHORD && s->note[i] && r->nsnd < SEQ_CHORD; i++) {
        uint32_t m = seq_note(s, i, transp);
        for (j = 0; j < keep && r->snd[j] != m; j++) ;
        if (j < keep || seq_live(t, m) || ctl_rec_mute >> t & 1u) continue;
        voice_trigger(t, m, s->vel ? s->vel : 100u);
        r->snd[r->nsnd++] = (uint8_t)m;
    }
}

static void seq_from_top(uint32_t t)            /* track t: step 1 in this block */
{
    seqrun_t *r = &sr[t];
    seq_release_t(t);
    r->step = 0xFFFFFFFFu;
    r->cnt = r->dur = r->frac = 0;
    r->play = r->tie = 0;
    r->wait = NO_WAIT;
    r->hold_end = 0;
}

static void seq_take_requests(void)             /* the start / stop requests (on a downbeat, or at once when stopped) */
{
    uint32_t p = trk_pending(), t;
    if (!p) return;
    for (t = 0; t < NTRK; t++)
        if (p >> t & 1u) {
            if (ctl_trk_on >> t & 1u) { ctl_trk_on &= ~(1u << t); seq_release_t(t); }
            else { ctl_trk_on |= 1u << t; seq_from_top(t); }
        }
    trk_req_seen ^= p;
}

/* (VIBE, SDK 8) chase: start where the DAW is – every track on the step that belongs to that moment (counted from the
 * song's start, as if it had played from bar 1), with the part of that step already gone; the beat inside the bar too.
 * Set by vibe_on_audio when the DAW starts or jumps (a loop going round, a click in its timeline). */
static volatile uint32_t ctl_seq_chase;
static double seq_chase_beat, seq_chase_bar;    /* quarter notes: where, and where its bar began */
static uint32_t seq_loop_on, seq_loop_at;       /* the DAW's loop end ahead: at engine frame seq_loop_at (audio thread only) */
static double seq_loop_beat, seq_loop_bar;      /* where it goes back to */
#define CHASE_SLACK 512u                        /* frames: a jump seen this late (a DAW's sub-block) – a track already there stays */
static uint32_t near_frames(uint32_t a, uint32_t b, uint32_t m) { uint32_t d = a > b ? a - b : b - a; return d <= CHASE_SLACK || (m && m - d <= CHASE_SLACK); }
static void seq_chase_apply(int running)        /* running: a jump while playing – a track already in place keeps going */
{
    uint32_t t, q = quarter_len();
    double b = seq_chase_beat, in_bar = b - seq_chase_bar;
    for (t = 0; t < NTRK; t++) {
        seqrun_t *r = &sr[t];
        uint32_t len = pat.trk[t].len, dv = DIV_PER_QUARTER[pat.trk[t].div % NDIV], cnt, step;
        double st = b * (double)dv;
        uint64_t n = (uint64_t)st;
        if (!len) continue;
        step = (uint32_t)(n % len);
        cnt = (uint32_t)((st - (double)n) * (double)(q / dv));   /* (how much of the step is gone) */
        if (running && r->dur && r->step != 0xFFFFFFFFu &&
            ((r->step == step && near_frames(r->cnt, cnt, 0)) ||   /* (there already, or just before the step begins) */
             ((r->step + 1u) % len == step && r->dur - r->cnt <= CHASE_SLACK && cnt <= CHASE_SLACK))) continue;
        seq_from_top(t);
        r->step = (step + len - 1u) % len;      /* (the step before: the next tick enters it at once – dur 0) */
        r->cnt = cnt;
    }
    if (in_bar < 0.0) in_bar = 0.0;
    {
        uint32_t whole = (uint32_t)in_bar, cnt = (uint32_t)((in_bar - (double)whole) * (double)q), n_now, c_now;
        if (cnt < CTL) { c_now = 0; n_now = whole; }   /* on a beat: it ticks now */
        else { c_now = cnt; n_now = whole + 1u; }      /* inside one: the next ticks */
        if (!running || n_now % BAR_BEATS != seq_beat_n % BAR_BEATS || !near_frames(seq_beat_cnt, c_now, q)) { seq_beat_cnt = c_now; seq_beat_n = n_now; }
    }
    count_left = 0;
}

static int seq_phase(uint32_t *ph) { *ph = seq_beat_cnt; return ctl_seq_run && seq_was; }
static int seq_starting(void) { return ctl_seq_run && (!seq_was || ctl_seq_restart); }

/* (VIBE) where track t is in this block (the arp asks before the sequencer's turn: a step that begins in this block
 * counts as begun) – Q8: step * 256 + how far into it; 0xFFFFFFFF = not running */
static uint32_t seq_q8_now(uint32_t t)
{
    const seqrun_t *r = &sr[t % NTRK];
    uint32_t len = pat.trk[t % NTRK].len, step = r->step, cnt = r->cnt, dur = r->dur;
    if (!len || !ctl_seq_run) return 0xFFFFFFFFu;
    if (seq_starting() || step == 0xFFFFFFFFu || !dur) return 0;
    if (cnt >= dur) { cnt -= dur; step = step + 1u >= len ? 0u : step + 1u; }
    return (step % len) * 256u + (cnt * 256u / dur > 255u ? 255u : cnt * 256u / dur);
}
/* (VIBE) the tracks' place at the end of each block, with that block's frame – the main loop finds where a key was
 * HEARD (Protokoll 5: vibe_event_heard) from it. A seqlock: odd = being written. */
static struct { volatile uint32_t seq, frame; uint32_t pos[NTRK], cnt[NTRK], dur[NTRK]; } snap;
static void snap_write(void)
{
    uint32_t t;
    snap.seq++; RING_PUBLISH();
    for (t = 0; t < NTRK; t++) { snap.pos[t] = sr[t].step == 0xFFFFFFFFu ? 0u : sr[t].step; snap.cnt[t] = sr[t].cnt; snap.dur[t] = sr[t].dur; }
    snap.frame = eng_t;
    RING_PUBLISH(); snap.seq++;
}
static int snap_q8(uint32_t t, uint32_t frame, uint32_t *q8)   /* track t's Q8 position at that frame (main loop) */
{
    uint32_t a, b, pos, cnt, dur, f, len = pat.trk[t % NTRK].len;
    int32_t back;
    do { a = snap.seq; RING_TAKE(); pos = snap.pos[t % NTRK]; cnt = snap.cnt[t % NTRK]; dur = snap.dur[t % NTRK]; f = snap.frame; RING_TAKE(); b = snap.seq; } while (a != b || (a & 1u));
    back = (int32_t)(f - frame);                /* frames from what was heard to the snapshot */
    if (!len || !dur || back < 0 || back > (int32_t)(FS * 2u)) return 0;
    {
        int64_t q = (int64_t)(pos % len) * 256 + (int64_t)cnt * 256 / dur - (int64_t)back * 256 / dur, L = (int64_t)len * 256;
        q %= L; if (q < 0) q += L;
        *q8 = (uint32_t)q;
    }
    return 1;
}

static void mx_swap_in(void)                    /* (ISR, inside the pattern swap: after its RING_TAKE) */
{
    uint32_t t;
    if (!ctl_mx_swap) return;
    for (t = 0; t < NPART; t++) {
        ctl_mix_lvl[t] = mx_q.lvl[t]; ctl_mix_cho[t] = mx_q.cho[t]; ctl_mix_dly[t] = mx_q.dly[t];
        ctl_mix_rev[t] = mx_q.rev[t]; ctl_mix_drv[t] = mx_q.drv[t]; ctl_mix_pan[t] = mx_q.pan[t];
    }
    ctl_mix_off = mx_q.off;
    ctl_dv_type = mx_q.dv_type; ctl_dv_amt = mx_q.dv_amt; ctl_dv_tone = mx_q.dv_tone; ctl_dv_mix = mx_q.dv_mix;
    ctl_m_vol = mx_q.m_vol; ctl_m_dj = mx_q.m_dj; ctl_m_drive = mx_q.m_drive; ctl_m_comp = mx_q.m_comp;
    ctl_mx_swap = 0;
}

/* the recorded knobs of track t on step i: into its sound (a lane being recorded right now is left to the hand) */
static volatile uint32_t ctl_auto_hold[NTRK];   /* main loop: bit l = lane l of track t is being turned */
static void seq_auto(uint32_t t, uint32_t i)
{
    uint32_t l;
    for (l = 0; l < NLANE; l++) {
        const lane_t *a = &pat.lane[t][l];
        uint8_t v = a->v[i % SEQ_STEPS];
        if (a->param == AUTO_NONE || v == AUTO_NONE || (ctl_auto_hold[t] >> l & 1u) || a->param >= NPARAM) continue;
        live_p[t].v[a->param] = v;
    }
}
static void seq_tick(void)
{
    uint32_t t, q;
    if (!ctl_seq_run) {
        if (seq_was) { for (t = 0; t < NTRK; t++) seq_release_t(t); seq_was = 0; }
        seq_take_requests();                    /* (stopped: a pad switches its track at once) */
        return;
    }
    if (!seq_was | (__atomic_exchange_n(&ctl_seq_restart, 0u, __ATOMIC_ACQ_REL) != 0u)) {   /* start: step 1 of every
                                                 * track in this block (exchange: a restart asked meanwhile is not lost) */
        seq_take_requests();
        for (t = 0; t < NTRK; t++) seq_from_top(t);
        seq_beat_cnt = 0;
        seq_beat_n = 0;
        count_left = ctl_count ? 4u : 0u;
        if (__atomic_exchange_n(&ctl_seq_chase, 0u, __ATOMIC_ACQ_REL)) { ctl_count = 0; seq_chase_apply(0); }   /* (the DAW's place) */
        seq_was = 1;
    }
    else if (seq_loop_on && (int32_t)(eng_t - seq_loop_at) > 0) {   /* the DAW's loop ends in this block: back to its start */
        seq_loop_on = 0; ctl_seq_chase = 0; seq_chase_beat = seq_loop_beat; seq_chase_bar = seq_loop_bar; ctl_count = 0; seq_chase_apply(1);
    }
    else if (__atomic_exchange_n(&ctl_seq_chase, 0u, __ATOMIC_ACQ_REL)) { ctl_count = 0; seq_chase_apply(1); }   /* (the DAW jumped) */
    q = quarter_len();
    if (seq_beat_cnt < CTL) {                   /* a beat (in this block): the PLAY light, the click, the count-in; a downbeat: requests */
        if (ctl_count && !count_left) {         /* counted in: the tracks from the top, now */
            ctl_count = 0;
            seq_beat_n = 0;
            for (t = 0; t < NTRK; t++) seq_from_top(t);
        }
        if (!ctl_count && seq_beat_n % BAR_BEATS == 0u) {   /* the bar's first beat: what waited for it */
            uint32_t one = 1u;
            if (ctl_pat_swap == 1u && __atomic_compare_exchange_n(&ctl_pat_swap, &one, 2u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                /* the next pattern (2 = being swapped in: the main loop leaves pat_q alone): the tracks from the top */
                if (ctl_pat_keep) pat_old = pat;
                pat = pat_q;
                if (ctl_snd_swap) { for (t = 0; t < NTRK; t++) { snd_old[t] = live_p[t]; live_p[t] = snd_q[t]; } ctl_snd_swap = 0; }
                mx_swap_in();                   /* (its mixer, drive and master on the same downbeat) */
                RING_PUBLISH();
                ctl_pat_swap = 0;
                ui_pat_swapped++;
                for (t = 0; t < NTRK; t++) seq_from_top(t);
            }
            seq_take_requests();
        }
        ui_seq_beat++;
        ui_seq_bar_beat = seq_beat_n % BAR_BEATS;
        if (ctl_click || ctl_count) click_start(seq_beat_n % BAR_BEATS == 0u);
        seq_beat_n++;
        if (count_left) count_left--;
        ui_seq_count = ctl_count ? count_left + 1u : 0u;
    }
    seq_beat_cnt += CTL;
    if (seq_beat_cnt >= q)                      /* the next beat: what is over keeps going (a DAW's quarter is any length) */
        seq_beat_cnt = seq_beat_cnt - q < CTL ? seq_beat_cnt - q : 0u;
    if (ctl_count) return;                      /* counting in: no steps yet */
    for (t = 0; t < NTRK; t++) {
        seqrun_t *r = &sr[t];
        const seqtrk_t *k = &pat.trk[t];
        const step_t *s;
        uint32_t len = k->len;
        if (!len || !(ctl_trk_on >> t & 1u)) { seq_release_t(t); r->tie = 0; r->wait = NO_WAIT; r->hold_end = 0; continue; }
        if (r->cnt >= r->dur) {                 /* the next step */
            uint32_t dv = DIV_PER_QUARTER[k->div % NDIV], base = q / dv, sw, carry = 0;
            r->frac += q % dv;                  /* (the rest of q spread over the steps: div steps are exactly a quarter) */
            if (r->frac >= dv) { r->frac -= dv; base++; }
            if (r->hold_end) { r->hold_end = 0; seq_release_t(t); }   /* (a carried note: over by now) */
            if (r->nsnd && r->wait == NO_WAIT && r->gate_at != 0xFFFFFFFFu) {   /* a late note (nudged) lasts into this step */
                uint32_t e = r->on_at + r->rat_i * r->sub + r->gate_at;
                if (e > r->dur) carry = e - r->dur;
            }
            r->cnt -= r->dur;
            r->step = r->step + 1u >= len ? 0u : r->step + 1u;
            sw = base * (uint32_t)(clampi(k->swing, 50, 75) - 50) / 50u;
            r->dur = r->step & 1u ? base - sw : base + sw;   /* swing: the first of each pair longer */
            if (r->dur < CTL) r->dur = CTL;
            s = seq_step(t, r->step);
            seq_auto(t, r->step);
            TRACE('T', t, r->step * 1000000u + (s->note[0] ? (uint32_t)s->note[0] * 1000u : 0u) + (s->note[1] ? 900u : 0u) + (s->gate >= GATE_TIE ? 1u : 0u));
            r->play = s->note[0] && !(s->ratch & STEP_OFF) && (s->chance >= 100u || xorshift(&rnd_state) % 100u < s->chance);
            r->wait = NO_WAIT;
            r->on_at = 0;
            if (r->play && s->nudge) {          /* later in the step: the old notes go (a late one at its end), the new ones when it is time */
                if (!carry) seq_release_t(t);
                r->on_at = (r->dur * s->nudge) >> 7;
                r->wait = r->on_at;
            } else if (r->tie)                  /* a tie: the notes the step plays again hold */
                seq_tie_into(t, s, k->transp, r->play);
            else {
                if (r->play) carry = 0;
                if (!carry) seq_release_t(t);
                if (r->play) seq_sound(t, s, k->transp);
            }
            r->hold_end = carry;
            r->rat_n = (s->ratch & 3u) + 1u;
            r->sub = (r->dur - r->on_at) / r->rat_n;
            r->rat_i = 0;
            r->tie = r->play && s->gate >= GATE_TIE && !s->nudge;
            {                                   /* the length: of a whole step (a ratchet's share), also for a late note */
                uint32_t g = r->dur / r->rat_n;
                r->gate_at = s->gate >= GATE_TIE ? 0xFFFFFFFFu : (uint32_t)clampi((int32_t)((g * (s->gate + 1u)) >> 7), CTL, (int32_t)(g > CTL ? g : CTL));
            }
            ui_seq_ticks++;
        } else {
            s = seq_step(t, r->step < len ? r->step : 0u);
            if (r->wait != NO_WAIT && r->cnt >= r->wait) { r->wait = NO_WAIT; r->hold_end = 0; seq_release_t(t); seq_sound(t, s, k->transp); }   /* a nudged step: now */
            else if (r->wait == NO_WAIT && r->play && r->rat_n > 1u && r->sub) {   /* a ratchet: the next hit */
                uint32_t i = (r->cnt - r->on_at) / r->sub;
                if (i > r->rat_i && i < r->rat_n) { r->rat_i = i; seq_release_t(t); seq_sound(t, s, k->transp); }
            }
        }
        if (r->hold_end && r->cnt >= r->hold_end) { r->hold_end = 0; seq_release_t(t); }   /* a late note from the step before: its end */
        if (r->nsnd && !r->hold_end && r->wait == NO_WAIT && r->gate_at != 0xFFFFFFFFu && r->cnt >= r->on_at && r->cnt - r->on_at - r->rat_i * r->sub >= r->gate_at)
            seq_release_t(t);
        r->cnt += CTL;
        ui_seq_dur[t] = r->dur;
        ui_seq_pos[t] = r->step;
        ui_seq_frac[t] = r->dur ? r->cnt * 256u / r->dur : 0u;
    }
    snap_write();
}

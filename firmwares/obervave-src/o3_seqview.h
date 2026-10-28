/* SPDX-License-Identifier: GPL-3.0-only */
/* OberVave 3 – the sequencer's views (SEQ, again: the next): TRACKS → STEPS → TRACK → PATTERNS. Each shows its part
 * of the pattern above and its four knobs below. */
static void knobs_small(const knob_t *k, int y, uint32_t col)
{
    for (int i = 0; i < 4; i++) {
        int x = CX0 + i * COLW;
        if (!k[i].label) continue;
        dial(x + 24, y + 30, 21, k[i].v, k[i].bip, col);
        vg_text(k[i].label, x + 50, y + 6, VG_MED, DIM, VG_LEFT);
        vg_text(k[i].value, x + 50, y + 26, VG_BIG, FG, VG_LEFT);
    }
}
static void view_tabs(void)
{
    static const uint8_t V[4] = {SV_TRACKS, SV_STEPS, SV_TRACK, SV_SLOTS};
    for (int i = 0; i < 4; i++) {
        int x = CX0 + i * COLW, on = V[i] == seq_view;
        vg_round(x, 41, COLW - 6, 26, 5, on ? KEY_RGB[CK_SEQ] : CELL);
        vg_text(V[i] == SV_STEPS && seq_shift ? "STEP+" : SV_NAME[V[i]], x + (COLW - 6) / 2, 46, VG_MED, on ? 0x111214 : DIM, VG_CENTER);
    }
}
static void state_text(uint32_t t, int x, int y)
{
    static const char *const ST[4] = {"STOP", "PLAY", "NEXT", "ENDS"};
    uint32_t s = trk_state(t);
    vg_text(ST[s & 3u], x, y, VG_MED, s == 1u ? GREEN : s >= 2u ? (blink() ? ACCENT : MUTED) : MUTED, VG_RIGHT);
}
static void tracks_view(void)                   /* A-D one under the other: sound, state, length, the bar playing */
{
    for (uint32_t t = 0; t < NTRK; t++) {
        int y = 72 + (int)t * 32, on = t == sel;
        uint32_t c = TRK_RGB[t], len = pat.trk[t].len, pos = len ? ui_seq_pos[t] % len : 0u, pg = pos / 16u;
        char b[24], n[PATCH_NAME + 1], l[2] = {(char)('A' + t), 0};
        vg_round(CX0, y, CW, 29, 4, on ? vg_mix(BG, c, 0.22f) : CELL);
        vg_round(CX0 + 4, y + 3, 23, 23, 3, c); vg_text(l, CX0 + 15, y + 6, VG_MED, 0x111214, VG_CENTER);
        fmt_u(b, trk_prog[t] + 1u); prog_name(trk_prog[t], n); scat(b, " "); scat(b, n);
        vg_text(b, CX0 + 34, y + 6, VG_MED, on ? FG : DIM, VG_LEFT);
        for (uint32_t s = 0; s < 16u; s++) {    /* the bar playing (stopped: the first) */
            uint32_t i = pg * 16u + s; const step_t *st = seq_step(t, i);
            int x = 178 + (int)s * 12, have = i < len && st->note[0] && !(st->ratch & STEP_OFF);
            uint32_t col = i >= len ? BG : have ? c : CELL2;
            if (ctl_seq_run && ctl_trk_on >> t & 1u && i == pos) col = FG;
            vg_rect(x, y + 7, 10, 15, col);
        }
        if (len) {                              /* the whole track under it: a piece per bar, filled up to where it plays */
            uint32_t bars = (len + 15u) / 16u, run = ctl_seq_run && ctl_trk_on >> t & 1u;
            for (uint32_t k = 0; k < bars; k++) {
                int x0 = 178 + (int)(k * 190u / bars), x1 = 178 + (int)((k + 1u) * 190u / bars) - 2, w = x1 - x0;
                vg_rect(x0, y + 24, w, 2, CELL2);
                if (run && k < pg) vg_rect(x0, y + 24, w, 2, vg_mix(BG, c, 0.6f));
                else if (run && k == pg) vg_rect(x0, y + 24, (int)((pos - k * 16u + 1u) * (uint32_t)w / (len - k * 16u < 16u ? len - k * 16u : 16u)), 2, FG);
            }
            if (run && bars > 1u) { fmt_u(b, pg + 1u); scat(b, "/"); fmt_u(b + slen(b), bars); }   /* playing: bar of bars */
            else fmt_u(b, len);
        } else { b[0] = 0; scat(b, "OFF"); }
        vg_text(b, 404, y + 6, VG_MED, ctl_seq_run && len > 16u ? FG : DIM, VG_RIGHT);
        state_text(t, 466, y + 6);
    }
}
static void steps_view(void)                    /* the cursor's bar: 16 steps, each its velocity as a bar, its pitch a dot */
{
    uint32_t t = sel, len = pat.trk[t].len, pg = seq_pg[t], pos = len ? ui_seq_pos[t] % len : 0u;
    uint32_t c = TRK_RGB[t], lo = 127, hi = 0;
    char b[24];
    if (!len) { vg_text("TRACK OFF – KNOB 1 (LENGTH) SWITCHES IT ON", 240, 120, VG_MED, DIM, VG_CENTER); return; }
    for (uint32_t s = 0; s < 16u; s++) { const step_t *st = seq_step(t, pg * 16u + s); if (pg * 16u + s < len && st->note[0]) { if (st->note[0] < lo) lo = st->note[0]; if (st->note[0] > hi) hi = st->note[0]; } }
    if (hi < lo) { lo = 48; hi = 72; }
    if (hi - lo < 12u) { lo = lo > 6u ? lo - 6u : 0u; hi = lo + 12u; }
    for (uint32_t s = 0; s < 16u; s++) {
        uint32_t i = pg * 16u + s;
        int x = CX0 + (int)s * CW / 16, cur = i == seq_cur[t] % len, held = (int32_t)i == step_held;
        const step_t *st = seq_step(t, i);
        if (i >= len) { vg_rect(x, 72, 26, 106, BG); continue; }
        vg_rect(x, 72, 26, 106, s % 4u == 0u ? 0x182030 : CELL);
        if (st->note[0]) {
            int off = st->ratch & STEP_OFF, h = st->vel * 100 / 127;
            uint32_t col = off ? CELL2 : c;
            vg_rect(x + 3, 174 - h, 20, h, vg_mix(BG, col, off ? 0.5f : 0.45f));
            for (uint32_t q = 0; q < SEQ_CHORD && st->note[q]; q++) {   /* the notes: dots at their pitch */
                int py = 170 - (int)((st->note[q] - lo) * 92u / (hi - lo + 1u));
                vg_rect(x + 3, py, 20, 4, off ? MUTED : FG);
            }
            if (st->gate >= GATE_TIE) vg_rect(x + 23, 122, 6, 4, c);   /* a tie into the next */
            if ((st->ratch & 3u) && !off) for (uint32_t r = 0; r <= (st->ratch & 3u); r++) vg_rect(x + 4 + (int)r * 4, 70, 2, 4, ACCENT);
        }
        if (ctl_seq_run && ctl_trk_on >> t & 1u && i == pos) vg_rect(x, 178, 26, 4, FG);
        if (cur || held) vg_frame(x - 1, 71, 28, 108, 2, held ? ACCENT : FG);
    }
    b[0] = 0; scat(b, "BAR "); fmt_u(b + slen(b), pg + 1u); scat(b, "/"); fmt_u(b + slen(b), seq_pages(t));
    if (ctl_seq_run && ctl_trk_on >> t & 1u && seq_pages(t) > 1u) {   /* where it plays, also when another bar is shown */
        scat(b, " · PLAYS "); fmt_u(b + slen(b), pos / 16u + 1u);
    }
    vg_text(b, 470, 186, VG_MED, ctl_seq_run && pos / 16u != pg ? ACCENT : DIM, VG_RIGHT);
    if (rec) vg_text(rec == REC_ON ? "● REC" : "REC ARMED", CX0, 186, VG_MED, RED, VG_LEFT);
    else vg_text(seq_shift ? "SHIFT: RATCHET · CHANCE · ON/OFF" : "PADS = STEPS · KEYS WRITE", CX0, 186, VG_MED, MUTED, VG_LEFT);
}
static void track_view(void)                    /* the whole track: every step, 16 to a row */
{
    uint32_t t = sel, len = pat.trk[t].len, pos = len ? ui_seq_pos[t] % len : 0u, rows = (len + 15u) / 16u;
    uint32_t c = TRK_RGB[t];
    int ch = rows > 8u ? 6 : rows > 4u ? 10 : 18;
    char b[32];
    if (!len) { vg_text("TRACK OFF – KNOB 1 (LENGTH) SWITCHES IT ON", 240, 120, VG_MED, DIM, VG_CENTER); return; }
    for (uint32_t i = 0; i < len; i++) {
        const step_t *st = seq_step(t, i);
        int x = CX0 + (int)(i % 16u) * CW / 16, y = 72 + (int)(i / 16u) * (ch + 3);
        uint32_t col = st->note[0] && !(st->ratch & STEP_OFF) ? c : i % 4u == 0u ? 0x182030 : CELL;
        if (ctl_seq_run && ctl_trk_on >> t & 1u && i == pos) col = FG;
        vg_rect(x, y, 26, ch, col);
    }
    b[0] = 0; fmt_u(b, len); scat(b, " steps · "); fmt_u(b + slen(b), (len + 15u) / 16u); scat(b, rows == 1u ? " bar" : " bars");
    vg_text(b, CX0, 186, VG_MED, DIM, VG_LEFT);
}
static void slots_view(void)                    /* the 8 patterns: playing, next, the one chosen, used / empty */
{
    for (uint32_t n = 0; n < NPAT; n++) {
        int x = CX0 + (int)(n % 4u) * COLW, y = 72 + (int)(n / 4u) * 56, now = n == set.pat, nx = pat_queue == n + 1u;
        char b[8];
        vg_round(x, y, COLW - 6, 50, 6, now ? vg_mix(BG, KEY_RGB[CK_SEQ], 0.35f) : CELL);
        if (n == slot_sel) vg_frame(x, y, COLW - 6, 50, 2, FG);
        b[0] = 'P'; fmt_u(b + 1, n + 1u);
        vg_text(b, x + 10, y + 3, VG_BIG, now ? FG : DIM, VG_LEFT);
        vg_text(now ? "PLAYING" : nx ? "NEXT" : pat_used >> n & 1u ? "used" : "empty", x + 10, y + 30, VG_MED,
                now ? GREEN : nx ? (blink() ? ACCENT : MUTED) : MUTED, VG_LEFT);
        if (now && ctl_seq_run)                 /* the beats of the bar */
            for (uint32_t q = 0; q < BAR_BEATS; q++)
                vg_rect(x + COLW - 22 - (int)(BAR_BEATS - 1u - q) * 10, y + 8, 7, 7, q == ui_seq_bar_beat ? FG : CELL2);
    }
}
static void seq_view_draw(void)
{
    knob_t k[4];
    view_tabs();
    if (seq_view == SV_TRACKS) tracks_view();
    else if (SV_ENTRY(seq_view)) steps_view();
    else if (seq_view == SV_TRACK) track_view();
    else slots_view();
    seq_knobs(k);
    knobs_small(k, 204, TRK_RGB[sel]);
}
static void seq_leds(void)                      /* STEPS: the steps (set: the track's colour, the playhead white); else rows */
{
    uint32_t t = sel, len = pat.trk[t].len, pos = len ? ui_seq_pos[t] % len : 0u;
    if (SV_ENTRY(seq_view)) {
        for (uint32_t s = 0; s < 16u; s++) {
            uint32_t i = seq_pg[t] * 16u + s; const step_t *st = seq_step(t, i);
            if (i >= len) vibe_led_pad((int)s, 0, 0);
            else if (ctl_seq_run && i == pos) vibe_led_pad((int)s, 0xFFFFFF, 1.0f);
            else vibe_led_pad((int)s, TRK_RGB[t], st->note[0] && !(st->ratch & STEP_OFF) ? 0.8f : 0.06f);
        }
        return;
    }
    for (uint32_t c = 0; c < 4u; c++) {         /* TRACKS etc.: rows 1/3 start / stop (REC: red), rows 2/4 the tracks */
        uint32_t s = trk_state(c);
        float lv = s == 1u ? 1.0f : s >= 2u ? (blink() ? 1.0f : 0.2f) : 0.12f;
        uint32_t col = rec ? RED : GREEN;
        vibe_led_pad((int)c, col, lv); vibe_led_pad((int)(8u + c), col, lv);
        vibe_led_pad((int)(4u + c), TRK_RGB[c], c == sel ? 1.0f : 0.25f); vibe_led_pad((int)(12u + c), TRK_RGB[c], c == sel ? 1.0f : 0.25f);
    }
}

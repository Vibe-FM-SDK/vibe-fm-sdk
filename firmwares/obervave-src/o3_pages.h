/* SPDX-License-Identifier: GPL-3.0-only */
/* OberVave 3 – the pages and what the four encoders do on them. The sound pages are OberVave's own table (PAGE in
 * obv_params.h); each colour key steps through its group as in 2.0:
 *   SOUND OSC 1 → OSC 2 → VOICE → OSC MOD → FILTER → FILTER +   ENV  FILTER ENV → AMP ENV   LFO  LFO → LFO DEST → LFO +
 *   ARP   ARP (on it again: the track's HOLD)          FX   CHORUS → DELAY → REVERB → DRIVE
 *   F6    GLOBAL → OUTPUT                      HOME  PERFORM
 * (3.2: VOICE – mode, unison, glide – is part of the sound, so it sits under SOUND, not under F6) */
static uint32_t page = PG_PERFORM, hold;        /* hold: bit t = track t's HOLD (the arp keeps playing) */
typedef struct { uint8_t first, count, key; } group_t;   /* key: the colour key 0..5 lit for it, 6 = F6, 7 = none */
static const group_t GROUPS[] = {
    {PG_PERFORM, 1, 7}, {PG_OSC1, 6, CK_SOUND}, {PG_FENV, 2, CK_ENV}, {PG_LFO, 3, CK_LFO}, {PG_ARP, 1, CK_ARP}, {PG_CHORUS, 4, CK_FX},
    {PG_GLOBAL, 1, 6}, {PG_OUTPUT, 1, 6},
};
static const group_t *page_group(uint32_t pg)
{
    for (uint32_t i = 0; i < sizeof GROUPS / sizeof GROUPS[0]; i++)
        if (pg >= GROUPS[i].first && pg < (uint32_t)GROUPS[i].first + GROUPS[i].count) return &GROUPS[i];
    return &GROUPS[0];
}
static uint32_t cycle_page(uint32_t first, uint32_t count)   /* a key steps through its pages */
{
    if (page >= first && page < first + count) return first + (page - first + 1u) % count;
    return first;
}
static int key_lit(void) { const group_t *g = page_group(page); return g->key < 6 ? g->key : -1; }
static int global_lit(void) { return page == PG_GLOBAL || page == PG_OUTPUT; }

static const char *const DV_NAME[NDV] = {"OVERDRV", "DIST", "FUZZ", "CRUSH"};
static const char *const CLICK_NAME[NCLICK] = {"OFF", "REC", "ON", "COUNT-IN"};

/* the four knobs of the page shown */
static void page_knobs(knob_t *k)
{
    for (int i = 0; i < 4; i++) {
        k[i].label = 0; k[i].value[0] = 0; k[i].v = 0; k[i].bip = 0;
        if (page == PG_DRIVE) {                 /* the drive send's effect: the settings' */
            static const char *const DL[4] = {"TYPE", "DRIVE", "TONE", "MIX"};
            k[i].label = DL[i];
            if (!i) { scat(k[i].value, DV_NAME[set.dv_type % NDV]); k[i].v = (float)(set.dv_type % NDV) / (NDV - 1); }
            else { uint32_t v = i == 1 ? set.dv_amt : i == 2 ? set.dv_tone : set.dv_mix; fmt_u(k[i].value, v); k[i].v = v / 127.0f; }
            continue;
        }
        if (page == PG_GLOBAL) {
            static const char *const GL[4] = {"TEMPO", "CLICK", "MIDI CH", "TUNE"};
            k[i].label = GL[i];
            if (i == 0) { fmt_u(k[i].value, set.tempo); k[i].v = (set.tempo - 40) / 200.0f; }
            else if (i == 1) { scat(k[i].value, CLICK_NAME[set.click % NCLICK]); k[i].v = (float)(set.click % NCLICK) / (NCLICK - 1); }
            else if (i == 2) { if (set.midi_ch) fmt_u(k[i].value, set.midi_ch); else scat(k[i].value, "OMNI"); k[i].v = set.midi_ch / 16.0f; }
            else { scat(k[i].value, set.tune < 0 ? "-" : "+"); fmt_u(k[i].value + 1, (uint32_t)(set.tune < 0 ? -set.tune : set.tune)); scat(k[i].value, "c"); k[i].v = (set.tune + 50) / 100.0f; k[i].bip = 1; }
            continue;
        }
        int p = PAGE[page].p[i];
        if (p < 0) continue;
        uint32_t v = ed_patch((uint32_t)p)->v[p];
        k[i].label = PDEF[p].name; param_value(k[i].value, (uint32_t)p, v);
        k[i].v = PDEF[p].max ? (float)v / (float)PDEF[p].max : 0;
        k[i].bip = PDEF[p].kind == K_BIP || PDEF[p].kind == K_SEMI || PDEF[p].kind == K_MORPH;
    }
}
static void auto_rec(uint32_t p);              /* (o3_seq.h: REC records the knob) */
static void knob_turn(int i, int e)             /* e: whole detents */
{
    if (page == PG_DRIVE) {
        if (!i) set.dv_type = (uint8_t)clampi((int32_t)(set.dv_type % NDV) + (e > 0 ? 1 : -1), 0, NDV - 1);
        else { uint8_t *v = i == 1 ? &set.dv_amt : i == 2 ? &set.dv_tone : &set.dv_mix; *v = (uint8_t)clampi((int32_t)*v + e * 2, 0, 127); }
        mix_apply(); set_touch(); return;
    }
    if (page == PG_GLOBAL) {
        if (i == 0) set.tempo = (uint8_t)clampi(set.tempo + e, 40, 240);
        else if (i == 1) set.click = (uint8_t)clampi((int32_t)(set.click % NCLICK) + (e > 0 ? 1 : -1), 0, NCLICK - 1);
        else if (i == 2) set.midi_ch = (uint8_t)clampi(set.midi_ch + e, 0, 16);
        else set.tune = (int8_t)clampi(set.tune + e, -50, 50);
        mix_apply(); set_touch(); return;
    }
    int p = PAGE[page].p[i];
    if (p < 0) return;
    set_param((uint32_t)p, (int32_t)ed_patch((uint32_t)p)->v[p] + e * (PDEF[p].max >= 100 ? 2 : 1));
    auto_rec((uint32_t)p);
    if (is_a_only((uint32_t)p)) {               /* the effects: the song's, kept in the settings */
        for (uint32_t j = 0; j < NFXP; j++) set.fx[j] = live_p[0].v[P_CHO_RATE + j];
        set.fx_ok = FX_OK; set_touch();
    }
}

/* SAVE: tapped, it asks "SAVE HERE?" (DATA picks another slot, 3 s to answer); tapped again, the sound is saved there */
static uint32_t save_armed, save_t, save_to, pat_save_armed;   /* (pat_save_armed: F5 in the sequencer asked) */
static void save_tap(void)
{
    if (!save_armed) { save_armed = 1; save_t = o3_ms; save_to = trk_prog[sel]; return; }
    uint32_t n = save_to % NPROG, t = sel % NTRK, u;
    patch_t sp = live_p[t];
    prog_put(n, &sp);
    trk_prog[t] = n; trk_edited[t] = 0; set.tprog[t] = (uint8_t)n; if (!t) set.prog = (uint8_t)n;
    set_touch();
    for (u = 0; u < NTRK; u++)                  /* (another track on the same program, unchanged: it hears it too) */
        if (u != t && trk_prog[u] == n && !trk_edited[u]) { live_p[u] = sp; if (!u) fx_keep(&live_p[0]); }
    save_armed = 0;
    say("SAVED");
}

/* SAVE held + ENTER: a new sound within useful ranges (2.0's dice) */
static uint32_t rnd = 0x2545F491u;
static uint32_t rn(uint32_t lo, uint32_t hi) { return lo + xorshift(&rnd) % (hi - lo + 1u); }
static void randomize(void)
{
    uint32_t i, r;
    patch_t *d = &live_p[sel % NTRK];
    for (i = 0; i < NPARAM; i++) d->v[i] = PDEF[i].def;
    d->v[P_O1_WAVE] = (uint8_t)rn(1, 3); d->v[P_O2_WAVE] = (uint8_t)rn(0, 3);
    d->v[P_O1_PW] = (uint8_t)rn(10, 110); d->v[P_O2_PW] = (uint8_t)rn(10, 110); d->v[P_O2_LVL] = (uint8_t)rn(60, 127);
    r = rn(0, 9); d->v[P_O2_SEMI] = (uint8_t)(r < 4 ? 24 : r < 6 ? 12 : r < 8 ? 36 : r == 8 ? 31 : 43);
    d->v[P_O2_FINE] = (uint8_t)rn(60, 76); d->v[P_NOISE] = (uint8_t)(rn(0, 5) ? 0 : rn(10, 70));
    d->v[P_SYNC] = (uint8_t)(rn(0, 4) == 0); d->v[P_XMOD] = (uint8_t)(rn(0, 4) ? 0 : rn(20, 100));
    d->v[P_ENV_O2] = (uint8_t)(d->v[P_SYNC] ? rn(70, 110) : 64);
    d->v[P_CUTOFF] = (uint8_t)rn(30, 110); d->v[P_RESO] = (uint8_t)rn(0, 90); d->v[P_FENV_AMT] = (uint8_t)rn(60, 120);
    d->v[P_FTYPE] = (uint8_t)(rn(0, 2) ? F_LP24 : rn(0, NFTYPE - 1)); d->v[P_FMORPH] = (uint8_t)rn(0, 127);
    d->v[P_DRIVE] = (uint8_t)(rn(0, 2) ? 0 : rn(10, 70));
    d->v[P_FA] = (uint8_t)rn(0, 90); d->v[P_FD] = (uint8_t)rn(30, 100); d->v[P_FS] = (uint8_t)rn(0, 100); d->v[P_FR] = (uint8_t)rn(20, 100);
    d->v[P_AA] = (uint8_t)rn(0, 80); d->v[P_AD] = (uint8_t)rn(30, 100); d->v[P_AS] = (uint8_t)rn(0, 127); d->v[P_AR] = (uint8_t)rn(20, 100);
    d->v[P_LFO_SHAPE] = (uint8_t)rn(0, 5); d->v[P_LFO_RATE] = (uint8_t)rn(20, 100);
    d->v[P_LFO_PITCH] = (uint8_t)(rn(0, 2) ? 0 : rn(5, 40)); d->v[P_LFO_PWM] = (uint8_t)(rn(0, 1) ? 0 : rn(20, 90));
    d->v[P_LFO_FLT] = (uint8_t)(rn(0, 2) ? 0 : rn(10, 70)); d->v[P_LFO_DELAY] = (uint8_t)rn(0, 70); d->v[P_LFO_PHASE] = (uint8_t)rn(0, 2);
    r = rn(0, 9); d->v[P_MODE] = (uint8_t)(r < 6 ? M_POLY : r < 7 ? M_DUAL : r < 9 ? M_UNISON : M_MONO);
    d->v[P_UNI_N] = (uint8_t)rn(2, 6); d->v[P_UNI_DET] = (uint8_t)rn(10, 80);
    d->v[P_GLIDE] = (uint8_t)(d->v[P_MODE] >= M_UNISON && rn(0, 1) ? rn(5, 40) : 0);
    d->v[P_VINTAGE] = (uint8_t)rn(0, 100); d->v[P_SPREAD] = (uint8_t)rn(20, 127);
    d->v[P_CHO_MIX] = (uint8_t)(rn(0, 2) ? 0 : rn(30, 100)); d->v[P_DLY_TIME] = (uint8_t)rn(0, NDDIV - 1);
    d->v[P_DLY_FB] = (uint8_t)rn(20, 80); d->v[P_DLY_MIX] = (uint8_t)(rn(0, 1) ? 0 : rn(20, 70));
    d->v[P_REV_SIZE] = (uint8_t)rn(40, 127); d->v[P_REV_MIX] = (uint8_t)(rn(0, 3) ? rn(15, 60) : 0);
    trk_edited[sel % NTRK] = 1;
    if (d == &live_p[0]) fx_keep(d);            /* (the dice leave the effects alone) */
}

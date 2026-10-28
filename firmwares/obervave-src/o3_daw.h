/* SPDX-License-Identifier: GPL-3.0-only */
/* The DAW's parameters (SDK 8, vibe_params) – every sound parameter, fixed to its track: an automation lane always moves
 * the same sound, whichever track the panel shows. Both ways: the DAW (or the plugin's Editor) sets a value
 * (VIBE_EV_PARAM), and what the panel, a MIDI CC or a program change does to a sound goes back (daw_sync, once a block).
 * Order (a DAW knows a parameter by its number): first the 32 of 26.10. (8 a track: Cutoff, Resonance, Env Amount,
 * Attack, Release, LFO Rate, LFO > Filter, Volume – projects automated since keep working), then every other voice
 * parameter of tracks A–D, then the effects (chorus, delay, reverb: the patch of track A holds them). Groups for the
 * Editor view: "A · FILTER", "FX · DELAY" … (vibe_param_groups). 27.10.2026. */
#define DAW_PER 8
static const uint8_t DAW_P[DAW_PER] = {P_CUTOFF, P_RESO, P_FENV_AMT, P_AA, P_AR, P_LFO_RATE, P_LFO_FLT, P_VOLUME};
static const char *const DAW_NAME[DAW_PER] = {"Cutoff", "Resonance", "Env Amount", "Attack", "Release", "LFO Rate",
                                              "LFO > Filter", "Volume"};
#define DAW_MAX 240
static vibe_param daw_list[DAW_MAX];
static const char *daw_grp[DAW_MAX];
static uint8_t daw_t[DAW_MAX], daw_pi[DAW_MAX];          /* each one's track and parameter */
static char daw_id[DAW_MAX][24], daw_nm[DAW_MAX][28], daw_gn[NTRK + 1][NPAGE][20];
static int DAW_N;

static int daw_bip(uint32_t p) { return PDEF[p].kind == K_BIP; }            /* (shown -64..63 around the middle) */
static float daw_off(uint32_t p) { return daw_bip(p) ? 64.0f : PDEF[p].kind == K_SEMI ? 24.0f : 0.0f; }
static float daw_of(uint32_t t, uint32_t p) { return (float)live_p[t].v[p] - daw_off(p); }

/* the page a parameter lives on (its group); the knob of CUTOFF is on PERFORM, its home is FILTER */
static int daw_page(uint32_t p)
{
    int g, k;
    if (p == P_CUTOFF) return PG_FILTER;
    if (p == P_MOD_DEPTH) return PG_OUTPUT;
    for (g = 0; g < NPAGE; g++) if (g != PG_PERFORM) for (k = 0; k < 4; k++) if (PAGE[g].p[k] == (int8_t)p) return g;
    return PG_OUTPUT;
}
static const char *const *daw_choices(uint32_t p)
{
    static const char *const ONOFF[2] = {"OFF", "ON"};
    switch (PDEF[p].kind) {
    case K_WAVE: return WAVE_NAME;   case K_FTYPE: return FTYPE_NAME; case K_MODE: return MODE_NAME;
    case K_LSHAPE: return LSHAPE_NAME; case K_LPHASE: return LPHASE_NAME; case K_TO: return TO_NAME;
    case K_ARP: return ARP_NAME;     case K_DIV: return DIV_NAME;     case K_DDIV: return DDIV_NAME;
    case K_ONOFF: return ONOFF;
    default: return 0;
    }
}
static void scopy(char *d, const char *s, int n) { int i = 0; while (s[i] && i < n - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static void daw_add(uint32_t t, uint32_t p, const char *legacy)
{
    int j = DAW_N, g = daw_page(p), fx = is_a_only(p);
    const char *const *ch = daw_choices(p);
    char *s = daw_id[j], *n = daw_nm[j];
    const char *pn = legacy ? legacy : PDEF[p].name, *gt = PAGE[g].title;
    if (!legacy && pn[0] == gt[0] && pn[1] == gt[1] && pn[2] == gt[2] && !pn[3]) pn = "MODE";   /* (ARP > ARP: "ARP MODE") */
    int i, k = 0;
    if (j >= DAW_MAX) return;
    /* id: "a_cutoff" for the 32 of 26.10. (as they were), else "a_filter_type", "fx_delay_mix" – stays when names change */
    if (fx) { s[k++] = 'f'; s[k++] = 'x'; } else s[k++] = (char)('a' + t);
    s[k++] = '_';
    if (!legacy) {
        for (i = 0; gt[i] && k < 14; i++) if ((gt[i] >= 'A' && gt[i] <= 'Z') || (gt[i] >= '0' && gt[i] <= '9')) s[k++] = (char)(gt[i] | 0x20);
        s[k++] = '_';
    }
    for (i = 0; pn[i] && k < 23; i++)
        if ((pn[i] >= 'A' && pn[i] <= 'Z') || (pn[i] >= 'a' && pn[i] <= 'z') || (pn[i] >= '0' && pn[i] <= '9')) s[k++] = (char)(pn[i] | 0x20);
    s[k] = 0;
    /* name: "A Cutoff" (as it was) or "A Filter Type", "FX Delay Mix" */
    k = 0;
    if (fx) { n[k++] = 'F'; n[k++] = 'X'; } else n[k++] = (char)('A' + t);
    n[k++] = ' ';
    if (!legacy) { for (i = 0; gt[i] && k < 14; i++) n[k++] = gt[i]; n[k++] = ' '; }
    for (i = 0; pn[i] && k < 27; i++) n[k++] = pn[i];
    n[k] = 0;
    /* group: "A · FILTER", "FX · DELAY" */
    {
        char *gn = daw_gn[fx ? NTRK : t][g];
        if (!gn[0]) {
            k = 0;
            if (fx) { gn[k++] = 'F'; gn[k++] = 'X'; } else gn[k++] = (char)('A' + t);
            scopy(gn + k, " \xc2\xb7 ", 5); k += 4;
            scopy(gn + k, gt, 20 - k);
        }
        daw_grp[j] = gn;
    }
    daw_t[j] = (uint8_t)(fx ? 0 : t); daw_pi[j] = (uint8_t)p;
    daw_list[j].id = daw_id[j]; daw_list[j].name = daw_nm[j];
    daw_list[j].min = -daw_off(p);
    daw_list[j].max = (float)PDEF[p].max - daw_off(p);
    daw_list[j].def = daw_of(daw_t[j], p);                   /* (the default: the sound as it loaded – its program's) */
    daw_list[j].unit = PDEF[p].kind == K_SEMI ? "st" : "";
    daw_list[j].steps = PDEF[p].max + 1;                     /* (whole numbers, as on the panel) */
    daw_list[j].choices = ch;
    DAW_N++;
}

static void daw_init(void)                      /* (after the tracks' sounds are loaded) */
{
    uint32_t t, k, p;
    DAW_N = 0;
    for (t = 0; t < NTRK; t++)                   /* the 32 of 26.10., in their places */
        for (k = 0; k < DAW_PER; k++) daw_add(t, DAW_P[k], DAW_NAME[k]);
    for (t = 0; t < NTRK; t++)                   /* every other voice parameter */
        for (p = 0; p < NPARAM; p++) {
            int legacy = 0;
            if (is_a_only(p)) continue;
            for (k = 0; k < DAW_PER; k++) if (DAW_P[k] == p) legacy = 1;
            if (!legacy) daw_add(t, p, 0);
        }
    for (p = 0; p < NPARAM; p++) if (is_a_only(p)) daw_add(0, p, 0);   /* the effects, once */
    vibe_params(daw_list, DAW_N);
    vibe_param_groups(daw_grp);
}

/* ---- 3.1 (SDK 9): the Editor as OberVave wants it – the cards in the panel's order, Cutoff x Resonance as a pad,
 * bipolar knobs where the panel shows -/+, the envelopes and the LFO drawn, a word of help where a name is not enough */
static const char *const ED_TAB[NTRK + 1] = {"A", "B", "C", "D", "FX"};
static vibe_card ed_cards[NPAGE];
static int ed_track(const char *tab) { return tab && tab[0] >= 'A' && tab[0] < 'A' + NTRK && !tab[1] ? tab[0] - 'A' : (int)sel; }
static int ed_env_curve(const char *tab, const char *card, float *pts, int max)   /* ADSR as the panel's knobs stand */
{
    const uint8_t *v = live_p[ed_track(tab)].v;
    int f = card[0] == 'F';
    float a = v[f ? P_FA : P_AA] / 127.0f * 0.3f + 0.02f, d = v[f ? P_FD : P_AD] / 127.0f * 0.3f + 0.02f, s = v[f ? P_FS : P_AS] / 127.0f, r = v[f ? P_FR : P_AR] / 127.0f * 0.3f + 0.02f;
    float h = 0.18f, tot = a + d + h + r;
    int n = max < 48 ? max : 48;
    for (int i = 0; i < n; i++) {
        float x = (float)i / (float)(n - 1) * tot, y;
        if (x < a) y = x / a;
        else if (x < a + d) y = 1.0f - (1.0f - s) * (x - a) / d;
        else if (x < a + d + h) y = s;
        else y = s * (1.0f - (x - a - d - h) / r);
        pts[i] = y < 0.0f ? 0.0f : y;
    }
    return n;
}
static int ed_lfo_curve(const char *tab, const char *card, float *pts, int max)   /* one cycle of the shape */
{
    static const uint8_t SH[8] = {90, 20, 70, 120, 40, 100, 10, 60};
    uint32_t shape = live_p[ed_track(tab)].v[P_LFO_SHAPE];
    int n = max < 32 ? max : 32;
    (void)card;
    for (int i = 0; i < n; i++) {
        float x = (float)i / (float)n, y;
        switch (shape) {
        case L_SAW: y = 1.0f - x; break;
        case L_RAMP: y = x; break;
        case L_SQR: y = x < 0.5f ? 1.0f : 0.0f; break;
        case L_SH: y = SH[(i * 8) / n] / 127.0f; break;
        case L_RND: { int k = (i * 8) / n; float t = (x * 8.0f) - (float)k; y = (SH[k] + (SH[(k + 1) & 7] - SH[k]) * t) / 127.0f; break; }
        default: y = x < 0.5f ? x * 2.0f : 2.0f - x * 2.0f; break;   /* L_TRI */
        }
        pts[i] = y;
    }
    return n;
}
static void daw_editor(void)
{
    static const uint8_t ORDER[] = {PG_OSC1, PG_OSC2, PG_OSCMOD, PG_FILTER, PG_FILTER2, PG_FENV, PG_AENV, PG_LFO, PG_LFODEST, PG_LFO2,
                                    PG_VOICE, PG_OUTPUT, PG_ARP, PG_CHORUS, PG_DELAY, PG_REVERB};
    int j, n = 0;
    for (uint32_t k = 0; k < sizeof ORDER; k++) {
        uint32_t g = ORDER[k];
        ed_cards[n].card = PAGE[g].title;
        ed_cards[n].width = g == PG_FILTER ? 6 : g == PG_LFO2 ? 3 : g == PG_CHORUS ? 3 : 4;
        ed_cards[n].note = g == PG_FILTER ? "the pad: cutoff across, resonance up" : g == PG_FENV || g == PG_AENV ? "the curve follows the knobs" : 0;
        n++;
    }
    vibe_editor_cards(ed_cards, n);
    for (j = 0; j < DAW_N; j++) {
        uint32_t p = daw_pi[j], kind = PDEF[p].kind;
        if (kind == K_BIP || kind == K_SEMI) vibe_editor_control(j, VIBE_CTL_BIPOLAR);
        else if (kind == K_ONOFF) vibe_editor_control(j, VIBE_CTL_TOGGLE);
        if (p == P_CUTOFF && j + 1 < DAW_N && daw_pi[j + 1] == P_RESO && daw_t[j + 1] == daw_t[j]) { vibe_editor_control(j, VIBE_CTL_XY_X); vibe_editor_control(j + 1, VIBE_CTL_XY_Y); }
        switch (p) {                            /* a word where the name does not say it */
        case P_VINTAGE: vibe_param_help(j, "How much the voices drift and differ, like an old polysynth's cards"); break;
        case P_SPREAD: vibe_param_help(j, "Spreads the voices across the stereo field"); break;
        case P_FMORPH: vibe_param_help(j, "Morphs the filter's character between its types"); break;
        case P_KEYTRK: vibe_param_help(j, "The filter follows the keyboard: higher notes open it more"); break;
        case P_XMOD: vibe_param_help(j, "Oscillator 2 modulates oscillator 1's frequency"); break;
        case P_LFO_DELAY: vibe_param_help(j, "The LFO fades in this long after a key"); break;
        case P_GLIDE: vibe_param_help(j, "In MONO and UNISON: the time a note slides to the next"); break;
        default: break;
        }
    }
    vibe_editor_accent(0xFF6A1A);               /* (ACCENT, o3_draw.h) */
    for (uint32_t t = 0; t < NTRK; t++) vibe_editor_tab_color(ED_TAB[t], TRK_RGB[t]);
    vibe_editor_tab_color(ED_TAB[NTRK], 0x3FA9F5);   /* (the FX key's blue) */
    vibe_editor_curve(PAGE[PG_FENV].title, ed_env_curve);
    vibe_editor_curve(PAGE[PG_AENV].title, ed_env_curve);
    vibe_editor_curve(PAGE[PG_LFO].title, ed_lfo_curve);
}

/* the DAW (or the Editor) moved one */
static void daw_from_host(uint32_t j, float v)
{
    uint32_t t, p;
    int32_t x;
    if (j >= (uint32_t)DAW_N) return;
    t = daw_t[j]; p = daw_pi[j];
    x = clampi((int32_t)(v + daw_off(p) + 0.5f), 0, PDEF[p].max);   /* (0..max here: rounds up from .5) */
    if (live_p[t].v[p] == x) return;
    live_p[t].v[p] = (uint8_t)x;
    trk_edited[t] = 1;
}

/* a sound changed here: tell the DAW (only what differs, so the DAW's own moves do not come back) */
static void daw_sync(void)
{
    int j;
    for (j = 0; j < DAW_N; j++) {
        float v = daw_of(daw_t[j], daw_pi[j]);
        if (vibe_param_get(j) != v) vibe_param_set(j, v);
    }
}

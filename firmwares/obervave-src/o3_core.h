/* SPDX-License-Identifier: GPL-3.0-only */
/* OberVave 3 – the state without the panel: settings, programs, the four tracks' sounds, the mixer, the master.
 * The settings and the program store keep OberVave 2.0's layout (the same files later: patterns and sounds carry over). */

static uint32_t o3_ms;                          /* milliseconds, counted by the audio */
#define NFXP (P_REV_MIX - P_CHO_RATE + 1u)     /* the effects' parameters */
#define SET_VERSION 6u
static struct {                                 /* = OberVave 2.0's settings, field for field */
    uint8_t version, prog, tempo, midi_ch;      /* midi_ch 0 = omni, else 1..16: track A's channel, B-D the next three */
    int8_t tune, oct;
    uint8_t pat, click;                         /* pat: the sequencer pattern; click: CLICK_* */
    uint8_t tprog[NTRK];                        /* each track's program */
    uint8_t mix_lvl[NTRK];                      /* the mixer: level 0..127 (100 = unity) */
    uint8_t mix_cho[NTRK], mix_dly[NTRK], mix_rev[NTRK];   /* the sends into chorus, delay, reverb 0..127 */
    int8_t mix_pan[NTRK];                       /* -64..64 */
    uint8_t mix_mute, mix_solo, rec_q, sync_int;   /* mute, solo: bit t; rec_q: (2.0's one quantise, now rec_qt); sync_int: 1 = SYNC
                                                   INTERNAL (a clock / DAW from outside ignored), 0 = AUTO (2.0's pad byte: 0) */
    uint8_t fx[NFXP], fx_ok;                    /* the effects: the song's, not a sound's; ok: FX_OK */
    uint8_t rec_qt[NTRK], rec_qt_ok;            /* REC's quantise per track; ok: RQT_OK */
    uint8_t mix_drv[NTRK], dv_type, dv_amt, dv_tone, dv_mix, dv_ok;   /* the drive send per track, the drive; ok: DV_OK */
    uint8_t m_vol, m_dj, m_drive, m_comp, m_ok; /* MASTER: volume (100 = unity), DJ filter (64 = off), drive, comp; ok: M_OK */
    uint8_t midi_out, mo_ok;                    /* 3.1 (SDK 9): MIDI OUT – 1 = tracks A-D play out on the global channel and the next three
                                                   (OMNI: 1-4), clock out while we lead; ok: MO_OK */
} set;
#define MO_OK 0x6Eu
#define FX_OK 0x5Au
#define RQT_OK 0xA5u
#define M_OK 0x3Cu
#define DV_OK 0x5Du
enum { CLICK_OFF, CLICK_REC, CLICK_ON, CLICK_COUNT, NCLICK };
enum { RQ_OFF, RQ_32, RQ_16, RQ_8, RQ_4, NRQ };
static const uint8_t TPROG_DEF[NTRK] = {0, 6, 42, 57};   /* DARK ARP, FAT BASS, WARM PAD, SOLO LEAD */
static const uint32_t TRK_RGB[NTRK] = {0x60B0FF, 0xFF9628, 0x6EDC96, 0xFF6096};

/* the user's programs: 2 x 50 (2.0's program sectors), all in RAM */
#define NPSECT 2u
#define PS_N 50u
#define PS_TAG 0x50564F42u                      /* "BOVP" */
typedef struct { uint32_t tag; uint8_t used[PS_N], pad[2]; patch_t p[PS_N]; } psect_t;
static psect_t psect[NPSECT];

static uint32_t trk_prog[NTRK], trk_edited[NTRK];
static uint32_t sel;                            /* the selected track: keys and pads play it, the pages edit it */

static void settings_defaults(void)
{
    uint32_t t;
    memset(&set, 0, sizeof set);
    set.version = SET_VERSION; set.tempo = 120; set.click = CLICK_REC;
    for (t = 0; t < NTRK; t++) {
        set.tprog[t] = TPROG_DEF[t]; set.mix_lvl[t] = 100; set.rec_qt[t] = RQ_16;
    }
    for (t = 0; t < NFXP; t++) set.fx[t] = (uint8_t)PDEF[P_CHO_RATE + t].def;   /* the effects full, the sends closed */
    set.fx[P_CHO_MIX - P_CHO_RATE] = set.fx[P_DLY_MIX - P_CHO_RATE] = set.fx[P_REV_MIX - P_CHO_RATE] = 127;
    set.fx_ok = FX_OK; set.rec_q = RQ_16; set.rec_qt_ok = RQT_OK;
    set.dv_type = DV_OVERDRIVE; set.dv_amt = 60; set.dv_tone = 90; set.dv_mix = 127; set.dv_ok = DV_OK;
    set.m_vol = 100; set.m_dj = 64; set.m_ok = M_OK;
}

static void prog_get(uint32_t n, patch_t *p)    /* program n: the user's, else the factory's (65-70 VIBE's, 71-99 INIT) */
{
    n %= NPROG;
    if (psect[n / PS_N].tag == PS_TAG && psect[n / PS_N].used[n % PS_N]) *p = psect[n / PS_N].p[n % PS_N];
    else patch_from_factory(p, n < NFACT ? &FACTORY[n] : n < NFACT + NVIBE ? &FACT_VIBE[n - NFACT] : &FACT_INIT);
}
static void prog_name(uint32_t n, char *out)    /* its 10-character name, trailing spaces cut */
{
    patch_t p; int i;
    prog_get(n, &p);
    for (i = 0; i < (int)PATCH_NAME; i++) out[i] = (char)p.v[NPARAM + i];
    out[PATCH_NAME] = 0;
    for (i = (int)PATCH_NAME - 1; i >= 0 && out[i] == ' '; i--) out[i] = 0;
}

/* the effects are global (as a DAW's return tracks): they live in track A's patch and in set.fx */
static void fx_keep(patch_t *p) { uint32_t i; if (set.fx_ok == FX_OK) for (i = 0; i < NFXP; i++) p->v[P_CHO_RATE + i] = set.fx[i]; }
static void trk_load(uint32_t t, uint32_t n)    /* track t: program n */
{
    patch_t p;
    t %= NTRK; n %= NPROG;
    prog_get(n, &p);
    if (!t) fx_keep(&p);
    live_p[t] = p;
    trk_prog[t] = n; trk_edited[t] = 0;
    set.tprog[t] = (uint8_t)n;
    if (!t) set.prog = (uint8_t)n;
}
static int is_a_only(uint32_t p) { return p >= P_CHO_RATE && p <= P_REV_MIX; }
static patch_t *ed_patch(uint32_t p) { return is_a_only(p) ? &live_p[0] : &live_p[sel % NTRK]; }
static void set_param(uint32_t p, int32_t v)
{
    patch_t *d = ed_patch(p);
    v = clampi(v, 0, PDEF[p].max);
    if (d->v[p] == v) return;
    d->v[p] = (uint8_t)v;
    trk_edited[(uint32_t)(d - live_p) % NTRK] = 1;
}

/* the panel's notes go to the selected track's sound; a release goes where its note began */
static uint8_t note_part[128];
static void play_note(uint32_t on, uint32_t note, uint32_t vel)
{
    note &= 127u;
    if (on) note_part[note] = (uint8_t)(sel % NTRK);
    note_event_p(note_part[note], on, note, vel);
}

static void mix_apply(void)                     /* the settings into the engine; mute and solo as one mask */
{
    uint32_t t, solo = set.mix_solo & ((1u << NTRK) - 1u);
    for (t = 0; t < NTRK; t++) {
        ctl_mix_lvl[t] = set.mix_lvl[t] > 127u ? 127u : set.mix_lvl[t];
        ctl_mix_pan[t] = (int8_t)clampi(set.mix_pan[t], -64, 64);
        ctl_mix_cho[t] = set.mix_cho[t] > 127u ? 127u : set.mix_cho[t];
        ctl_mix_dly[t] = set.mix_dly[t] > 127u ? 127u : set.mix_dly[t];
        ctl_mix_rev[t] = set.mix_rev[t] > 127u ? 127u : set.mix_rev[t];
        ctl_mix_drv[t] = set.mix_drv[t] > 127u ? 127u : set.mix_drv[t];
    }
    ctl_mix_off = solo ? ~solo & ((1u << NTRK) - 1u) : set.mix_mute & ((1u << NTRK) - 1u);
    ctl_dv_type = set.dv_type; ctl_dv_amt = set.dv_amt; ctl_dv_tone = set.dv_tone; ctl_dv_mix = set.dv_mix;
    ctl_tempo = set.tempo; ctl_tune8 = set.tune * 256 / 100;
    vibe_tempo_set((float)set.tempo);           /* (SDK 9) the clock out follows our tempo */
    ctl_m_vol = set.m_vol; ctl_m_dj = set.m_dj; ctl_m_drive = set.m_drive; ctl_m_comp = set.m_comp;
}

/* MASTER: the sum after the effects – DJ filter, drive, compressor, volume, a limiter that never lets it clip */
static float o3_sr = 48000.0f, lvl_l, lvl_r;
static void master(float *l, float *r, int n)
{
    static vd_svf fl, fr; static vd_svf_c c; static int dj_was = -1; static float env;
    int dj = (int)ctl_m_dj - 64, mode = dj <= -2 ? 1 : dj >= 2 ? 2 : 0;
    if (dj != dj_was) {
        float x = (float)(dj < 0 ? -dj : dj) / 64.0f;
        vd_svf_set(&c, mode == 1 ? 18000.0f * vd_pow(60.0f / 18000.0f, x) : 20.0f * vd_pow(6000.0f / 20.0f, x), 1.1f, o3_sr);
        dj_was = dj;
    }
    float d = (float)ctl_m_drive / 127.0f, pre = 1.0f + 8.0f * d, post = (1.0f + d) / pre;
    float cm = (float)ctl_m_comp / 127.0f, thr = vd_pow(10.0f, (-4.0f - 20.0f * cm) / 20.0f), ex = 1.0f - 1.0f / (1.0f + 5.0f * cm);
    float mk = cm > 0.0f ? vd_pow(1.0f / thr, ex * 0.45f) : 1.0f, vol = (float)ctl_m_vol / 100.0f;
    float att = 1.0f - vd_coef(0.003f, o3_sr), rel = 1.0f - vd_coef(0.15f, o3_sr), pl = 0, pr = 0;
    vol *= vol;
    for (int i = 0; i < n; i++) {
        float a = l[i], b = r[i];
        if (mode) { vd_svf_out ol = vd_svf_tick(&fl, &c, a), orr = vd_svf_tick(&fr, &c, b); a = mode == 1 ? ol.lp : ol.hp; b = mode == 1 ? orr.lp : orr.hp; }
        if (d > 0.0f) { a = vd_tanh(a * pre) * post; b = vd_tanh(b * pre) * post; }
        if (cm > 0.0f) {
            float p = vd_maxf(vd_absf(a), vd_absf(b));
            env += (p - env) * (p > env ? att : rel);
            float g = env > thr ? vd_pow(thr / env, ex) : 1.0f;
            a *= g * mk; b *= g * mk;
        }
        a *= vol; b *= vol;
        float ma = vd_absf(a), mb = vd_absf(b);
        if (ma > 0.8f) a = (a < 0 ? -1.0f : 1.0f) * (0.8f + 0.2f * vd_tanh((ma - 0.8f) / 0.2f));
        if (mb > 0.8f) b = (b < 0 ? -1.0f : 1.0f) * (0.8f + 0.2f * vd_tanh((mb - 0.8f) / 0.2f));
        l[i] = a; r[i] = b;
        if (ma > pl) pl = ma;
        if (mb > pr) pr = mb;
    }
    lvl_l = pl > lvl_l ? pl : lvl_l * 0.95f;
    lvl_r = pr > lvl_r ? pr : lvl_r * 0.95f;
}

/* ---- text: a parameter's value as the pages show it (<= 7 characters) */
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void scat(char *d, const char *s) { d += slen(d); while ((*d++ = *s++)) ; }
static void fmt_u(char *s, uint32_t v) { char b[12]; uint32_t i = 0, j = 0; do { b[i++] = (char)('0' + v % 10u); v /= 10u; } while (v); while (i) s[j++] = b[--i]; s[j] = 0; }
static void fmt_ms(char *s, uint32_t ms_x10)
{
    char t[4];
    if (ms_x10 < 1000u) { fmt_u(s, ms_x10 / 10u); scat(s, "."); fmt_u(t, ms_x10 % 10u); scat(s, t); scat(s, "ms"); }
    else if (ms_x10 < 10000u) { fmt_u(s, ms_x10 / 10u); scat(s, "ms"); }
    else { fmt_u(s, ms_x10 / 10000u); scat(s, "."); fmt_u(t, (ms_x10 / 1000u) % 10u); scat(s, t); scat(s, "s"); }
}
static void param_value(char *s, uint32_t p, uint32_t v)
{
    char t[8];
    s[0] = 0;
    switch (PDEF[p].kind) {
    case K_BIP: if (v < 64u) { scat(s, "-"); fmt_u(t, 64u - v); } else { scat(s, v > 64u ? "+" : ""); fmt_u(t, v - 64u); } scat(s, t); break;
    case K_ONOFF: scat(s, v ? "ON" : "OFF"); break;
    case K_WAVE: scat(s, WAVE_NAME[v & 3u]); break;
    case K_FTYPE: scat(s, FTYPE_NAME[v % NFTYPE]); break;
    case K_MODE: scat(s, MODE_NAME[v & 3u]); break;
    case K_LSHAPE: scat(s, LSHAPE_NAME[v % 6u]); break;
    case K_LPHASE: scat(s, LPHASE_NAME[v % 3u]); break;
    case K_TO: scat(s, TO_NAME[v % 3u]); break;
    case K_ARP: scat(s, ARP_NAME[v % NARPMODE]); break;
    case K_DIV: scat(s, DIV_NAME[v % NDIV]); break;
    case K_DDIV: scat(s, DDIV_NAME[v % NDDIV]); break;
    case K_MORPH: if (v == 64u) scat(s, "NOTCH"); else { scat(s, v < 64u ? "LP " : "HP "); fmt_u(t, v < 64u ? 64u - v : v - 64u); scat(s, t); } break;
    case K_SEMI: if (v < 24u) { scat(s, "-"); fmt_u(t, 24u - v); } else { scat(s, v > 24u ? "+" : ""); fmt_u(t, v - 24u); } scat(s, t); break;
    case K_MS:
        if (p == P_GLIDE) { if (!v) scat(s, "OFF"); else { fmt_u(s, GLIDE_MS[v & 127u]); scat(s, "ms"); } }
        else fmt_ms(s, TIME_MS_X10[v & 127u]);
        break;
    case K_HZ: { uint32_t h = LFO_HZ_X100[v & 127u]; fmt_u(s, h / 100u); scat(s, "."); fmt_u(t, (h / 10u) % 10u); scat(s, t); scat(s, "Hz"); } break;
    case K_PCT: fmt_u(s, (v * 100u + 63u) / 127u); scat(s, "%"); break;
    case K_CUT: { uint32_t hz = CUT_HZ[(v + 8u) % 137u]; if (hz < 10000u) { fmt_u(s, hz); scat(s, "Hz"); } else { fmt_u(s, hz / 1000u); scat(s, "."); fmt_u(t, (hz / 100u) % 10u); scat(s, t); scat(s, "k"); } } break;
    case K_OCT: fmt_u(s, v); scat(s, " OCT"); break;
    default: fmt_u(s, v);
    }
}

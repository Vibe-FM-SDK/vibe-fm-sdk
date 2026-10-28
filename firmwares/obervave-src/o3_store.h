/* SPDX-License-Identifier: GPL-3.0-only */
/* OberVave 3 – what stays when the device is off, in the SDK's storage. The files are OberVave 2.0's (its flash
 * sectors as host.c kept them: a 4-byte head, byte 0 = 1, then the struct): "settings", "plob0" / "plob1" (the
 * user's programs, 50 each), "blob0".."blob7" (the patterns). So 2.0's sounds and patterns carry over. */
#define ST_HEAD 4
static uint8_t st_buf[ST_HEAD + (sizeof(pattern_t) > sizeof(psect_t) ? sizeof(pattern_t) : sizeof(psect_t))];   /* (a pattern or a program sector: the biggest files) */
static int st_load(const char *name, void *p, uint32_t n)
{
    memset(st_buf, 0, sizeof st_buf);          /* (a shorter, older file: the rest reads as 0 – e.g. a pattern without knob lanes) */
    if (vibe_store_read(name, st_buf, (int)(ST_HEAD + n)) < (int)(ST_HEAD + 1u) || st_buf[0] != 1) return -1;
    memcpy(p, st_buf + ST_HEAD, n);
    return 0;
}
static int st_save(const char *name, const void *p, uint32_t n)
{
    memset(st_buf, 0, ST_HEAD); st_buf[0] = 1;
    memcpy(st_buf + ST_HEAD, p, n);
    return vibe_store_write(name, st_buf, (int)(ST_HEAD + n));
}

static uint32_t set_dirty, ps_dirty, store_t;   /* ps_dirty: bit i = program sector i; written ~1.5 s after the last change */
static void set_touch(void) { set_dirty = 1; store_t = o3_ms; }

static void store_boot(void)                    /* the settings and the programs as they were, else the defaults */
{
    uint32_t n;
    static const char *const PS[NPSECT] = {"plob0", "plob1"};
    for (n = 0; n < NPSECT; n++)
        if (st_load(PS[n], &psect[n], sizeof psect[n]) || psect[n].tag != PS_TAG) { memset(&psect[n], 0, sizeof psect[n]); psect[n].tag = PS_TAG; }
    if (st_load("settings", &set, sizeof set) || set.version != SET_VERSION) settings_defaults();
    if (set.fx_ok != FX_OK || set.rec_qt_ok != RQT_OK || set.dv_ok != DV_OK || set.m_ok != M_OK) {   /* (2.0 before its last additions) */
        uint8_t keep[NTRK]; memcpy(keep, set.tprog, sizeof keep);
        if (set.rec_qt_ok != RQT_OK) { for (n = 0; n < NTRK; n++) set.rec_qt[n] = set.rec_q % NRQ; set.rec_qt_ok = RQT_OK; }
        if (set.dv_ok != DV_OK) { for (n = 0; n < NTRK; n++) set.mix_drv[n] = 0; set.dv_type = DV_OVERDRIVE; set.dv_amt = 60; set.dv_tone = 90; set.dv_mix = 127; set.dv_ok = DV_OK; }
        if (set.m_ok != M_OK) { set.m_vol = 100; set.m_dj = 64; set.m_drive = set.m_comp = 0; set.m_ok = M_OK; }
        if (set.fx_ok != FX_OK) settings_defaults();
        memcpy(set.tprog, keep, sizeof keep);
    }
    if (set.tempo < 40u || set.tempo > 240u) set.tempo = 120;
    if (set.midi_ch > 16u) set.midi_ch = 0;
    if (set.mo_ok != MO_OK) { set.midi_out = 0; set.mo_ok = MO_OK; }   /* (3.0's settings: MIDI OUT off, as it was) */
}

static void store_poll(void)                    /* from the draw: what changed, once it has been quiet for a while */
{
    static const char *const PS[NPSECT] = {"plob0", "plob1"};
    uint32_t n;
    if ((!set_dirty && !ps_dirty) || o3_ms - store_t < 1500u) return;
    for (n = 0; n < NPSECT; n++) if (ps_dirty >> n & 1u) st_save(PS[n], &psect[n], sizeof psect[n]);
    if (set_dirty) st_save("settings", &set, sizeof set);
    set_dirty = ps_dirty = 0;
}

static void prog_put(uint32_t n, const patch_t *p)   /* program n is the user's now */
{
    n %= NPROG;
    psect[n / PS_N].p[n % PS_N] = *p;
    psect[n / PS_N].used[n % PS_N] = 1;
    ps_dirty |= 1u << (n / PS_N); store_t = o3_ms;
}

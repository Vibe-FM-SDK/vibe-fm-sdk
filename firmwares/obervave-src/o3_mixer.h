/* SPDX-License-Identifier: GPL-3.0-only */
/* OberVave 3 – the MIXER (F1; again: the next row) as in 2.0: rows LEVEL → PAN → CHORUS → DELAY → REVERB → DRIVE →
 * MASTER. Encoders 1-4 = tracks A-D in the row (MASTER: VOLUME, FILTER, DRIVE, COMP). Pads: row 1 start / stop,
 * rows 2-3 mute (SHIFT: solo), row 4 the track. Each track shows its level, what it plays now (a meter after fader,
 * mute and solo) and MUTE / SOLO. */
static uint32_t mix_row;
static const char *const MIX_ROW[7] = {"LEVEL", "PAN", "CHORUS", "DELAY", "REVERB", "DRIVE", "MASTER"};
static uint8_t meter[NTRK];
static uint32_t meter_t;

static uint8_t *mix_send(uint32_t row) { return row == 2u ? set.mix_cho : row == 3u ? set.mix_dly : row == 4u ? set.mix_rev : set.mix_drv; }
static void mixer_knobs(knob_t *k)
{
    static const char *const ML[4] = {"A", "B", "C", "D"}, *const MM[4] = {"VOLUME", "FILTER", "DRIVE", "COMP"};
    for (uint32_t t = 0; t < 4u; t++) {
        k[t].value[0] = 0; k[t].bip = 0;
        k[t].label = mix_row == 6u ? MM[t] : ML[t];
        if (mix_row == 6u) {
            if (t == 0u) { fmt_u(k[t].value, set.m_vol); k[t].v = set.m_vol / 127.0f; }
            else if (t == 1u) { int32_t d = (int32_t)set.m_dj - 64;
                if (d > -2 && d < 2) scat(k[t].value, "OFF"); else { scat(k[t].value, d < 0 ? "LP " : "HP "); fmt_u(k[t].value + 3, (uint32_t)(d < 0 ? -d : d)); }
                k[t].v = set.m_dj / 128.0f; k[t].bip = 1; }
            else { uint32_t v = t == 2u ? set.m_drive : set.m_comp; fmt_u(k[t].value, (v * 100u + 63u) / 127u); scat(k[t].value, "%"); k[t].v = v / 127.0f; }
        } else if (mix_row == 0u) { fmt_u(k[t].value, set.mix_lvl[t]); k[t].v = set.mix_lvl[t] / 127.0f; }
        else if (mix_row == 1u) {
            int32_t pn = set.mix_pan[t];
            if (!pn) scat(k[t].value, "C"); else { scat(k[t].value, pn < 0 ? "L" : "R"); fmt_u(k[t].value + 1, (uint32_t)(pn < 0 ? -pn : pn)); }
            k[t].v = (pn + 64) / 128.0f; k[t].bip = 1;
        } else { uint8_t v = mix_send(mix_row)[t]; fmt_u(k[t].value, ((uint32_t)v * 100u + 63u) / 127u); scat(k[t].value, "%"); k[t].v = v / 127.0f; }
    }
}
static void mixer_turn(int k, int e)
{
    if (mix_row == 6u) {
        if (k == 0) set.m_vol = (uint8_t)clampi((int32_t)set.m_vol + e, 0, 127);
        else if (k == 1) set.m_dj = (uint8_t)clampi((int32_t)set.m_dj + e, 0, 128);
        else if (k == 2) set.m_drive = (uint8_t)clampi((int32_t)set.m_drive + e * 2, 0, 127);
        else set.m_comp = (uint8_t)clampi((int32_t)set.m_comp + e * 2, 0, 127);
    } else if (mix_row == 0u) set.mix_lvl[k] = (uint8_t)clampi((int32_t)set.mix_lvl[k] + e, 0, 127);
    else if (mix_row == 1u) set.mix_pan[k] = (int8_t)clampi((int32_t)set.mix_pan[k] + e, -64, 64);
    else { uint8_t *sv = mix_send(mix_row); sv[k] = (uint8_t)clampi((int32_t)sv[k] + e * 2, 0, 127); }
    mix_apply(); set_touch();
}
static void mixer_pad(uint32_t p, int shift_held)   /* rows 2-3: mute (SHIFT: solo), row 4: the track */
{
    uint32_t t = p % 4u, r = p / 4u;
    if (r == 1u || r == 2u) {
        if (shift_held) set.mix_solo ^= (uint8_t)(1u << t); else set.mix_mute ^= (uint8_t)(1u << t);
        mix_apply(); set_touch();
    } else if (r == 3u) sel = t;
}

static void meters_take(void)                   /* the tracks' peaks: up at once, down ~2 px per 30 ms (0..44, square law) */
{
    if (o3_ms - meter_t < 30u) return;
    meter_t = o3_ms;
    for (uint32_t t = 0; t < NTRK; t++) {
        uint32_t pk = ui_trk_peak[t], m = 0;
        ui_trk_peak[t] = 0;
        while (m < 44u && pk > 6400u * (m + 1u) / 44u * (m + 1u) / 44u) m++;   /* (6400 = the output at 0 dBFS) */
        meter[t] = (uint8_t)(m > meter[t] ? m : meter[t] > 2u ? meter[t] - 2u : 0u);
    }
}
static void mixer_view(void)
{
    knob_t k[4];
    mixer_knobs(k);
    for (int i = 0; i < 7; i++) {               /* the rows, the open one bright */
        int x = CX0 + i * 66, on = (uint32_t)i == mix_row;
        vg_round(x, 41, 62, 24, 4, on ? (i == 6 ? 0xE8423A : ACCENT) : CELL);
        vg_text(MIX_ROW[i], x + 31, 46, VG_BOLD, on ? 0x111214 : DIM, VG_CENTER);
    }
    for (uint32_t t = 0; t < 4u; t++) {
        int x = CX0 + (int)t * COLW, cx = x + COLW / 2 - 2, on = t == sel, mu = set.mix_mute >> t & 1u, so = set.mix_solo >> t & 1u;
        int off = mu || (set.mix_solo && !so);
        uint32_t c = TRK_RGB[t];
        if (on && mix_row != 6u) vg_frame(x, 70, COLW - 4, 206, 2, c);
        vg_text(k[t].label, cx, 76, VG_MED, mix_row == 6u ? DIM : c, VG_CENTER);
        dial(cx, 126, 30, k[t].v, k[t].bip, mix_row == 6u ? 0xE8423A : c);
        vg_text(k[t].value, cx, 160, VG_BIG, FG, VG_CENTER);
        if (mix_row == 6u) continue;            /* (MASTER: its four knobs only) */
        {   /* the fader's level and the meter */
            int h = set.mix_lvl[t] * 72 / 127, mh = meter[t] * 72 / 44;
            vg_rect(x + 14, 196, 12, 72, CELL); vg_rect(x + 14, 268 - h, 12, h, off ? MUTED : c);
            vg_rect(x + 30, 196, 9, 72, CELL);
            for (int j = 0; j < mh; j += 3) vg_rect(x + 30, 266 - j, 9, 2, j >= 64 ? RED : j >= 50 ? 0xFFC828 : GREEN);
        }
        vg_text(ctl_trk_on >> t & 1u ? "PLAY" : "STOP", x + 78, 198, VG_MED, ctl_trk_on >> t & 1u ? GREEN : MUTED, VG_CENTER);
        vg_round(x + 50, 236, 26, 26, 4, mu ? RED : CELL2); vg_text("M", x + 63, 241, VG_MED, mu ? 0x111214 : DIM, VG_CENTER);
        vg_round(x + 80, 236, 26, 26, 4, so ? 0xFFD23C : CELL2); vg_text("S", x + 93, 241, VG_MED, so ? 0x111214 : DIM, VG_CENTER);
    }
}
static void mixer_leds(void)                    /* row 1 start/stop, rows 2-3 mute / solo, row 4 the tracks */
{
    for (uint32_t t = 0; t < 4u; t++) {
        uint32_t mu = set.mix_mute >> t & 1u, so = set.mix_solo >> t & 1u;
        vibe_led_pad((int)t, GREEN, ctl_trk_on >> t & 1u ? 1.0f : 0.15f);
        vibe_led_pad((int)(4u + t), so ? 0xFFD23C : RED, mu || so ? 1.0f : 0.12f);
        vibe_led_pad((int)(8u + t), so ? 0xFFD23C : RED, mu || so ? 1.0f : 0.12f);
        vibe_led_pad((int)(12u + t), TRK_RGB[t], t == sel ? 1.0f : 0.25f);
    }
}

/* GLOBAL, SHIFT + ENTER: "RESET ALL?" – ENTER wipes everything saved and starts afresh (2.0's) */
static uint32_t reset_ask;
static void reset_all(void)
{
    static const char *const F[] = {"settings", "blob0", "blob1", "blob2", "blob3", "blob4", "blob5", "blob6", "blob7", "plob0", "plob1",
                                    "psnd0", "psnd1", "psnd2", "psnd3", "psnd4", "psnd5", "psnd6", "psnd7"};
    for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++) vibe_store_delete(F[i]);
    vibe_request(VIBE_REQ_RESTART);
}
static void reset_view(void)
{
    vg_round(80, 70, 320, 170, 8, 0x2A0E0E); vg_round(82, 72, 316, 166, 7, 0x140A0A);
    vg_text("RESET ALL?", 240, 88, VG_BIG, RED, VG_CENTER);
    vg_text("Patterns, sounds and settings", 240, 128, VG_MED, FG, VG_CENTER);
    vg_text("go back to the factory state.", 240, 148, VG_MED, FG, VG_CENTER);
    vg_text("ENTER = RESET", 240, 180, VG_MED, RED, VG_CENTER);
    vg_text("any other key = cancel", 240, 206, VG_MED, DIM, VG_CENTER);
}

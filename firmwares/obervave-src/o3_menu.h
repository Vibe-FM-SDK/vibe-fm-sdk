/* SPDX-License-Identifier: GPL-3.0-only */
/* OberVave 3 – MENU: what every control does on the page shown, laid out as on the device (2.0's overview).
 * Hold SHIFT: the second layer. Closed by MENU again, HOME or another area. */
static void ov_cell(int x, int y, int w, int h, const char *name, const char *what, const char *sub, uint32_t col)
{
    vg_round(x, y, w, h, 4, 0x18223A);
    if (col) vg_rect(x, y, 3, h, col);
    vg_text(name, x + 7, y + 3, VG_SMALL, DIM, VG_LEFT);
    vg_text(what && what[0] ? what : "–", x + 7, y + 17, VG_BOLD, what && what[0] ? FG : MUTED, VG_LEFT);
    if (sub && sub[0]) vg_text(sub, x + w - 5, y + 3, VG_SMALL, 0x3FA9F5, VG_RIGHT);
}
static void overview(void)
{
    knob_t k[4];
    char t[40];
    vg_rect(0, 0, VIBE_SCREEN_W, VIBE_SCREEN_H, 0x0B1220);
    vg_text("CONTROLS", 10, 6, VG_MED, ACCENT, VG_LEFT);
    t[0] = 0; scat(t, in_seq() ? SV_NAME[SV_EFF] : PAGE[page].title); scat(t, " · TRACK "); { char l[2] = {(char)('A' + sel), 0}; scat(t, l); }
    vg_text(t, 104, 9, VG_SMALL, FG, VG_LEFT);
    vg_text(shift ? "with SHIFT" : "hold SHIFT: 2nd layer", 470, 9, VG_SMALL, shift ? 0x3FA9F5 : MUTED, VG_RIGHT);
    int sq = in_seq(), entry = sq && (SV_ENTRY(seq_view) || seq_view == SV_SLOTS);
    if (page == PG_MIXER) mixer_knobs(k); else if (sq) seq_knobs(k); else page_knobs(k);
    ov_cell(6, 30, 92, 40, "↩ / HOME", "PERFORM", "", 0);
    for (int i = 0; i < 4; i++) { char n[2] = {(char)('1' + i), 0}; ov_cell(104 + i * 74, 30, 70, 40, n, k[i].label ? k[i].label : "", k[i].label ? k[i].value : "", ACCENT); }
    ov_cell(404, 30, 70, 40, "DATA", shift ? "TEMPO" : save_armed ? "SLOT" : sq && SV_ENTRY(seq_view) ? "BAR" : sq || page == PG_MIXER ? "PATTERN" : page == PG_GLOBAL ? "MIDI OUT" : "PATCH", "", ACCENT);
    { static const char *FN[6] = {"MIXER", "TRACKS", "PRG -", "PRG +", "SAVE", "GLOBAL"}, *FSH[6] = {"", "", "", "", "HOLD", "UNDO"};
      for (int i = 0; i < 6; i++) { char n[3] = {'F', (char)('1' + i), 0}; ov_cell(6 + i * 78, 76, 74, 40, n, shift && FSH[i][0] ? FSH[i] : FN[i], "", 0x3FA9F5); } }
    ov_cell(6, 122, 74, 40, "SHIFT", shift ? "HELD" : "2ND LAYER", "", shift ? 0x3FA9F5 : 0);
    ov_cell(84, 122, 74, 40, "OCT - / +", "OCTAVE", "", 0);
    ov_cell(162, 122, 70, 40, "ENTER", shift && page == PG_GLOBAL ? "RESET ALL" : f5_down ? "DICE" : page == PG_GLOBAL ? "SYNC" : "SELECT", "", shift && page == PG_GLOBAL ? RED : 0);
    for (int i = 0; i < 6; i++) {
        int x = 236 + i * 40, on = (sq ? CK_SEQ : key_lit()) == i;
        vg_round(x, 122, 37, 40, 4, on ? KEY_RGB[i] : vg_mix(0x0B1220, KEY_RGB[i], 0.45f));
        vg_text(KEY_NAME[i], x + 18, 136, VG_BOLD, on ? 0x111214 : FG, VG_CENTER);
    }
    ov_cell(6, 170, 112, 40, "A", entry ? (seq_view == SV_SLOTS ? "SLOT" : "STEP ◀ ▶") : "TRACK A–D", "", GREEN);
    ov_cell(122, 170, 112, 40, "B", sq && seq_view == SV_STEPS && !shift ? "NOTE UP/DOWN" : sq && seq_view == SV_TRACK ? "TRANSPOSE" : "PATCH",
            sq && seq_view == SV_STEPS ? "SHIFT: PATCH" : "", GREEN);
    ov_cell(6, 216, 74, 40, "◀", "PRG -", "", 0); ov_cell(84, 216, 74, 40, "MENU", "CONTROLS", "", 0); ov_cell(162, 216, 72, 40, "▶", "PRG +", "", 0);
    ov_cell(6, 262, 74, 52, "REC", shift ? "CLEAR" : "RECORD", "", RED);
    ov_cell(84, 262, 74, 52, "STOP", "STOP", "", 0); ov_cell(162, 262, 72, 52, "PLAY", "START", "", GREEN);
    {   /* the pads, row by row */
        static const char *const ROWS[4] = {"START / STOP", "SELECT TRACK", "NOTES", "NOTES"};
        static const char *const MROWS[4] = {"START / STOP", "MUTE (SHIFT: SOLO)", "MUTE (SHIFT: SOLO)", "SELECT TRACK"};
        static const char *const SROWS[4] = {"STEPS 1–4", "STEPS 5–8", "STEPS 9–12", "STEPS 13–16"};
        static const char *const TROWS[4] = {"START / STOP", "SELECT TRACK", "START / STOP", "SELECT TRACK"};
        int mx = page == PG_MIXER, st = sq && SV_ENTRY(seq_view), tr = sq && seq_view == SV_TRACKS;
        vg_text("PADS", 350, 172, VG_SMALL, DIM, VG_LEFT);
        for (int r = 0; r < 4; r++) {
            int y = 188 + r * 32;
            for (int c = 0; c < 4; c++) {
                uint32_t col = st ? KEY_RGB[CK_SEQ] : (mx ? r == 3 : tr ? (r & 1) : r == 1) ? TRK_RGB[c] : r == 0 || (tr && r == 2) ? (rec ? RED : GREEN) : mx ? RED : DIM;
                vg_round(242 + c * 26, y, 22, 26, 3, vg_mix(0x0B1220, col, 0.55f));
            }
            vg_text(st ? SROWS[r] : mx ? MROWS[r] : tr ? TROWS[r] : (r == 0 && rec) ? "RECORD" : ROWS[r], 350, y + 7, VG_SMALL, FG, VG_LEFT);
        }
        for (int c = 0; c < 4; c++) { char l[2] = {(char)('A' + c), 0}; vg_text(l, 253 + c * 26, 172, VG_BOLD, TRK_RGB[c], VG_CENTER); }
    }
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* OberVave 3 – the display, 480 x 320, drawn for VIBE (the style of GROOVE and the VIBE pages of 2.0): a header,
 * the page over the full width, the soft keys below */
#define BG 0x0B0D12
#define FG 0xE6EAF2
#define DIM 0x8A93A6
#define MUTED 0x5B6577
#define CELL 0x141A26
#define CELL2 0x24314A
#define SEP 0x1C2333
#define RED 0xFF5A4D
#define GREEN 0x6EE7A8
#define ACCENT 0xFF6A1A
/* the six colour keys, left to right as the signal goes (Sven 20.10.): the sound (oscillators, filter), what shapes it,
 * how it is played, its effects, the sequencer. Each keeps its colour wherever it sits. */
enum { CK_SOUND, CK_ENV, CK_LFO, CK_ARP, CK_FX, CK_SEQ };
static const char *const KEY_NAME[6] = {"SOUND", "ENV", "LFO", "ARP", "FX", "SEQ"};
static const uint32_t KEY_RGB[6] = {0xB38CFF, 0xF5C93F, 0x6EE7A8, 0xFF6A1A, 0x3FA9F5, 0xFF5A8A};

static const char *toast; static uint32_t toast_t;
static void say(const char *s) { toast = s; toast_t = o3_ms; }
static int blink(void) { return (int)(o3_ms / 300u) & 1; }

static void dial(int cx, int cy, int r, float v, int bip, uint32_t col)
{
    const float span = 0.75f;                   /* from 7:30 round to 4:30 */
    float px = 0, py = 0;
    for (int i = 0; i <= 48; i++) {
        float t = i / 48.0f, a = 0.625f - span * t;
        float x = cx + vd_sin(a + 0.25f) * r, y = cy - vd_sin(a) * r;
        int on = bip ? (v >= 0.5f ? (t >= 0.5f && t <= v) : (t <= 0.5f && t >= v)) : t <= v;
        if (i) vg_line(px, py, x, y, 4.0f, on ? col : CELL2);
        px = x; py = y;
    }
    float a = 0.625f - span * vd_clamp(v, 0, 1);
    vg_circle((float)cx, (float)cy, r - 9.0f, CELL2);
    vg_line((float)cx, (float)cy, cx + vd_sin(a + 0.25f) * (r - 11), cy - vd_sin(a) * (r - 11), 3.0f, FG);
}

/* the page area: the full width (no key column – the colour keys light themselves, the page's tabs carry its colour,
 * MENU explains every control): x 10..470, four columns of 115 px */
#define CX0 10
#define CW 460
#define COLW 115
static int colx(int i) { return CX0 + i * COLW + COLW / 2; }

/* four knobs of a page: label, the dial, the value – big enough to read on the device */
typedef struct { const char *label; char value[16]; float v; int bip; } knob_t;
static void knobs_row(const knob_t *k, int y, uint32_t col)
{
    for (int i = 0; i < 4; i++) {
        int cx = colx(i);
        if (!k[i].label) continue;
        vg_text(k[i].label, cx, y, VG_MED, DIM, VG_CENTER);
        dial(cx, y + 58, 38, k[i].v, k[i].bip, col);
        vg_text(k[i].value, cx, y + 102, VG_BIG, FG, VG_CENTER);
    }
}

static void softkeys(const char *const *f, uint32_t on_mask)
{
    vg_rect(0, 282, VIBE_SCREEN_W, 38, 0x0E1220);
    for (int i = 0; i < 6; i++) {
        int x = 40 + i * 80, on = on_mask >> i & 1u;
        vg_round(x - 37, 286, 74, 30, 5, on ? ACCENT : CELL2);
        vg_text(f[i], x, 293, VG_MED, on ? 0x111214 : FG, VG_CENTER);
        vibe_label(VB_F1 + i, f[i]);
    }
    for (int i = 0; i < 6; i++) vibe_label(VB_K1 + i, KEY_NAME[i]);
}
static void track_letters(int x, int y)         /* A-D: the selected one bright and underlined, muted ones red */
{
    for (uint32_t t = 0; t < NTRK; t++) {
        char l[2] = {(char)('A' + t), 0};
        int mu = set.mix_mute >> t & 1u, on = t == sel;
        uint32_t c = mu ? RED : TRK_RGB[t];
        if (!on && !mu) c = vg_mix(BG, c, 0.45f);
        vg_text(l, x + (int)t * 18, y, VG_MED, c, VG_CENTER);
        if (mu) vg_rect(x + (int)t * 18 - 5, y + 9, 10, 2, RED);
        if (on) vg_rect(x + (int)t * 18 - 5, y + 20, 10, 2, TRK_RGB[t]);
    }
}
static void out_meters(int x, int y, int h)     /* the output, left and right (after MASTER): two thin bars */
{
    float lv[2] = { lvl_l, lvl_r };
    for (int c = 0; c < 2; c++) {
        float v = lv[c] > 0.00001f ? 1.0f + vd_log2(lv[c]) / 8.0f : 0.0f;   /* 48 dB over the height */
        int m = (int)(v * h); if (m < 0) m = 0; if (m > h) m = h;
        vg_rect(x + c * 7, y, 5, h, CELL);
        vg_rect(x + c * 7, y + h - m, 5, m, lv[c] > 0.79f ? RED : lv[c] > 0.5f ? 0xFFC828 : GREEN);
    }
}
/* the header: the page (in its key's colour), the pattern, A-D, the sound, the tempo, play, the output */
static void header(const char *title, uint32_t title_rgb)
{
    char b[24], n[PATCH_NAME + 1];
    vg_text(title, 12, 8, VG_MED, title_rgb, VG_LEFT);
    track_letters(178, 7);
    fmt_u(b, trk_prog[sel] + 1u); prog_name(trk_prog[sel], n);
    vg_text(b, 266, 8, VG_MED, TRK_RGB[sel], VG_RIGHT);
    vg_text(n, 272, 8, VG_MED, trk_edited[sel] ? ACCENT : FG, VG_LEFT);
    fmt_u(b, ctl_daw_quarter ? (60u * obv_fs + ctl_daw_quarter / 2u) / ctl_daw_quarter : set.tempo);   /* (a DAW's tempo: green) */
    vg_text(b, 432, 8, VG_MED, ctl_daw_quarter ? GREEN : DIM, VG_RIGHT);
    if (ctl_seq_run) vg_tri(438, 9, 450, 16, 438, 23, GREEN); else vg_rect(438, 10, 11, 11, MUTED);
    out_meters(458, 6, 24);
    vg_rect(10, 34, CW, 1, SEP);
}
static void toast_draw(void)
{
    if (!toast || o3_ms - toast_t > 1500u) return;
    int w = vg_text_width(toast, VG_MED) + 32;
    vg_round(240 - w / 2, 234, w, 32, 6, ACCENT);
    vg_text(toast, 240, 241, VG_MED, 0x111214, VG_CENTER);
}

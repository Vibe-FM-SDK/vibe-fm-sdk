/* SPDX-License-Identifier: GPL-3.0-only */
/* OberVave 3 – the start: 2.0's logo strike (obv_splash.h, 240 x 49, two layers: the glowing letters, the same with
 * the three bands), drawn twice the size for VIBE's 480 px. Dark, two short flashes, a dip, it holds; the bands sweep
 * in from the left, the line below fades in; it stands until 2.4 s (any control ends it sooner). */
#include "obv_splash.h"
#define SPL_BG 0x0E121C                         /* 2.0's panel, the logo's own background (its colour 0x0883 = none) */
static uint16_t spl_px[2][SPLASH_W * SPLASH_H]; /* both layers unpacked once */
static int splash_on = 1;

static void splash_unpack(void)
{
    const uint8_t *src[2] = {SPLASH_RLE0, SPLASH_RLE1};
    for (int l = 0; l < 2; l++) {
        const uint8_t *a = src[l];
        uint32_t i = 0, n = SPLASH_W * SPLASH_H;
        while (i < n) { uint32_t run = *a++, c = *a++; while (run-- && i < n) spl_px[l][i++] = SPLASH_PAL[c]; }
    }
}
static uint16_t blend565(uint16_t a, uint16_t b, uint32_t f)   /* a -> b by f/256 */
{
    int32_t ar = a >> 11, ag = a >> 5 & 63, ab = a & 31, br = b >> 11, bg = b >> 5 & 63, bb = b & 31, k = (int32_t)f;
    return (uint16_t)((ar + (br - ar) * k / 256) << 11 | (ag + (bg - ag) * k / 256) << 5 | (ab + (bb - ab) * k / 256));
}
static void splash_draw(uint16_t *fb)
{
    static const uint16_t STRIKE[12] = {0, 90, 20, 0, 200, 60, 30, 256, 180, 256, 256, 256};   /* per 40 ms frame */
    const uint16_t bg = 0x0883;
    uint32_t k = o3_ms / 40u, fade = k < 12u ? STRIKE[k] : 256u, sub = k < 16u ? 0u : (k - 16u) * 32u;
    int32_t x_to = k < 10u ? 0 : k < 24u ? (int32_t)((k - 10u) * (k - 10u) * 240u / 196u) + 2 : 242;
    vg_clear(SPL_BG);
    for (uint32_t y = 0; y < SPLASH_H; y++)
        for (uint32_t x = 0; x < SPLASH_W; x++) {
            uint16_t c = spl_px[(int32_t)x < x_to ? 1 : 0][y * SPLASH_W + x];
            if (c == bg) continue;
            if (fade < 256u) c = blend565(bg, c, fade);
            uint16_t *p = fb + (90u + 2u * y) * VIBE_SCREEN_W + 2u * x;
            p[0] = p[1] = p[VIBE_SCREEN_W] = p[VIBE_SCREEN_W + 1] = c;
        }
    if (sub) {
        uint32_t c = vg_mix(SPL_BG, 0x60B0FF, sub >= 256u ? 1.0f : sub / 256.0f);
        vg_text("4-TRACK ANALOG POLY", 240, 208, VG_MED, c, VG_CENTER);
        vg_text("VIBE FM MK1 · OBERVAVE 3", 240, 234, VG_MED, c, VG_CENTER);
    }
    if (o3_ms >= 2400u) splash_on = 0;
}

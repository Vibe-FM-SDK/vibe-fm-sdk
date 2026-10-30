/* SPDX-License-Identifier: GPL-3.0-only */
/* tools/fm1/fm1_screen.h – the FM-1's 240 x 240 screen on the VIBE's 480 x 320: as large as the display is high (320 x
 * 320, in the middle, black beside it – the captions of the keys are on the keys), scaled by area: every 3 source pixels
 * become 4, a source pixel is copied whole wherever it covers an output pixel whole and only the pixel across an edge is
 * mixed (sharp text, no blur, no uneven doubling). Only the rows that changed since the last picture are drawn again –
 * on a converted FM-1 firmware the picture is made in the audio's flow. */
#define FS_OUT 320
#define FS_DX ((VIBE_SCREEN_W - FS_OUT) / 2)
static uint16_t fs_i0[FS_OUT]; static uint8_t fs_w[FS_OUT];   /* per output row / column: first source pixel, weight of the next (0..255) */
static uint16_t fs_prev[240 * 240], fs_h[240][FS_OUT];      /* the screen last drawn; each source row widened */
static uint8_t fs_dirty[240], fs_ready;
static uint16_t fs_mix(uint16_t a, uint16_t b, uint32_t f)
{
  if (!f || a == b) return a;
  uint32_t r = ((a >> 11) * (256u - f) + (b >> 11) * f) >> 8, g = ((a >> 5 & 63u) * (256u - f) + (b >> 5 & 63u) * f) >> 8;
  uint32_t bl = ((a & 31u) * (256u - f) + (b & 31u) * f) >> 8;
  return (uint16_t)(r << 11 | g << 5 | bl);
}
static void fm1_screen_draw(const uint16_t *src240, uint16_t *out)
{
  if (!fs_ready) {
    for (int o = 0; o < FS_OUT; o++) {           /* output pixel o covers source [3o/4, 3(o+1)/4) */
      int a = o * 3, b = a + 3, i0 = a / 4, edge = (i0 + 1) * 4;   /* (in quarters of a source pixel) */
      fs_i0[o] = (uint16_t)(i0 > 238 ? 238 : i0);
      fs_w[o] = (uint8_t)(b > edge ? ((b - edge) * 256) / 3 : 0);
      if (i0 > 238) fs_w[o] = 255;
    }
    for (int y = 0; y < 240; y++) fs_prev[y * 240] = (uint16_t)~src240[y * 240];   /* (every row new) */
    for (int y = 0; y < VIBE_SCREEN_H; y++) { uint16_t *row = out + y * VIBE_SCREEN_W; for (int x = 0; x < FS_DX; x++) row[x] = row[VIBE_SCREEN_W - 1 - x] = 0; }
    fs_ready = 1;
  }
  for (int y = 0; y < 240; y++) {
    const uint16_t *s = src240 + y * 240; uint16_t *pv = fs_prev + y * 240;
    const uint32_t *a = (const uint32_t *)s, *b = (const uint32_t *)pv; int x = 0;
    while (x < 120 && a[x] == b[x]) x++;          /* (word by word) */
    fs_dirty[y] = x < 120;
    if (!fs_dirty[y]) continue;
    for (x = 0; x < 240; x++) pv[x] = s[x];
    for (x = 0; x < FS_OUT; x++) fs_h[y][x] = fs_mix(s[fs_i0[x]], s[fs_i0[x] + 1], fs_w[x]);
  }
  for (int y = 0; y < FS_OUT; y++) {
    uint32_t i0 = fs_i0[y], w = fs_w[y];
    if (!fs_dirty[i0] && !(w && fs_dirty[i0 + 1])) continue;   /* (the SDK's frame buffer keeps the rest) */
    uint16_t *row = out + y * VIBE_SCREEN_W + FS_DX;
    for (int x = 0; x < FS_OUT; x++) row[x] = fs_mix(fs_h[i0][x], fs_h[i0 + 1][x], w);
  }
}

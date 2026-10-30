/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * tools/fm1/dinghy/host.c – the fake FM-1 for the Dinghy family (OberVave 2, FM-106, FRODHES, SHINE ON …), as the VIBE
 * gives it. A Dinghy firmware's platform.c includes "../../tests/host.c" when DINGHY_HOST is set; tools/fm1/build-fm1.sh
 * puts this file there, next to an unchanged copy of the firmware's src/ – nothing in the firmware changes.
 * It is the firmware's own tests/host.c without its script engine, stdio and main(): keys, buttons, encoders and
 * LEDs that the adapter (tools/fm1/fm1_dinghy.c) drives, the 240 x 240 screen, USB-MIDI in, the settings sector (kept on
 * the VIBE by the adapter). What a firmware's platform has beyond this (FM-106: its preset bank, SysEx out …) comes from
 * firmware/<id>/dinghy_app.c, which tools/fm1-convert.sh takes from the firmware's own tests/host.c (and may be edited).
 * One core: DINGHY_DUAL_CORE stays unset, as in every tests/host.c – the voices render one after the other.
 */
#include <stdint.h>
#include <stddef.h>

#ifndef APP_NAME
#define APP_NAME FM1_NAME
#endif
#ifndef APP_VER
#define APP_VER FM1_VERSION
#endif
#define FS 44100u
#define HALF_FRAMES 256u
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")

void *memset(void *d, int c, size_t n); void *memcpy(void *d, const void *s, size_t n); void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
/* (what a tests/host.c prints goes nowhere on the VIBE) */
#define printf(...) ((void)0)
#define fprintf(...) ((void)0)
#define puts(...) ((void)0)
#define fflush(...) ((void)0)

static volatile uint32_t fm1_ms;                  /* counted by the adapter: one per simulated millisecond */
#ifndef FM1_OWN_TICKS
static uint32_t fm1_ticks(void) { return fm1_ms * 24000u; }
#endif
static inline void fm1_irq_off(void) {}
static inline void fm1_irq_on(void) {}
enum { FM1_ADC_BATT = 3, FM1_ADC_MASTER = 4 };   /* (hal/fm1_adc.h) the battery reads full, MASTER all the way up: the */
static int32_t fm1_adc_read(uint32_t ch) { return ch == FM1_ADC_MASTER ? 1023 : 900; }
static inline int32_t platform_battery_bars(void) { return 4; }   /* VIBE's VOLUME is the runtime's */
#ifndef DINGHY_HAS_STR_CPY
static void str_cpy(char *d, const char *s, unsigned max) { unsigned i = 0; while (s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0; }
#endif
#ifdef FM1_MATRIX_OWN                             /* (GRIMM: its hal/fm1_matrix.h has the layout – and its ghost test) */
#include "fm1_matrix.h"
#else
#define FM1_NCOL 11u
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
};
#endif
static uint8_t fm1_led_bg[FM1_NCOL];
static volatile uint16_t fm1_led_bg_ns;
static struct { uint32_t overruns, underruns, alt, starts, stalls, rate_sets, flowing, missed; } uac;   /* (no USB audio here) */
static uint32_t audio_lates;

/* the panel: the adapter sets these */
static struct { uint32_t notes, buttons; } fm1_in;
static uint32_t host_edges, host_released, host_note_edges;
static int32_t host_enc[8];
static uint8_t host_led[64], host_glow[64];
static uint32_t fm1_input_edges(uint32_t *released) { uint32_t b = host_edges; host_edges = 0; if (released) *released = host_released; host_released = 0; return b; }
static uint32_t fm1_input_note_edges(void) { uint32_t b = host_note_edges; host_note_edges = 0; return b; }
static int32_t fm1_enc_take(uint32_t e) { int32_t d = e < 8u ? host_enc[e] : 0; if (e < 8u) host_enc[e] = 0; return d; }
static void fm1_led_key(uint32_t id, int on) { if (id < 64u) host_led[id] = (uint8_t)(on != 0); }
static void fm1_led_key_glow(uint32_t id, int on) { if (id < 64u) host_glow[id] = (uint8_t)(on != 0); }

/* the screen: 240 x 240 RGB565, as the ST7789 gets it (gfx.c sends the bytes swapped) */
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    for (uint32_t j = y; j < y + h && j < 240u; j++)
        for (uint32_t i = x; i < x + w && i < 240u; i++) fb[j * 240u + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u) { uint16_t c = px[j * w + i]; fb[(y + j) * 240u + x + i] = (uint16_t)(c >> 8 | c << 8); }
}
#define CV_MAX (240u * 96u)
#include "gfx.c"

/* the settings sector (4 KiB on the FM-1): kept on the VIBE by the adapter (vibe_store "settings") */
static uint8_t host_store[4096 + 4];
static volatile uint32_t host_store_dirty;
static int settings_load(void *p, uint32_t n)
{
    if (host_store[0] != 1 || n > 4096u) return -1;
    memcpy(p, host_store + 4, n);
    return 0;
}
static int settings_save(const void *p, uint32_t n)
{
    if (n > 4096u) return -1;
    host_store[0] = 1; host_store[1] = (uint8_t)n; host_store[2] = (uint8_t)(n >> 8);
    memcpy(host_store + 4, p, n);
    host_store_dirty = 1;
    return 0;
}

#ifndef NPSECT
#define NPSECT 2u
#endif
#ifndef NBLOB
#define NBLOB 8u
#endif

/* program sectors (presets) and pattern sectors: kept on VIBE by the adapter */
static uint8_t host_psect[NPSECT][4096 + 4];
static volatile uint32_t host_psect_dirty;

static int psect_load(uint32_t i, void *p, uint32_t n)
{
    if (i >= NPSECT || host_psect[i][0] != 1 || n > 4096u) return -1;
    memcpy(p, host_psect[i] + 4, n);
    return 0;
}
static int psect_save(uint32_t i, const void *p, uint32_t n)
{
    if (i >= NPSECT || n > 4096u) return -1;
    host_psect[i][0] = 1; host_psect[i][1] = (uint8_t)n; host_psect[i][2] = (uint8_t)(n >> 8);
    memcpy(host_psect[i] + 4, p, n);
    host_psect_dirty |= (1u << i);
    return 0;
}

static uint8_t host_blob[NBLOB][4096 + 4];
static volatile uint32_t host_blob_dirty;

static int blob_load(uint32_t i, void *p, uint32_t n)
{
    if (i >= NBLOB || host_blob[i][0] != 1 || n > 4096u) return -1;
    memcpy(p, host_blob[i] + 4, n);
    return 0;
}
static int blob_save(uint32_t i, const void *p, uint32_t n)
{
    if (i >= NBLOB || n > 4096u) return -1;
    host_blob[i][0] = 1; host_blob[i][1] = (uint8_t)n; host_blob[i][2] = (uint8_t)(n >> 8);
    memcpy(host_blob[i] + 4, p, n);
    host_blob_dirty |= (1u << i);
    return 0;
}

/* USB-MIDI in (from the adapter); MIDI out goes nowhere */
static uint8_t mq[64][3];
static uint32_t mq_w, mq_r;
static int midi_get(uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    if (mq_r == mq_w) return 0;
    *st = mq[mq_r % 64u][0]; *d1 = mq[mq_r % 64u][1]; *d2 = mq[mq_r % 64u][2];
    mq_r++;
    return 1;
}
static void midi_send(uint32_t st, uint32_t d1, uint32_t d2) { (void)st; (void)d1; (void)d2; }

static void app_audio(int32_t *out, uint32_t n);
static void app_main(void);
static void platform_poll(void);                  /* the adapter: parks the main loop until the next millisecond */

#ifdef DINGHY_APP
#include DINGHY_APP                               /* firmware/<id>/dinghy_app.c: the rest of the firmware's platform */
#endif

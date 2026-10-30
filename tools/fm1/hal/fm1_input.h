/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on VIBE (from zp12's simulator): the key / button / encoder matrix and the LEDs. sim.c sets the debounced state (fm1_in.notes,
 * .buttons), the edges and the encoder steps; the app's LED columns (fm1_led, fm1_led_dim) are read back through
 * FM1_KEYMAP, the device's matrix (firmware/hal/fm1_input.h). */
#pragma once
#include <stdint.h>
#include "fm1_time.h"
#define FM1_NCOL 11u
#define FM1_NKEY 41u              /* ids: 0..13 buttons, 14..40 note keys */
#define FM1_NENC 7u
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
};
enum { FM1_BTN_OCT_DOWN = 0, FM1_BTN_OCT_UP = 1 };
static volatile struct {
    uint32_t notes, buttons, pressed, released, notes_pressed, frames;
    int16_t enc_steps[FM1_NENC];
} fm1_in;
static uint8_t fm1_led[FM1_NCOL], fm1_led_bg[FM1_NCOL];
#ifdef FM1_LED_PLANES                       /* (Melodee: dim planes, fm1_led_dim[plane][col]; the build sees it in the source) */
static uint8_t fm1_led_dim[FM1_LED_PLANES][FM1_NCOL];
#define FM1_LED_DIM_ROW (fm1_led_dim[0])
#else
static uint8_t fm1_led_dim[FM1_NCOL];
#define FM1_LED_DIM_ROW fm1_led_dim
#endif
static volatile uint16_t fm1_led_bg_ns;
/* newer Felucca's LED extras (breathing, a mid level, the sweep / idle animations, the glow level): kept, not shown */
static uint8_t fm1_led_breath[FM1_NCOL], fm1_led_mid[FM1_NCOL];
/* (newer still: the dim frame's divider, a duty level, faint and "hi" columns – FiMba, X0X, JIANT …: kept, not shown) */
/* felucca-bsp's analog side (velocity, the MASTER pot through the ADC): its state, idle here (fm1-base sets it) */
static volatile struct {
    uint16_t con; uint8_t drive, probe, pend, col, rowbit, driven, probing, phase; uint16_t master, timeouts; uint8_t immediate, selftest, time_req; uint32_t us;
} fm1_an;
static volatile uint8_t fm1_led_dim_div, fm1_led_brightness = 4;
static uint8_t fm1_led_faint[FM1_NCOL], fm1_led_hi[FM1_NCOL];
static uint8_t fm1_led_dim_mask[4];                 /* (Melodee: the dim planes' duty) */
static inline void fm1__led_lines(uint32_t rowmask) { (void)rowmask; }
static inline void fm1_led_dim_level(uint32_t lo) { (void)lo; }
static inline void fm1_led_anim_start(uint32_t glow) { (void)glow; }
static inline void fm1_led_idle_start(uint32_t glow) { (void)glow; }
static inline int fm1_led_anim_on(void) { return 0; }
static inline int fm1_led_idle_on(void) { return 0; }
static inline void fm1_led_anim_stop(void) {}
static void fm1_input_init(void) {}
static void fm1_input_scan(void) { fm1_in.frames++; }
static void fm1_input_tick(void) {}
static int32_t fm1_enc_take(uint32_t e)
{
    int32_t s = fm1_in.enc_steps[e % FM1_NENC];
    fm1_in.enc_steps[e % FM1_NENC] = 0;
    return s;
}
static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t p;
    sim_yield();                                /* SLOOP's main loop waits for the next frame polling ui_input
                                                   (main.c): each poll parks it until the next millisecond */
    p = fm1_in.pressed;
    if (released)
        *released = fm1_in.released;
    fm1_in.pressed = fm1_in.released = 0;
    return p;
}
static uint32_t fm1_input_note_edges(void)
{
    uint32_t p = fm1_in.notes_pressed;
    fm1_in.notes_pressed = 0;
    return p;
}
static void fm1_led_key(uint32_t id, int on)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                if (on) fm1_led[p] |= (uint8_t)(1u << r);
                else fm1_led[p] &= (uint8_t)~(1u << r);
            }
}
/* SLOOP 2.7: buttons held over MIDI (REMOTE); the fake panel reads them as held too */
static volatile uint32_t fm1_remote_btn;
#ifndef FM1_LED_PHASES
#define FM1_LED_PHASES 4u          /* (felucca-bsp's LED dim phases – niagara sizes its tables by it; not shown here) */
#endif
/* the headphone jack (niagara, in felucca-bsp's input header): the VIBE's (and the plugin's) output is always the full stereo one */
static inline int fm1_jack_phones(void) { return 1; }
/* the LEDs' mid level (felucca-bsp: lit one frame in fm1__mid_n; FRASIL reads the phase): kept, not shown */
#ifndef FM1_LED_MID_N
#define FM1_LED_MID_N 8u
#endif
static uint8_t fm1__mid_n = FM1_LED_MID_N, fm1__mid_ph;
/* the LED frames' extra levels (niagara: fm1_led_pwm[phase][col]) and the key-pressure probe (MONK-1): kept, not shown / nothing to sense */
static uint8_t fm1_led_pwm[FM1_LED_PHASES][FM1_NCOL];
static inline uint32_t fm1_cap_probe(uint32_t rowbit) { (void)rowbit; return 0; }

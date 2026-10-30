/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on VIBE (from zp12's simulator): the time base. TIMER4 counts 24 MHz ticks of the simulated clock (sim.c advances it a
 * millisecond at a time); waiting parks the main loop until the clock got there (sim_yield). */
#pragma once
#include <stdint.h>
#define FM1_TICKS_PER_US 24u
static volatile uint32_t sim_ticks;
static void sim_yield(void);                    /* sim.c: park the main loop until the next millisecond */
static inline void fm1_time_init(void) {}
#ifndef FM1_OWN_TICKS                     /* (OPN1, SID-1, CHIPBOX: their own fm1_ticks) */
static inline uint32_t fm1_ticks(void) { return sim_ticks; }
#endif
static inline uint32_t fm1_micros(void) { return sim_ticks / FM1_TICKS_PER_US; }
static void fm1_delay_us(uint32_t us)
{
    uint32_t start = sim_ticks, span = us * FM1_TICKS_PER_US;
    while ((uint32_t)(sim_ticks - start) < span)
        sim_yield();
}
static void fm1_delay_ms(uint32_t ms) { fm1_delay_us(ms * 1000u); }

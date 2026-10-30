/* SPDX-License-Identifier: GPL-3.0-only */
/* VIBE: the FM-1's performance counters (the corex2 block) do not exist here. The cycle count follows the simulated
 * clock (360 MHz: 15 cycles a 24 MHz tick); fm1_perf_cycles_run says "not running", so a firmware falls back to TIMER4
 * as it does on a unit where the counter stands; no stalls. */
#pragma once
#include <stdint.h>
#include "fm1_time.h"
static inline uint32_t fm1_perf_cycles(void) { return fm1_ticks() * 15u; }
static inline int fm1_perf_cycles_run(void) { return 0; }
static inline void fm1_perf_stalls(uint32_t s[3]) { s[0] = s[1] = s[2] = 0; }
static inline void fm1_perf_stalls_enable(int on) { (void)on; }

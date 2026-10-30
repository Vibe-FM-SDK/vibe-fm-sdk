/* SPDX-License-Identifier: GPL-3.0-only */
/* VIBE: the FM-1's supply setup (P33 analog registers) does not exist here: always ready, rails read as their set value */
#pragma once
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"      /* (the PMU bus, fm1_p33_*, lives there as in felucca-bsp) */
static uint8_t fm1_power_ready = 1;
static inline int fm1_power_init(void) { return 1; }
enum { FM1_RAIL_SYSVDD, FM1_RAIL_VDC14, FM1_RAIL_VDDIO, FM1_RAILS };
static inline uint32_t fm1_power_get(uint32_t rail) { return rail == FM1_RAIL_SYSVDD ? 15u : 7u; }
static inline void fm1_clock_regs(uint32_t *v) { for (int i = 0; i < 5; i++) v[i] = 0; }
static inline uint32_t fm1_cpu_ticks(void) { return 0; }

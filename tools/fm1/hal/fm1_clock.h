/* SPDX-License-Identifier: GPL-3.0-only */
/* VIBE: the FM-1's clock registers (felucca-bsp chip/fm1_clock.h) do not exist here: read as zeros, the stock clock
 * already (360 MHz: nothing to fix) */
#pragma once
#include <stdint.h>
static inline void fm1_clock_read(uint32_t r[6]) { for (int i = 0; i < 6; i++) r[i] = 0; }
static inline int fm1_clock_stock(void) { return 0; }
static inline uint32_t fm1_clock_mult(void) { return 0; }
static inline uint32_t fm1_clock_mhz_of(uint32_t mult) { return mult * 4u / 3u; }

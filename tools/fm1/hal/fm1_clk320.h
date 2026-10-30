/* SPDX-License-Identifier: GPL-3.0-only */
/* VIBE: no clock to switch (JIANT's 320 MHz option): the check says "not the expected state", nothing switches */
#pragma once
#include <stdint.h>
typedef struct { uint32_t pll2_con1, sys_div, sfc_baud, sfc_con, cache_b9; } fm1_clk320_t;
static inline uint32_t fm1_clk320_mult(void) { return 0; }
static inline int fm1_clk320_check(fm1_clk320_t *p) { (void)p; return -1; }
static inline void fm1_clk320_switch(fm1_clk320_t *p) { (void)p; }
static inline int fm1_clk_320(void) { return 0; }

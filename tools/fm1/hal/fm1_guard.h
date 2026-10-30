/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on VIBE (from zp12's simulator): no hardware guards */
#pragma once
#include <stdint.h>
#include "fm1_irq.h"
enum { FM1_GUARD_STACK = 1, FM1_GUARD_WRITE = 2, FM1_GUARD_BUS = 4, FM1_GUARD_PC = 8 };
static void fm1_guard_enable(uint32_t which) { (void)which; }
static void fm1_guard_lock_top(void) {}
static void fm1_guard_unlock_top(void) {}
static void fm1_guard_pc_open(void) {}
static int fm1_guard_top_locked(void) { return 1; }
static inline uint32_t fm1_sp(void) { return 0; }   /* (the console's stack figures: nothing to show here) */
static inline uint32_t fm1_ssp(void) { return 0; }
static inline uint32_t fm1_usp(void) { return 0; }

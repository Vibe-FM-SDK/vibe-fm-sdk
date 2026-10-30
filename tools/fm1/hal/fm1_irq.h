/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on VIBE (from zp12's simulator): interrupts. sim.c calls the two ISRs (TIMER5, ALNK0) itself, between passes of the main
 * loop: nothing interrupts it, so the critical sections are no-ops. */
#pragma once
#include <stdint.h>
enum { FM1_IRQ_EXCEPTION = 1, FM1_IRQ_ALNK0 = 11, FM1_IRQ_SPI1 = 16, FM1_IRQ_UART1 = 20,
       FM1_IRQ_SARADC = 24, FM1_IRQ_SPI2 = 37, FM1_IRQ_TIMER5 = 63, FM1_IRQ_SOFT0 = 120 };
typedef struct {
    uint32_t magic, count, vec, pc, rets, emu, dbg, sp, psr, icfg, uptime_ms;
    uint32_t etm[4];
    uint32_t early;
    uint32_t core;
} fm1_crash_t;
#define FM1_CRASH_MAGIC 0x43525348u   /* (newer Felucca reads the last crash at boot: here there never is one) */
static fm1_crash_t fm1_crash;
#define FM1_CRASH_CTX_N 4u                /* (felucca-bsp: the registers at the last crashes – here none) */
typedef struct { uint32_t count, pc, r[16], usp, ssp; } fm1_crash_ctx_t;
static fm1_crash_ctx_t fm1_crash_ctx[FM1_CRASH_CTX_N];
static void fm1_fault(const fm1_crash_t *c);
static inline uint32_t fm1_icfg(void) { return 0; }
static inline void fm1_icfg_set(uint32_t v) { (void)v; }
static inline void fm1_irq_off(void) {}
static inline void fm1_irq_on(void) {}
static void fm1_irq_init(void) {}
static void fm1_irq_attach(uint32_t n, void (*h)(void), uint32_t prio) { (void)n; (void)h; (void)prio; }
static void fm1_irq_mask(uint32_t n) { (void)n; }
static void fm1_irq_enable_all(void) {}

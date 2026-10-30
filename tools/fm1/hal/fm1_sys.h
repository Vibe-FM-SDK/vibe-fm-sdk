/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on VIBE (from zp12's simulator): system HAL. A power-on reset; feeding the watchdog is where the main loop passes the
 * millisecond (sim_yield); a reboot or an update entry stops the simulated FM-1 (sim_halt). */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"
#include "fm1_time.h"
#include "fm1_clk320.h"   /* (JIANT reads fm1_clk_320() from here) */
/* felucca-bsp: the PMU bus (P33): nothing behind it on the VIBE (its register numbers kept for the callers) */
#define FM1_P3_ANA_CON0 0x00u
#define FM1_P3_ANA_CON1 0x01u
#define FM1_P3_ANA_CON6 0x06u
#define FM1_P3_ANA_CON9 0x09u
static uint32_t fm1_p33_timeouts;
static inline uint8_t fm1_p33_read(uint32_t a) { (void)a; return 0; }
static inline void fm1_p33_write(uint32_t a, uint8_t d) { (void)a; (void)d; }
#define FM1_UPDATA_PARM_LEN 112u
static void sim_halt(uint32_t why);             /* sim.c: 1 reboot, 2 UBOOT, 3 update */
static struct { uint8_t p3_rst; uint32_t rst_src; uint8_t wdt_con; } fm1_boot;
static void fm1_reset_reason(void) { fm1_boot.p3_rst = 1; }   /* power-on */
static void fm1_wdt_arm(uint8_t t) { (void)t; }
static void fm1_wdt_feed(void) { sim_yield(); }
static void fm1_wdt_stop(void) {}
static void fm1_reboot(void) { sim_halt(1); }
static void fm1_enter_uboot(void) { sim_halt(2); }
static void fm1_core_reset(void) { sim_halt(1); }
static void fm1_enter_update(const uint8_t *parm) { (void)parm; sim_halt(3); }
static void fm1_updata_parm_clear(void) {}
static void fm1_mailbox_clear(void) {}
static int fm1_mem_readable(uint32_t a, uint32_t n) { (void)a; (void)n; return 0; }
static uint8_t fm1_peek8(uint32_t a) { (void)a; return 0; }
/* SLOOP 2.7: the supplies (VDC14, SYSVDD) as M-VAVE's firmware leaves them: 1.60 V, 1.38 V */
static uint32_t fm1_vdc14(void) { return 7u; }
static uint32_t fm1_sysvdd(void) { return 15u; }
static void fm1_supplies_stock(void) {}
/* X0X raises the core supply as it starts: nothing to raise on the VIBE */
static inline void fm1_vdc14_raise(uint32_t level) { (void)level; }
static inline void fm1_sysvdd_raise(uint32_t level) { (void)level; }
/* one core: nothing to hold (Melodee holds CPU1 before it clears RAM) */
static inline void fm1_core1_stop(void) {}

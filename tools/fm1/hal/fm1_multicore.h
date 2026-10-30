/* SPDX-License-Identifier: GPL-3.0-only */
/* VIBE: no second core. The firmwares' multicore layers (JIANT's fm1_mc_*, Melodee's fm1_multicore_start / AW_*) answer
 * "not there": a job is never taken, the caller does the work itself – the one-core path every one of them has. */
#pragma once
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_cpu1.h"
/* JIANT */
typedef struct { volatile uint32_t job, arg; void (*volatile fn)(uint32_t); volatile uint32_t check; uint32_t pad0[12];
                 volatile uint32_t alive, done, bad; uint32_t pad1[13]; } fm1_mc_box_t;
static fm1_mc_box_t fm1_mc_box;
static uint8_t fm1_mc_on, fm1_mc_lost;
static uint32_t fm1_mc_max_ticks;
#define FM1_MC_BAR_MAGIC 0x3152414Au
#define FM1_MC_CLK_ARMED (1u << 16)
#define FM1_MC_CLK_TRIP (2u << 16)
typedef struct { uint32_t magic, barred; } fm1_mc_bar_t;
static fm1_mc_bar_t fm1_mc_bar;
static inline uint32_t fm1_mc_ticks(void) { return fm1_ticks(); }
static inline int fm1_mc_barred(void) { return fm1_mc_bar.magic == FM1_MC_BAR_MAGIC && (fm1_mc_bar.barred & 1u); }
static inline void fm1_mc_bar_set(void) { fm1_mc_bar.magic = FM1_MC_BAR_MAGIC; fm1_mc_bar.barred |= 1u; }
static inline void fm1_mc_hold(void) {}
static inline int fm1_mc_start(void) { return -1; }
static inline void fm1_mc_park(void) {}
static inline int fm1_mc_run(void (*fn)(uint32_t), uint32_t arg) { (void)fn; (void)arg; return 0; }
static inline int fm1_mc_done(uint32_t job) { (void)job; return 1; }
static inline int fm1_mc_wait(void) { return 0; }
/* Melodee */
#define FM1_CORE1_BAR_MAGIC 0x52414231u
typedef struct { uint32_t magic, barred; } fm1_core1_bar_t;
static fm1_core1_bar_t fm1_core1_bar;
static inline int fm1_core1_barred(void) { return fm1_core1_bar.magic == FM1_CORE1_BAR_MAGIC && fm1_core1_bar.barred == 1u; }
static inline void fm1_core1_bar_set(void) { fm1_core1_bar.magic = FM1_CORE1_BAR_MAGIC; fm1_core1_bar.barred = 1; }
static inline int fm1_core1_boot_allowed(uint32_t power_ready, uint32_t boot_failed) { (void)power_ready; (void)boot_failed; return 0; }
static struct { volatile uint32_t count; uint32_t echoes, cpu, dbg, emu, pc, rets; } fm1_core1_fault;
typedef volatile uint32_t fm1_aw_word;
#define AW_WORD fm1_aw_word
static inline uint32_t fm1_aw_load(volatile uint32_t *p) { return *p; }
static inline void fm1_aw_store(volatile uint32_t *p, uint32_t v) { *p = v; }
#define AW_LOAD(p) fm1_aw_load(p)
#define AW_STORE(p, v) fm1_aw_store(p, v)
#define AW_TICKS() fm1_ticks()
#define AW_TIMEOUT_TICKS (10000u * FM1_TICKS_PER_US)
#define AW_HOLD() fm1_core1_stop()
#define AW_RETIRED() fm1_core1_bar_set()
#define AW_FAULTS() fm1_core1_fault.count
#define AW_FAILURE() ((void)0)
#define AW_RAM_LOOP __attribute__((noinline, used))
static inline int fm1_multicore_start(void) { return -1; }

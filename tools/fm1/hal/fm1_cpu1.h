/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on VIBE: no second core. sloop_vibe.c builds with FELUCCA_CPU1 0 (one core, the same sound bit for bit,
 * src/cpu1.h); felucca.c still includes this header, so it holds what the one-core build names. */
#pragma once
#include <stdint.h>
#include "fm1_sys.h"      /* (fm1_core1_stop lives there: Melodee calls it without this header) */
#define FM1_C1_STACK_WORDS 1u
#define FM1_C1_MARK 0x43314D4Bu
static uint32_t _c1_ustack[1];
static volatile uint32_t fm1_c1_trace;
static uint8_t fm1_c1_on;
static void fm1_cpu1_hold(void) {}
static int fm1_cpu1_start(void) { return -1; }
/* newer Felucca hands core 1 jobs: 0 = not taken, the caller does the work itself (as on a unit without core 1) */
static inline int fm1_cpu1_run(void (*fn)(uint32_t), uint32_t arg) { (void)fn; (void)arg; return 0; }
static inline int fm1_cpu1_wait(void) { return 0; }
static uint8_t fm1_c1_gave_up;
static inline void fm1_cpu1_halt(void) {}
/* newer firmwares: which core runs this (always 0 here), a lock between the cores (one core: nothing to lock), the
 * second core held at a reset */
static inline uint32_t fm1_cnum(void) { return 0; }
static inline uint32_t fm1__lock(void) { return 0; }
static inline void fm1__unlock(uint32_t v) { (void)v; }
/* X0X, TRIBE …: the core-1 mailbox and its "one core until power-off" bar (nothing to hand over here) */
typedef struct { volatile uint32_t job, arg; void (*volatile fn)(uint32_t); volatile uint32_t check; uint32_t pad0[12];
                 volatile uint32_t alive, done, bad, bad_job, bad_done, bad_fn; uint32_t pad1[10]; } fm1_c1_mb_t;
static fm1_c1_mb_t fm1_c1_mb;
#define FM1_C1_BAR_MAGIC 0x43314241u
static struct { uint32_t magic, barred; } fm1_c1_bar;
static inline void fm1_cpu1_bar(void) { fm1_c1_bar.magic = FM1_C1_BAR_MAGIC; fm1_c1_bar.barred = 1; }

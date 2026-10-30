/* SPDX-License-Identifier: GPL-3.0-only */
/* VIBE: no cache to turn into RAM (Melodee's MELODEE_CACHE_RAM): "not ready", the firmware keeps its plain path */
#pragma once
#include <stdint.h>
#define FM1_CACHE_READY 1u
static struct { uint32_t status, con_before, data_before, instruction_before, con_after, data_after, instruction_after,
                failure_address, test_ticks; } fm1_cache;
static void fm1_cache_init(uint32_t a) { (void)a; }

/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on VIBE (from zp12's simulator): the MASTER pot (the page's knob) and a full battery */
#pragma once
#include <stdint.h>
enum { FM1_ADC_BATT = 3, FM1_ADC_MASTER = 4 };
static volatile int32_t sim_master10 = 1023;   /* 0..1023 */
static void fm1_adc_init(void) {}
static int32_t fm1_adc_read(uint32_t ch) { return ch == FM1_ADC_MASTER ? sim_master10 : 700; }
/* felucca-bsp's ADC control word (fm1-base's plat_fm1.c writes it into fm1_an.con): the value only */
#define FM1_ADC_CON(div, delay) ((uint32_t)((div) & 7u) | 8u | ((uint32_t)((delay) & 15u) << 12))
#define FM1_ADC_CON_SDK FM1_ADC_CON(6, 15)

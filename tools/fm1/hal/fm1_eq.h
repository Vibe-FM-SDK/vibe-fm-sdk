/* SPDX-License-Identifier: GPL-3.0-only */
/* VIBE: the AC79's hardware EQ block does not exist here: "not run" (< 0), the firmware's software path takes it */
#pragma once
#include <stdint.h>
static inline int fm1_eq_run(float row[4][8], const float *in, float *out, uint32_t n) { (void)row; (void)in; (void)out; (void)n; return -1; }

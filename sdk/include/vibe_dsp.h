/* SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE. */
/*
 * vibe_dsp.h – small, fast building blocks for sound: maths without libc, oscillators, envelopes, filters, noise.
 * Everything is per-sample and allocation-free, so it runs the same in the browser and on the device.
 */
#ifndef VIBE_DSP_H
#define VIBE_DSP_H
#include "vibe.h"

#define VD_PI 3.14159265358979f
#define VD_TAU 6.28318530717959f

static inline float vd_clamp(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float vd_absf(float v) { return v < 0 ? -v : v; }
static inline float vd_minf(float a, float b) { return a < b ? a : b; }
static inline float vd_maxf(float a, float b) { return a > b ? a : b; }

float vd_sin(float phase);              /* phase in turns (0..1 = one cycle) */
float vd_exp2(float x);                 /* 2^x */
float vd_log2(float x);
float vd_pow(float a, float b);
float vd_tanh(float x);                 /* soft clip */
float vd_sqrt(float x);
float vd_mtof(float note);              /* MIDI note -> Hz */
float vd_coef(float seconds, float sr); /* one-pole coefficient: reaches ~1/e of the way in `seconds` */

/* white noise, xorshift */
typedef struct { uint32_t s; } vd_noise;
static inline float vd_noise_next(vd_noise *n) { uint32_t x = n->s ? n->s : 0x9E3779B9u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; n->s = x; return (float)(int32_t)x * (1.0f / 2147483648.0f); }
uint32_t vd_rand(void);                 /* shared generator for firmware logic */

/* band-limited oscillators (polyBLEP) */
float vd_blep(float t, float dt);
float vd_saw(float phase, float dt);
float vd_square(float phase, float dt);

/* state-variable filter (TPT): one call gives low, band and high */
typedef struct { float ic1, ic2; } vd_svf;
typedef struct { float lp, bp, hp; } vd_svf_out;
vd_svf_out vd_svf_run(vd_svf *f, float in, float cutoff_hz, float q, float sr);
/* faster when the cutoff stays put for a block: compute the coefficients once, then tick per sample */
typedef struct { float k, a1, a2, a3; } vd_svf_c;
void vd_svf_set(vd_svf_c *c, float cutoff_hz, float q, float sr);
static inline vd_svf_out vd_svf_tick(vd_svf *f, const vd_svf_c *c, float in) {
  float v3 = in - f->ic2, v1 = c->a1 * f->ic1 + c->a2 * v3, v2 = f->ic2 + c->a2 * f->ic1 + c->a3 * v3;
  f->ic1 = 2.0f * v1 - f->ic1; f->ic2 = 2.0f * v2 - f->ic2;
  vd_svf_out o = { v2, v1, in - c->k * v1 - v2 }; return o;
}

/* envelope: attack (linear), decay to sustain and release (exponential) */
typedef struct { float level, a_inc, d_coef, r_coef, sustain; int stage; } vd_env;   /* stage: 0 off 1 a 2 d 3 r */
void  vd_env_set(vd_env *e, float a, float d, float s, float r, float sr);
void  vd_env_gate(vd_env *e, int on);
float vd_env_next(vd_env *e);

#endif

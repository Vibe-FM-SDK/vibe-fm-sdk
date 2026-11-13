// SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE.
// The position from a MIDI clock (SDK 27, 13.11.2026) – runtime/common/vibe_clock_follow.h line for line, for the browser:
// a master sends START and 24 clocks a quarter; the position is a line (an anchor and a slope) that runs on with the
// measured tempo and, at every master quarter, makes the next quarter longer or shorter by half the error plus an eighth
// of its sum (at most 3 %), jumping only at START, a Song Position Pointer, a tempo change over 4 % or an error over 1/4.
// Pure: the caller gives each message with its time in ms (one clock for all calls), vcfBeat(c, t) reads it.
export const VCF_MAX_CORR = 0.03, VCF_JUMP = 0.25;

export function vcfNew() {
  return { has: 0, playing: 0, bpm: 0, a_t: 0, a_beat: 0, slope: 0, moving: 0, ticks: 0, last_tick_t: 0, prev_q_t: -1, integ: 0, q_period: new Float64Array(8), nq: 0, t0: 0, b0: 0, k0: 0, t0_ok: 0 };
}
export function vcfBeat(c, t) {
  if (!c.moving || t <= c.a_t) return c.a_beat;
  return c.a_beat + (t - c.a_t) * c.slope;
}
export function vcfStart(c, t) { c.has = 1; c.playing = 1; c.moving = 0; c.ticks = 0; c.a_beat = 0; c.a_t = t; c.integ = 0; c.prev_q_t = -1; c.t0_ok = 0; }
export function vcfContinue(c, t) { c.has = 1; c.playing = 1; c.a_beat = vcfBeat(c, t); c.a_t = t; c.moving = 0; c.integ = 0; c.prev_q_t = -1; c.t0 = t; c.b0 = c.a_beat; c.k0 = c.ticks; c.t0_ok = 1; }
export function vcfStop(c, t) { c.a_beat = vcfBeat(c, t); c.a_t = t; c.moving = 0; c.playing = 0; }
export function vcfSpp(c, t, sixteenths) { c.ticks = sixteenths * 6; c.a_beat = sixteenths / 4; c.a_t = t; c.moving = 0; c.integ = 0; c.prev_q_t = -1; c.t0 = t; c.b0 = c.a_beat; c.k0 = c.ticks; c.t0_ok = 1; }
export function vcfTempoMs(c) {
  if (!c.nq) return 0;
  const n = Math.min(c.nq, 8); let s = 0;
  for (let i = 0; i < n; i++) s += c.q_period[i];
  return s / n;
}
// every F8: its time. Returns 1 when a quarter was measured.
export function vcfTick(c, t) {
  let quarter = 0;
  if (c.has && c.last_tick_t > 0 && t - c.last_tick_t > 1000) c.nq = 0;
  c.has = 1; c.last_tick_t = t;
  if (!c.playing) return 0;
  const k = c.ticks++;
  if (k % 24) {                                   // between quarters: no tempo measured yet – a provisional one (the C header)
    if (c.nq === 0 && c.t0_ok && k > c.k0 + 1) {
      const per = (t - c.t0) * 24 / (k - c.k0);
      if (per > 50 && per < 4000) {
        const cur = vcfBeat(c, t), on = c.b0 + (k - c.k0) / 24;   // (never backwards)
        c.a_beat = c.moving && cur > on ? cur : on; c.a_t = t; c.slope = 1 / per; c.moving = 1; c.bpm = 60000 / per;
      }
    }
    return 0;
  }
  let qb = Math.floor(k / 24);
  if (c.a_beat > qb + 1 && !c.moving) qb = c.a_beat;
  if (c.prev_q_t >= 0) {
    const p = t - c.prev_q_t;
    if (p > 50 && p < 4000) {
      const m = vcfTempoMs(c);
      if (m > 0 && (p > m * 1.04 || p < m * 0.96)) { c.nq = 0; c.moving = 0; }
      for (let i = 7; i > 0; i--) c.q_period[i] = c.q_period[i - 1];
      c.q_period[0] = p; if (c.nq < 8) c.nq++;
      c.bpm = 60000 / vcfTempoMs(c);
      quarter = 1;
    }
  }
  c.prev_q_t = t;
  const per = vcfTempoMs(c);
  if (per <= 0) {                                 // the very first clock: the line starts here, the tempo from the next ones
    c.a_t = t; c.a_beat = qb; c.t0 = t; c.b0 = qb; c.k0 = k; c.t0_ok = 1; c.moving = 0; c.integ = 0;
    return quarter;
  }
  if (!c.moving) {                                // a (re)start with a tempo: the line from this quarter on
    c.a_t = t; c.a_beat = qb; c.moving = 1; c.slope = 1 / per; c.integ = 0;
    return quarter;
  }
  const err = vcfBeat(c, t) - qb;
  if (err > VCF_JUMP || err < -VCF_JUMP) { c.a_t = t; c.a_beat = qb; c.slope = 1 / per; c.integ = 0; return quarter; }
  c.integ += err;
  let corr = -(err * 0.5 + c.integ * 0.125);
  if (corr > VCF_MAX_CORR) corr = VCF_MAX_CORR;
  if (corr < -VCF_MAX_CORR) corr = -VCF_MAX_CORR;
  c.a_beat = vcfBeat(c, t); c.a_t = t;
  c.slope = (1 + corr) / per;
  return quarter;
}
export function vcfWatch(c, t) {
  if (c.has && t - c.last_tick_t > 1000) { c.has = 0; c.a_beat = vcfBeat(c, t); c.a_t = t; c.moving = 0; c.nq = 0; c.bpm = 0; return 1; }
  return 0;
}
// the line as the audio side needs it (a message): when the position is wanted at time t, vcfBeat(line, t)
export const vcfLine = (c) => ({ has: c.has, playing: c.playing, moving: c.moving, bpm: c.bpm, a_t: c.a_t, a_beat: c.a_beat, slope: c.slope });
// (the processor) write vibe_transport_info at addr in the firmware's memory: has@0 playing@4 bpm f32@8 sig@12/@16 beat f64@24
// bar_start f64@32 loop@40 – beat = the line at time t; has 0 once when the clock stops leading
export function vcfWrite(buf, addr, line, t, st) {
  const dv = new DataView(buf);
  if (!line || !line.has) { if (st.was) { dv.setInt32(addr, 0, true); dv.setInt32(addr + 4, 0, true); st.was = 0; } return; }
  const beat = vcfBeat(line, t);
  dv.setInt32(addr, 1, true); dv.setInt32(addr + 4, line.playing ? 1 : 0, true); dv.setFloat32(addr + 8, line.bpm, true);
  dv.setInt32(addr + 12, 4, true); dv.setInt32(addr + 16, 4, true);
  dv.setFloat64(addr + 24, beat, true); dv.setFloat64(addr + 32, Math.floor(beat / 4) * 4, true); dv.setInt32(addr + 40, 0, true);
  st.was = 1;
}

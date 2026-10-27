// SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE.
// Runs a firmware .wasm in Node without a browser: sends events, renders audio to WAV and the display to PNG.
//   node tools/wasmtest.mjs build/groove.wasm <script.json> <outdir>
// script: [{"t":0.0,"ev":[type,id,down,value]}, {"t":1.2,"shot":"pattern"}, ...], "seconds": 8
//         optional "input": "voice.wav" (IN 1) and "input2": "synth.wav" (IN 2), 16-bit or float WAV at the test rate,
//         path relative to the script; first channel is used, silence after the end
//         optional "store": "dir" – the firmware's saved files (SDK 3) live in that folder
//         a step's "heard": ms (Protokoll 5) – the event was heard that long before it arrives (latency, a slow thread)
//         optional "daw": {"bpm":120, "at":1.0, "start":6, "loop":[4,12], "sig":[4,4]} – a DAW (SDK 8, vibe_transport): it
//         plays from "at" seconds at beat "start" (quarter notes), looping if "loop"; sends TRANSPORT tempo + PLAY too
import fs from 'fs';
import zlib from 'zlib';
const [, , wasmFile, scriptFile, outDir = '.', rateArg] = process.argv;
const script = scriptFile ? JSON.parse(fs.readFileSync(scriptFile, 'utf8')) : { seconds: 2, steps: [] };
const sr = rateArg ? +rateArg : 48000, block = 128;
let inst;
const logs = [], requests = [], files = new Map();
const storeDir = script.store ? (script.store.startsWith('/') ? script.store : scriptFile.replace(/[^/]*$/, '') + script.store) : null;
if (storeDir) { fs.mkdirSync(storeDir, { recursive: true }); for (const n of fs.readdirSync(storeDir, { recursive: true })) if (fs.statSync(storeDir + '/' + n).isFile()) files.set(n.split(/[\\/]/).join('/'), fs.readFileSync(storeDir + '/' + n)); }   // (SDK 4: folders)
inst = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(wasmFile)), { env: {
  vibe_host_log: (p, n) => logs.push(Buffer.from(inst.exports.memory.buffer, p, n).toString()),
  vibe_host_request: (w) => requests.push(w),
  // storage (SDK 3): in memory; script "store": "dir" keeps the files in a folder (a second run finds them again)
  vibe_host_store: (op, np, nl, dp, len) => {
    const name = Buffer.from(inst.exports.memory.buffer, np, nl).toString(), f = files.get(name);
    if (op === 0) { if (!f) return -1; const n = Math.min(len, f.length); new Uint8Array(inst.exports.memory.buffer, dp, n).set(f.subarray(0, n)); return n; }
    if (op === 1) { const c = Buffer.from(new Uint8Array(inst.exports.memory.buffer, dp, len)); files.set(name, c); if (storeDir) { fs.mkdirSync((storeDir + '/' + name).replace(/\/[^/]*$/, ''), { recursive: true }); fs.writeFileSync(storeDir + '/' + name, c); } return 0; }
    if (op === 2) return f ? f.length : -1;
    if (op === 3) { if (!f) return -1; files.delete(name); if (storeDir) fs.rmSync(storeDir + '/' + name, { force: true }); return 0; }
    if (op === 4) { const b = Buffer.from([...files.keys()].sort().map((k) => k + '\n').join('')); let n = 0; for (const k of [...files.keys()].sort()) { if (n + k.length + 1 > len) break; n += k.length + 1; } new Uint8Array(inst.exports.memory.buffer, dp, n).set(b.subarray(0, n)); return n; }
    if (op === 5) { if (!f) return -1; const off = new DataView(inst.exports.memory.buffer).getUint32(dp, true), n = Math.max(0, Math.min(len, f.length - off)); if (n) new Uint8Array(inst.exports.memory.buffer, dp, n).set(f.subarray(off, off + n)); return n; }
    return -1;
  } } });
const e = inst.exports;
e.vibe_wasm_init(sr);
const total = Math.ceil(script.seconds * sr / block);
const L = new Float32Array(total * block), R = new Float32Array(total * block);
const steps = (script.steps || []).slice().sort((a, b) => a.t - b.t);
let drawMs = 0, draws = 0, audioMs = 0;
const shots = [];
const here = (f) => f && (f.startsWith('/') ? f : scriptFile.replace(/[^/]*$/, '') + f);
const in1 = readWav(here(script.input)), in2 = readWav(here(script.input2));
const daw = script.daw || null;
if (daw) steps.push({ t: 0, ev: [8, 1, 0, daw.bpm || 120] }, { t: daw.at || 0, ev: [8, 0, 1, 0] }), steps.sort((a, b) => a.t - b.t);
function dawWrite(b) {   // the position table at this block's first frame (beat counted from "at", wrapped by the loop)
  if (!daw || !e.vibe_wasm_transport) return;
  const bpm = daw.bpm || 120, [num, den] = daw.sig || [4, 4], t = b * block / sr - (daw.at || 0), lp = daw.loop;
  let beat = (daw.start || 0) + Math.max(0, t) * bpm / 60;
  if (lp && beat >= lp[1] && (daw.start || 0) < lp[1]) beat = lp[0] + (beat - lp[1]) % (lp[1] - lp[0]);
  const bar = num * 4 / den, p = e.vibe_wasm_transport(), dv = new DataView(e.memory.buffer);
  dv.setInt32(p, 1, true); dv.setInt32(p + 4, t >= 0 ? 1 : 0, true); dv.setFloat32(p + 8, bpm, true); dv.setInt32(p + 12, num, true); dv.setInt32(p + 16, den, true);
  dv.setFloat64(p + 24, beat, true); dv.setFloat64(p + 32, Math.floor(beat / bar + 1e-9) * bar, true);
  dv.setInt32(p + 40, lp ? 1 : 0, true); dv.setFloat64(p + 48, lp ? lp[0] : 0, true); dv.setFloat64(p + 56, lp ? lp[1] : 0, true);
}
for (let b = 0; b < total; b++) {
  const t = b * block / sr;
  while (steps.length && steps[0].t <= t) {
    const s = steps.shift();
    if (s.ev && e.vibe_wasm_time) {   // Protokoll 5 (7.1): the frame heard when it happened – "heard": ms before now, absent: unknown
      const vt = new Uint32Array(e.memory.buffer, e.vibe_wasm_time(), 8), f = Math.max(0, b * block - Math.round((s.heard ?? 0) * sr / 1000));
      vt[0] = f >>> 0; vt[1] = Math.floor(f / 4294967296); vt[2] = 0; vt[3] = 0; vt[4] = s.heard !== undefined ? 1 : 0;
    }
    if (s.ev) e.vibe_wasm_event(s.ev[0], s.ev[1], s.ev[2], s.ev[3] ?? 0, s.ev[4] ?? 0);
    if (s.shot) shots.push(s.shot);
  }
  if (in1 || in2) {
    const io = new Float32Array(e.memory.buffer, e.vibe_wasm_io(), 256 * 4);
    if (in1) io.set(slice(in1, b * block), 0);
    if (in2) io.set(slice(in2, b * block), 768);
  }
  dawWrite(b);
  const t0 = performance.now(); e.vibe_wasm_audio(block, (in1 ? 1 : 0) | (in2 ? 2 : 0)); audioMs += performance.now() - t0;
  const io = new Float32Array(e.memory.buffer, e.vibe_wasm_io(), 256 * 3);
  L.set(io.subarray(256, 256 + block), b * block); R.set(io.subarray(512, 512 + block), b * block);
  if (b % 12 === 0 || shots.length) { const t1 = performance.now(); const fb = e.vibe_wasm_draw(); drawMs += performance.now() - t1; draws++;
    while (shots.length) png(new Uint16Array(e.memory.buffer, fb, 480 * 320), `${outDir}/${shots.shift()}.png`); }
}
let peak = 0, nan = 0, sum = 0;
for (let i = 0; i < L.length; i++) { if (!Number.isFinite(L[i]) || !Number.isFinite(R[i])) nan++; peak = Math.max(peak, Math.abs(L[i]), Math.abs(R[i])); sum += L[i] * L[i]; }
wav(`${outDir}/audio.wav`);
console.log(JSON.stringify({ peak: +peak.toFixed(3), rms: +Math.sqrt(sum / L.length).toFixed(4), nan,
  cpu_audio_percent: +(audioMs / (script.seconds * 1000) * 100).toFixed(1), draw_ms: +(drawMs / draws).toFixed(2), logs, requests }));

function png(px, file) {
  const raw = Buffer.alloc(320 * 1441);
  for (let y = 0; y < 320; y++) for (let x = 0; x < 480; x++) { const v = px[y * 480 + x], o = y * 1441 + 1 + x * 3; raw[o] = (v >> 11) << 3; raw[o + 1] = ((v >> 5) & 63) << 2; raw[o + 2] = (v & 31) << 3; }
  const tab = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; tab[n] = c >>> 0; }
  const crc = (b) => { let r = -1; for (const x of b) r = tab[(r ^ x) & 255] ^ (r >>> 8); return (r ^ -1) >>> 0; };
  const chunk = (ty, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(ty), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const ih = Buffer.alloc(13); ih.writeUInt32BE(480, 0); ih.writeUInt32BE(320, 4); ih[8] = 8; ih[9] = 2;
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
}
function wav(file) {
  if (script.float) {   // "float": true in the script – 32-bit float WAV, for measurements below 16 bit
    const n = L.length, b = Buffer.alloc(44 + n * 8);
    b.write('RIFF', 0); b.writeUInt32LE(36 + n * 8, 4); b.write('WAVEfmt ', 8); b.writeUInt32LE(16, 16); b.writeUInt16LE(3, 20); b.writeUInt16LE(2, 22); b.writeUInt32LE(sr, 24); b.writeUInt32LE(sr * 8, 28); b.writeUInt16LE(8, 32); b.writeUInt16LE(32, 34); b.write('data', 36); b.writeUInt32LE(n * 8, 40);
    for (let i = 0; i < n; i++) { b.writeFloatLE(L[i], 44 + i * 8); b.writeFloatLE(R[i], 48 + i * 8); }
    fs.writeFileSync(file, b); return;
  }
  const n = L.length, b = Buffer.alloc(44 + n * 4);
  b.write('RIFF', 0); b.writeUInt32LE(36 + n * 4, 4); b.write('WAVEfmt ', 8); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(2, 22); b.writeUInt32LE(sr, 24); b.writeUInt32LE(sr * 4, 28); b.writeUInt16LE(4, 32); b.writeUInt16LE(16, 34); b.write('data', 36); b.writeUInt32LE(n * 4, 40);
  for (let i = 0; i < n; i++) { b.writeInt16LE(Math.round(Math.max(-1, Math.min(1, L[i])) * 32767), 44 + i * 4); b.writeInt16LE(Math.round(Math.max(-1, Math.min(1, R[i])) * 32767), 46 + i * 4); }
  fs.writeFileSync(file, b);
}
function slice(a, o) { const s = new Float32Array(block); if (o < a.length) s.set(a.subarray(o, Math.min(a.length, o + block))); return s; }
function readWav(file) {
  if (!file) return null;
  const b = fs.readFileSync(file); let o = 12, fmt = null;
  while (o + 8 <= b.length) {
    const id = b.toString('ascii', o, o + 4), len = b.readUInt32LE(o + 4), d = o + 8;
    if (id === 'fmt ') fmt = { tag: b.readUInt16LE(d), ch: b.readUInt16LE(d + 2), rate: b.readUInt32LE(d + 4), bits: b.readUInt16LE(d + 14) };
    if (id === 'data') {
      const bytes = fmt.bits / 8, n = Math.floor(len / (bytes * fmt.ch)), a = new Float32Array(n);
      if (fmt.rate !== sr) console.error(`warning: ${file} is ${fmt.rate} Hz, test runs at ${sr} Hz`);
      for (let i = 0; i < n; i++) { const p = d + i * bytes * fmt.ch; a[i] = fmt.tag === 3 ? b.readFloatLE(p) : b.readInt16LE(p) / 32768; }
      return a;
    }
    o = d + len + (len & 1);
  }
  throw new Error('no data chunk in ' + file);
}

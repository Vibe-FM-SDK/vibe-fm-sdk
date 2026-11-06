// SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE.
// vibe-check – does a firmware keep the VIBE FM Protocol? Stage 1: the parts that need no
// listening – format, robustness, realtime rules, storage, report. (Block sizes and sample rates come next.)
//   node tools/vibe-check.mjs build/groove.wasm [--manifest firmware/groove/firmware.json] [--seconds 60] [--json]
// Exit code 0 = passed, 1 = failed, 2 = could not run.
import fs from 'fs';
import path from 'path';
import { execFileSync } from 'child_process';
import { MediaHost } from './media-host.mjs';

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const file = args.find((a, i) => !a.startsWith('--') && !['--manifest', '--seconds', '--files', '--store', '--script'].includes(args[i - 1]));
if (!file) { console.error('usage: node tools/vibe-check.mjs <firmware.wasm> [--manifest firmware.json] [--seconds 60] [--store DIR] [--script FILE] [--json]'); process.exit(2); }
const SECONDS = +opt('--seconds', 60), JSON_OUT = args.includes('--json'), SR = 48000, BLOCK = 128;
const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const id = path.basename(file).replace(/\.wasm$/, '');
const results = [];   // { part, ok: true | false | null (not tested), text }
const check = (part, ok, text) => results.push({ part, ok, text });

// ---------------------------------------------------------------- 1. format (sections 3, 4, 5, 14) ------------------
const bytes = fs.readFileSync(file);
let mod;
try { mod = new WebAssembly.Module(bytes); } catch (e) { check('Format', false, 'not valid WebAssembly: ' + e.message); report(); }
check('Format', bytes.length <= 4 << 20, `size ${(bytes.length / 1024).toFixed(0)} KB` + (bytes.length > 4 << 20 ? ' – over 4 MB (SHOULD NOT)' : ''));

const ALLOWED = ['vibe_host_log', 'vibe_host_request', 'vibe_host_store'];
const imports = WebAssembly.Module.imports(mod);
const badImp = imports.filter((i) => i.module !== 'env' || i.kind !== 'function' || !ALLOWED.includes(i.name));
check('Format', !badImp.length, badImp.length ? 'forbidden imports: ' + badImp.map((i) => `${i.module}.${i.name} (${i.kind})`).join(', ') : 'imports: only ' + imports.map((i) => i.name).join(', '));

const exports = new Map(WebAssembly.Module.exports(mod).map((e) => [e.name, e.kind]));
const NEED = ['vibe_sdk_version', 'vibe_wasm_init', 'vibe_wasm_event', 'vibe_wasm_io', 'vibe_wasm_audio', 'vibe_wasm_draw', 'vibe_wasm_leds'];
const missing = NEED.filter((n) => exports.get(n) !== 'function').concat(exports.get('memory') === 'memory' ? [] : ['memory']);
check('Format', !missing.length, missing.length ? 'missing exports: ' + missing.join(', ') : 'all required exports present' +
  ['vibe_wasm_rear', 'vibe_wasm_labels'].filter((n) => !exports.has(n)).map((n) => ` (without ${n})`).join(''));

// start section (id 8): a firmware must not run code on instantiation – except, from Protokoll 6, wasm-ld's one-time
// initialisation of a shared memory; memory section (id 5): flag 0x02 = shared (Protokoll 6)
let start = false, shared = false;
for (let o = 8; o < bytes.length;) {
  const sid = bytes[o++]; let len = 0, sh = 0, b;
  do { b = bytes[o++]; len |= (b & 127) << sh; sh += 7; } while (b & 128);
  if (sid === 8) start = true;
  if (sid === 5 && bytes[o] >= 1) shared = (bytes[o + 1] & 2) !== 0;   // (count, then the first memory's flags)
  o += len;
}
check('Format', !start || shared, start ? (shared ? 'start section only for setting up the shared memory (Protokoll 6)' : 'has a start section (SHOULD NOT)') : 'no start section');
check('Format', true, shared ? 'memory shared (Protokoll 6: audio may run in its own flow)' : 'memory not shared (up to Protokoll 5)');

// allowed wasm extensions: WABT's defaults minus reference types (threads, exceptions, gc, memory64, tail calls are off)
// wasm-validate: $WASM_VALIDATE if set, else wasm-validate on the PATH, else the old local WABT install
let validate, haveValidate;
if (process.env.WASM_VALIDATE) { validate = process.env.WASM_VALIDATE; haveValidate = fs.existsSync(validate); }
else {
  try { execFileSync('wasm-validate', ['--version'], { stdio: 'pipe' }); validate = 'wasm-validate'; haveValidate = true; }
  catch { validate = path.join(process.env.HOME, '.local/opt/wabt-dl/wabt-1.0.36/bin/wasm-validate'); haveValidate = fs.existsSync(validate); }
}
if (haveValidate) {
  try { execFileSync(validate, ['--disable-reference-types', ...(shared ? ['--enable-threads'] : []), file], { stdio: 'pipe' }); check('Format', true, 'only allowed WebAssembly extensions'); }
  catch (e) { check('Format', false, 'forbidden WebAssembly extension: ' + String(e.stderr || e.message).split('\n')[0]); }
} else check('Format', null, 'extensions not checked (WABT missing: ' + validate + ')');

// manifest: --manifest, else firmware.json next to the .wasm, <id>.json beside it (tools/build.sh copies it there), or
// firmware.json in <id>/, examples/<id>/ or firmware/<id>/ (relative to ROOT)
const mpath = opt('--manifest') || [path.join(path.dirname(file), 'firmware.json'), path.join(path.dirname(file), id + '.json'),
  path.join(ROOT, id, 'firmware.json'), path.join(ROOT, 'examples', id, 'firmware.json'),
  path.join(ROOT, 'firmware', id, 'firmware.json')].find((p) => fs.existsSync(p));
let manifest = null;
if (!mpath || !fs.existsSync(mpath)) check('Manifest', false, 'firmware.json not found (pass --manifest)');
else {
  try { manifest = JSON.parse(fs.readFileSync(mpath, 'utf8')); } catch (e) { check('Manifest', false, 'firmware.json is not valid JSON: ' + e.message); }
  if (manifest) {
    const fid = manifest.id || id;
    const miss = ['name', 'version', 'author', 'description'].filter((k) => !manifest[k]);
    check('Manifest', !miss.length, miss.length ? 'missing: ' + miss.join(', ') : `${manifest.name} ${manifest.version} by ${manifest.author}`);
    check('Manifest', /^[a-z0-9_-]{1,31}$/.test(fid), `ID "${fid}"` + (/^[a-z0-9_-]{1,31}$/.test(fid) ? '' : ' – only a-z 0-9 _ -, 1-31 characters'));
    check('Manifest', manifest.license ? true : null, manifest.license ? 'license ' + manifest.license : 'no license given (required for distribution)');
  }
}

// the firmware's face: icon.png next to firmware.json (or <id>.png next to the .wasm) – 16:9, PNG, 1 MB at most
{
  const ic = [mpath && path.join(path.dirname(mpath), 'icon.png'), path.join(path.dirname(file), id + '.png')].find((p) => p && fs.existsSync(p));
  if (!ic) check('Icon', null, 'no icon.png next to firmware.json – recommended (its tile in the VIBE menu, its card in the hub)');
  else {
    const b = fs.readFileSync(ic), kb = Math.ceil(b.length / 1024);
    if (b.length < 24 || b.toString('latin1', 1, 4) !== 'PNG') check('Icon', false, 'icon.png is not a PNG');
    else {
      const w = b.readUInt32BE(16), h = b.readUInt32BE(20), r = w / h;
      if (kb > 1024) check('Icon', false, `icon.png: ${kb} KB – 1024 at most`);
      else check('Icon', Math.abs(r - 16 / 9) < 0.08 ? true : null, Math.abs(r - 16 / 9) < 0.08 ? `icon.png ${w} × ${h}, ${kb} KB` : `icon.png: ${w} × ${h} – 16:9 recommended (e.g. 960 × 540)`);
    }
  }
}

// SDK 16: the firmware's own Editor page – editor/ next to firmware.json (or <id>.editor/ next to the .wasm)
{
  const dirs = [mpath && path.join(path.dirname(mpath), 'editor'), path.join(path.dirname(file), id + '.editor')].filter((d) => d && fs.existsSync(d));
  if (dirs.length) {
    const d = dirs[0], all = [], bad = [];
    const walk = (p) => { for (const e of fs.readdirSync(p, { withFileTypes: true })) { const q = path.join(p, e.name); if (e.isDirectory()) walk(q); else all.push(q); } };
    walk(d);
    const kb = Math.ceil(all.reduce((s, f) => s + fs.statSync(f).size, 0) / 1024);
    if (!fs.existsSync(path.join(d, 'index.html'))) bad.push('editor/ has no index.html');
    if (kb > 2048) bad.push(`editor/ is ${kb} KB – 2048 at most`);
    for (const f of all.filter((f) => /\.(html|js|css|svg)$/i.test(f))) {
      const s = fs.readFileSync(f, 'utf8'), rel = path.relative(d, f), u = /(?:src|href|url\()\s*=?\s*["'(]?\s*(https?:\/\/|\/\/)[^"')\s]+/i.exec(s) || /fetch\(\s*["'`]https?:/i.exec(s);
      if (u) bad.push(`${rel}: an outside address (${u[0].slice(0, 40)}) – the page has no network`);
      if (/\beval\s*\(|new\s+Function\s*\(/.test(s)) bad.push(`${rel}: eval / new Function – blocked in the sandbox`);
    }
    check('Editor page', !bad.length, bad.length ? bad.slice(0, 3).join('; ') : `${all.length} files, ${kb} KB, no outside addresses`);
  }
}
if (badImp.length || missing.length) report();

// ---------------------------------------------------------------- a host that watches ----------------------------------
const NAME = /^(?:[a-z0-9_-][a-z0-9._-]{0,30}|(?=.{1,127}$)[a-z0-9_-][a-z0-9._-]{0,62}(?:\/[a-z0-9_-][a-z0-9._-]{0,62}){1,4})$/;   // SDK 4: paths
// SDK 13: the media a host offers – the card (writes go there) and a stick with UTF-8 names, folders and a big file; the
// stick is pulled halfway through the random run and comes back later (a firmware must get -1, never crash)
const STICK = () => { const big = Buffer.alloc(2 << 20); for (let i = 0; i < big.length; i++) big[i] = (i * 7 + (i >> 9)) & 255;
  return { 'Grüße – Ä.wav': big, 'b.mp3': Buffer.from('ID3hello'), 'Drums/Kicks/k1.wav': Buffer.alloc(4000), 'Drums/s1.wav': Buffer.alloc(100), 'Ünïcödé 日本/x.txt': Buffer.from('x') }; };
const mediaHost = () => new MediaHost({ fw: id, pending: 2, sources: { sd: { files: {}, write: true, label: 'VIBE' }, 'usb-CHECK': { files: STICK(), usb: true, label: 'STICK' } } });
function host(files) {
  const h = { files, inAudio: false, media: mediaHost(), mediaAudio: 0, audioStore: 0, audioLog: 0, logs: [], requests: [], badNames: 0, reads: 0, writes: 0, inst: null, midi: { n: 0, bad: 0, clock: 0 } };
  const mem = () => h.inst.exports.memory.buffer, str = (p, n) => Buffer.from(mem(), p, n).toString();
  const inJob = () => { const x = h.inst && h.inst.exports; if (x && x.vibe_wasm_in_job && x.vibe_wasm_in_job()) h.jobCalls = (h.jobCalls || 0) + 1; };   // (Protokoll 8: no host call from a job)
  h.inst = new WebAssembly.Instance(mod, { env: {
    vibe_host_log: (p, n) => { inJob(); if (h.inAudio) h.audioLog++; if (h.logs.length < 50) h.logs.push(str(p, n)); },
    vibe_host_request: (w) => { inJob(); h.requests.push(w); },
    vibe_host_store: (op, np, nl, dp, len) => { inJob();
      if (h.inAudio) h.audioStore++;
      if (op >= 6) { if (h.inAudio) h.mediaAudio++; return h.media.store(op, mem(), np, nl, dp, len); }   // SDK 13
      if (op === 4) { const b = Buffer.from([...files.keys()].sort().map((k) => k + '\n').join('')); let n = 0; for (const k of [...files.keys()].sort()) { if (n + k.length + 1 > len) break; n += k.length + 1; } new Uint8Array(mem(), dp, n).set(b.subarray(0, n)); return n; }   // SDK 4: list
      const name = str(np, nl); if (!NAME.test(name)) { h.badNames++; return -1; }
      const f = files.get(name);
      if (op === 0) { h.reads++; if (!f) return -1; const n = Math.min(len, f.length); new Uint8Array(mem(), dp, n).set(f.subarray(0, n)); return n; }
      if (op === 1) { h.writes++; files.set(name, Buffer.from(new Uint8Array(mem(), dp, len))); return 0; }
      if (op === 2) return f ? f.length : -1;
      if (op === 3) { if (!f) return -1; files.delete(name); return 0; }
      if (op === 5) { h.reads++; if (!f) return -1; const off = new DataView(mem()).getUint32(dp, true), n = Math.max(0, Math.min(len, f.length - off)); if (n) new Uint8Array(mem(), dp, n).set(f.subarray(off, off + n)); return n; }   // SDK 4: read at
      return -1;
    } } });
  return h;
}
const last = [];   // the last events before a crash, for the report
let seed = 0x2545F491; const rnd = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; return (seed >>> 0) / 4294967296; };
const pick = (n) => Math.floor(rnd() * n);

// --map build/<id>.map (wasm-ld's): a NaN hunt – the adapter's ring (vb_l, vb_r) watched after every call; the first call
// that leaves a NaN in it is named, with the data just below the NaN (an array written past its end is the usual cause)
const MAP = opt('--map') && fs.existsSync(opt('--map')) ? fs.readFileSync(opt('--map'), 'utf8').split('\n').map((l) => l.match(/^\s+([0-9a-f]+)\s+[0-9a-f-]+\s+([0-9a-f]+)\s{9,}(\S.*)$/)).filter(Boolean)
  .map((m) => ({ at: parseInt(m[1], 16), size: parseInt(m[2], 16), name: m[3].trim() })).filter((s) => !s.name.includes('(')).sort((a, b) => a.at - b.at) : null;
const WATCH = MAP ? MAP.filter((s) => s.name === 'vb_l' || s.name === 'vb_r') : [];
let hunted = null;
function hunt(x, what) {
  if (hunted || !WATCH.length) return;
  for (const w of WATCH) {
    const f = new Float32Array(x.memory.buffer, w.at, w.size / 4);
    for (let i = 0; i < f.length; i++) if (!Number.isFinite(f[i])) {
      const a = w.at + i * 4, near = MAP.filter((s) => s.at + s.size <= a && s.name !== w.name).slice(-4).map((s) => `${s.name} (0x${s.at.toString(16)}+${s.size})`);
      hunted = `NaN hunt: ${w.name}[${i}] (0x${a.toString(16)}) after ${what} – the data just before it: ${near.join(', ')}`; return;
    }
  }
}
// SDK 9 (Protocol 7.4): the firmware's MIDI out queue, drained after a block as a host does – bytes checked, counters kept
function midiDrain(h, x) {
  if (!x.vibe_wasm_midi_out) return;
  const base = x.vibe_wasm_midi_out(), dv = new DataView(x.memory.buffer), w = dv.getUint32(base, true);
  let r = dv.getUint32(base + 4, true);
  for (; r !== w; r = (r + 1) >>> 0) {
    const q = base + 16 + (r % 256) * 6, st = dv.getUint8(q + 2), d1 = dv.getUint8(q + 3), d2 = dv.getUint8(q + 4), len = dv.getUint8(q + 5), frame = dv.getUint16(q, true);
    h.midi.n++;
    if (!len && st === 0xF0) { const n = dv.getUint32(base + 12, true); if (n < 2 || n > 256) h.midi.bad++; dv.setUint32(base + 12, 0, true); continue; }
    if (st < 0x80 || len < 1 || len > 3 || (len > 1 && d1 > 127) || (len > 2 && d2 > 127) || frame >= BLOCK) h.midi.bad++;
    if (st === 0xF8) h.midi.clock++;
  }
  dv.setUint32(base + 4, w, true);
}
// one run: init, then blocks with events in between, a draw every 12 blocks. Returns what happened.
function run(h, seconds, fuzz) {
  const x = h.inst.exports, out = { trap: null, nan: 0, ms: [], grew: false, peak: 0, version: 0 };
  try {
    out.version = x.vibe_sdk_version();
    h.inAudio = false; x.vibe_wasm_init(SR);
    if (out.version >= 5 && x.vibe_wasm_flows) out.flows = x.vibe_wasm_flows();
    const mem0 = x.memory.buffer.byteLength, io = x.vibe_wasm_io();
    const blocks = Math.ceil(seconds * SR / BLOCK), held = [];
    for (let b = 0; b < blocks; b++) {
      if (fuzz && b === Math.floor(blocks / 2)) h.media.remove('usb-CHECK');                       // (SDK 13: the stick pulled …
      if (fuzz && b === Math.floor(blocks * 3 / 4)) h.media.add('usb-CHECK', { files: STICK(), usb: true, label: 'STICK' });   // … and back)
      for (const m of h.media.events()) { last.push([12, m.id, m.down, m.value, 0]); if (last.length > 6) last.shift(); x.vibe_wasm_event(12, m.id, m.down, m.value, 0); }
      if (fuzz) for (let k = pick(3); k > 0; k--) { fuzzEvent(x, held); hunt(x, 'event ' + JSON.stringify(last[last.length - 1])); }
      if (fuzz && rnd() < 0.05) { const io4 = new Float32Array(x.memory.buffer, io, 1024); for (let i = 0; i < BLOCK; i++) { io4[i] = rnd() * 2 - 1; io4[768 + i] = rnd() * 2 - 1; } }
      const has = fuzz && out.version >= 2 ? pick(4) : 0;
      h.inAudio = true; const t0 = performance.now(); x.vibe_wasm_audio(BLOCK, has); out.ms.push(performance.now() - t0); h.inAudio = false; hunt(x, `audio block ${b}`);
      midiDrain(h, x);
      const f = new Float32Array(x.memory.buffer, io, 768);
      for (let i = 0; i < BLOCK; i++) { const l = f[256 + i], r = f[512 + i]; if (!Number.isFinite(l) || !Number.isFinite(r)) { if (!out.nan++) { out.nanAt = b; out.nanLast = last.slice(); out.nanIn = has; } } else out.peak = Math.max(out.peak, Math.abs(l), Math.abs(r)); }
      if (b % 12 === 0) { x.vibe_wasm_draw(); hunt(x, `draw (block ${b})`); x.vibe_wasm_leds(); hunt(x, `leds (block ${b})`); }
    }
    out.grew = x.memory.buffer.byteLength !== mem0;
  } catch (e) { out.trap = String(e && e.message || e); out.stack = String(e && e.stack || ""); h.inAudio = false; }
  return out;
}
// a played note, once with ~30 draws a second and once with none (Protokoll 9: a host may stop drawing – the plugin
// does while its window is closed): returns the level of the sound after the note
function playRun(h, draw) {
  const x = h.inst.exports;
  try {
    h.inAudio = false; x.vibe_wasm_init(SR);
    const io = x.vibe_wasm_io(), blocks = Math.ceil(4 * SR / BLOCK); let sum = 0, n = 0;
    for (let b = 0; b < blocks; b++) {
      const t = b * BLOCK / SR;
      if (Math.abs(t - 2.5) < BLOCK / SR / 2) { x.vibe_wasm_event(2, 12, 1, 0.8, 0); x.vibe_wasm_event(5, 0, 0, 0, 0x90 | 60 << 8 | 100 << 16); }
      if (Math.abs(t - 3.5) < BLOCK / SR / 2) { x.vibe_wasm_event(2, 12, 0, 0, 0); x.vibe_wasm_event(5, 0, 0, 0, 0x80 | 60 << 8); }
      h.inAudio = true; x.vibe_wasm_audio(BLOCK, 0); h.inAudio = false;
      if (t >= 2.5) { const f = new Float32Array(x.memory.buffer, io, 768); for (let i = 0; i < BLOCK; i++) { sum += f[256 + i] * f[256 + i]; n++; } }
      if (draw && b % 12 === 0) x.vibe_wasm_draw();
    }
    return Math.sqrt(sum / Math.max(1, n));
  } catch (e) { return -1; }
}
// Protokoll 3.2 (3.11.2026): a button's state is only `down`, its value always 0 – the VIBE's buttons pressed one by one
// (F1-F6, K1-K6, MENU, ◀ ▶, REC STOP PLAY) with this value: how many of them changed the display (S1VAVE 1.12 tested
// e->value – no button reacted in the browser or on the VIBE, only through a controller profile's 0.9)
function buttonRun(h, value) {
  const x = h.inst.exports;
  try {
    h.inAudio = false; x.vibe_wasm_init(SR);
    const blocks = (n) => { for (let b = 0; b < n; b++) { h.inAudio = true; x.vibe_wasm_audio(BLOCK, 0); h.inAudio = false; } };
    const pic = () => { const p = x.vibe_wasm_draw(), u = new Uint32Array(x.memory.buffer, p, 480 * 320 / 2); let k = 0; for (let i = 0; i < u.length; i++) k = (k * 31 + u[i]) >>> 0; return k; };
    blocks(Math.ceil(2 * SR / BLOCK)); let last = pic(), changed = 0;
    for (const id of [0, 1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15, 19, 18, 20, 21, 22, 23]) {
      x.vibe_wasm_event(3, id, 1, value, 0); blocks(8); x.vibe_wasm_event(3, id, 0, 0, 0); blocks(24);
      const now = pic(); if (now !== last) changed++; last = now;
    }
    return changed;
  } catch (e) { return -1; }
}
// (3.11.2026, Protocol 3.2) HOME tapped: the firmware shows what every control does – it must not ask for the library
// (HOME held 1 s / SHIFT+HOME, the host does that); { lib: asked for it, changed: the display changed }
function homeRun(h) {
  const x = h.inst.exports;
  try {
    h.inAudio = false; x.vibe_wasm_init(SR);
    const blocks = (n) => { for (let b = 0; b < n; b++) { h.inAudio = true; x.vibe_wasm_audio(BLOCK, 0); h.inAudio = false; } };
    const pic = () => { const p = x.vibe_wasm_draw(), u = new Uint32Array(x.memory.buffer, p, 480 * 320 / 2); let k = 0; for (let i = 0; i < u.length; i++) k = (k * 31 + u[i]) >>> 0; return k; };
    blocks(Math.ceil(2 * SR / BLOCK)); const before = pic(); h.requests.length = 0;
    x.vibe_wasm_event(3, 17, 1, 0, 0); blocks(8); x.vibe_wasm_event(3, 17, 0, 0, 0); blocks(24);
    return { lib: h.requests.includes(1), changed: pic() !== before };
  } catch (e) { return null; }
}
// random events of every type, out-of-range ids, unknown types, releases without a press (section 7)
function fuzzEvent(x0, held) {
  const x = { vibe_wasm_event: (...a) => { last.push(a); if (last.length > 6) last.shift(); x0.vibe_wasm_event(...a); } };
  const r = rnd();
  if (r < 0.04) { x.vibe_wasm_event(8 + pick(250), pick(256), pick(2), rnd() * 4 - 2, pick(1 << 24)); return; }   // unknown type
  if (r < 0.08) { x.vibe_wasm_event(1 + pick(7), 25 + pick(230), pick(2), rnd(), 0); return; }                  // unknown id
  if (r < 0.12) { x.vibe_wasm_event(1 + pick(3), pick(16), 0, 0, 0); return; }                                   // release, never pressed
  if (r < 0.30 && held.length) { const [t, i] = held.splice(pick(held.length), 1)[0]; x.vibe_wasm_event(t, i, 0, 0, 0); return; }
  const t = [1, 2, 3, 4, 4, 5, 6, 7][pick(8)];
  if (t === 1 || t === 2) { const i = pick(t === 1 ? 16 : 25); x.vibe_wasm_event(t, i, 1, 0.05 + rnd() * 0.95, 0); held.push([t, i]); }
  else if (t === 3) { const i = pick(25); if (i === 17 && rnd() < 0.9) return; x.vibe_wasm_event(3, i, 1, 0, 0); held.push([3, i]); }   // (HOME seldom: the menu)
  else if (t === 4) x.vibe_wasm_event(4, pick(7), 0, (rnd() < 0.8 ? (pick(2) ? 1 : -1) : rnd() * 8 - 4), 0);
  else if (t === 5) { const st = [0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0][pick(7)] | pick(16); x.vibe_wasm_event(5, 0, 0, 0, st | pick(128) << 8 | pick(128) << 16); }
  else if (t === 6) { const i = pick(2); x.vibe_wasm_event(6, i, i ? 0 : pick(2), rnd(), 0); }
  else x.vibe_wasm_event(7, 0, 0, 0.1 + rnd() * 0.2, 0);
}

// ---------------------------------------------------------------- 2. robustness + 3. realtime -------------------------
const files = new Map();   // --files dir: the firmware's storage starts with these (music for PLAYER, samples …)
const STORE = opt('--files') || opt('--store');   // (--store DIR: a saved state – the jobs check plays with it too)
if (STORE) for (const n of fs.readdirSync(STORE, { recursive: true })) { const k = n.split(path.sep).join('/'), f = path.join(STORE, n); if (fs.statSync(f).isFile() && NAME.test(k)) files.set(k, fs.readFileSync(f)); }
const h1 = host(files), r1 = run(h1, SECONDS, true);
check('Version', r1.version >= 1 && r1.version <= 8, `Protokoll ${r1.version}` + (manifest && manifest.protocol && manifest.protocol !== r1.version ? ` – manifest says ${manifest.protocol}` : ''));
if (shared && r1.version < 6) check('Format', false, 'shared memory requires Protokoll 6 or later');
if (r1.version >= 5) {   // Protokoll 5: the time table and the flows (6.2, 7.1)
  const miss5 = ['vibe_wasm_time', 'vibe_wasm_flows'].filter((n) => exports.get(n) !== 'function');
  check('Format', !miss5.length, miss5.length ? 'version 5 without ' + miss5.join(', ') : `time table present · flows: ${r1.flows === 2 ? '2 (parallel audio allowed)' : '1'}`);
  if (!miss5.length) check('Format', r1.flows === 1 || r1.flows === 2, `vibe_wasm_flows = ${r1.flows}`);
}
if (manifest && manifest.protocol && manifest.protocol !== r1.version) check('Manifest', false, `"protocol": ${manifest.protocol}, the firmware reports ${r1.version}`);
if (r1.trap && process.env.VIBE_STACK) console.error(r1.stack);
{ // Protokoll 9: nothing time-critical in draw – a note must sound the same whether the host draws or not
  const withDraw = playRun(host(new Map()), true), noDraw = playRun(host(new Map()), false);
  if (withDraw > 0.001) check('Flow', noDraw > withDraw * 0.3, noDraw > withDraw * 0.3 ? 'plays even when the host does not draw (plugin with its window closed)'
    : `plays only while drawing (level ${withDraw.toFixed(3)} with, ${noDraw.toFixed(3)} without draw) – time-critical work does not belong in vibe_on_draw (section 9)`);
  else check('Flow', null, 'no sound on key/MIDI note – "plays without draw" not checked');
}
{ // Protocol 3.2: HOME tapped = what every control does, never the library
  const hr = homeRun(host(new Map()));
  if (hr) check('HOME', !hr.lib && hr.changed ? true : hr.lib ? false : null, hr.lib ? 'HOME tapped opens the library – the host does that (HOME held 1 s / SHIFT+HOME). HOME tapped SHOULD show what every key, knob and pad on this page does: vg_controls() draws it in one call (Protocol 3.2)'
    : hr.changed ? 'HOME tapped shows something (the controls)' : 'HOME tapped does not change the display – it SHOULD show the controls (vg_controls, Protocol 3.2)');
}
{ // Protokoll 3.2: buttons by `down` alone – a host sends value 0 with every button
  const n0 = buttonRun(host(new Map()), 0), n9 = buttonRun(host(new Map()), 0.9);
  if (n0 > 0) check('Events', true, `buttons: ${n0} of 18 change the display (pressed = e->down)`);
  else if (n9 > 0) check('Events', false, `no button reacts – only with value > 0 (${n9} of 18). For VIBE_EV_BUTTON pressed is only e->down, value is always 0: if (e->down) instead of if (e->value > 0) (Protocol 3.2)`);
  else if (n0 === 0) check('Events', null, 'no button changes the display – on purpose? (checked: F1–F6, K1–K6, MENU, ◀ ▶, REC, STOP, PLAY)');
}

// Protokoll 8: worker jobs – the same, bit for bit, in any order (a job that leans on another's memory sounds different
// in parallel, or crashes), no host call from a job, the time each job takes
// --script FILE: a wasmtest script ("steps": [{"t": s, "ev": [type, id, down, value, midi]}], "seconds") plays instead of the chords
const SCRIPT_J = opt('--script') ? JSON.parse(fs.readFileSync(opt('--script'), 'utf8')) : null, SCRIPT = SCRIPT_J && (SCRIPT_J.steps || []).slice().sort((a, b) => a.t - b.t), SCRIPT_S = SCRIPT_J && SCRIPT_J.seconds;
if (exports.get('vibe_wasm_workers') === 'function' && exports.get('vibe_wasm_jobs_reverse') === 'function') {
  const play = (rev, timeJobs) => {
    const hh = host(new Map(files)), x = hh.inst.exports; let out = [], ms = [[], [], []];   // (with --store: the saved state – tracks that use the jobs)
    try {
      x.vibe_wasm_init(SR); x.vibe_wasm_jobs_reverse(rev);
      const io = x.vibe_wasm_io(), nj = x.vibe_wasm_workers(); let si = 0;
      for (let b = 0; b < Math.ceil((SCRIPT_S || 4) * SR / BLOCK); b++) {
        if (SCRIPT) { while (si < SCRIPT.length && SCRIPT[si].t * SR <= b * BLOCK) { const ev = SCRIPT[si++].ev; if (ev) x.vibe_wasm_event(ev[0], ev[1], ev[2] || 0, ev[3] || 0, ev[4] || 0); } }
        else if (b % 40 === 2) for (let v = 0; v < 4; v++) { x.vibe_wasm_event(5, 0, 0, 0, 0x90 | v | (48 + v * 7 + (b / 40 | 0) % 5) << 8 | 100 << 16); x.vibe_wasm_event(2, (v * 5 + b / 40) % 25 | 0, 1, 0.8, 0); }
        if (!SCRIPT && b % 40 === 30) for (let v = 0; v < 4; v++) { x.vibe_wasm_event(5, 0, 0, 0, 0x80 | v | (48 + v * 7 + (b / 40 | 0) % 5) << 8); x.vibe_wasm_event(2, (v * 5 + b / 40) % 25 | 0, 0, 0, 0); }
        hh.inAudio = true; x.vibe_wasm_audio(BLOCK, 0); hh.inAudio = false;
        const f = new Float32Array(x.memory.buffer, io, 768); out.push(Float32Array.from(f.subarray(256, 256 + BLOCK)), Float32Array.from(f.subarray(512, 512 + BLOCK)));
        if (timeJobs) for (let k = 0; k < nj; k++) { const t0 = performance.now(); x.vibe_wasm_job(k, BLOCK); ms[k].push(performance.now() - t0); }
      }
      return { n: nj, out, ms, calls: hh.jobCalls || 0 };
    } catch (e) { return { n: 0, trap: String(e && e.message || e), calls: hh.jobCalls || 0 }; }
  };
  const a = play(0, false), b = play(1, false), tm = play(0, true), n = a.n;
  if (n > 0) {
    if (a.trap || b.trap) { const e = a.trap || b.trap; check('Jobs', false, `crash in a job: ${e}`); }
    else {
      let diff = 0, loud = 0; for (let i = 0; i < a.out.length; i++) for (let j = 0; j < a.out[i].length; j++) { if (!Object.is(a.out[i][j], b.out[i][j])) diff++; loud = Math.max(loud, Math.abs(a.out[i][j])); }
      const calls = a.calls + b.calls;
      const t = tm.ms.slice(0, n).map((m, k) => `${k}: ${(m.reduce((s, v) => s + v, 0) / Math.max(1, m.length)).toFixed(3)} / ${Math.max(0, ...m).toFixed(3)} ms`).join(' · ');
      if (diff) check('Jobs', false, `the result depends on the order of the jobs (${n} samples differ) – a job reads or writes what another one writes: in parallel it sounds different or crashes`.replace('${n}', diff));
      else if (calls) check('Jobs', false, `${n}× a host call from a job (vibe_* in a job: MUST NOT – log, storage, media, MIDI …)`.replace('${n}', calls));
      else check('Jobs', loud > 1e-6 ? true : null, loud > 1e-6 ? `${n} jobs: bit-identical in any order, no host call · time per job (mean / peak): ${t}` : `${n} jobs – rendered silent, bit-identity says little`);
    }
  }
}
check("Robustness", !r1.trap, r1.trap ? `crash after ${r1.ms.length} blocks (${(r1.ms.length * BLOCK / SR).toFixed(1)} s): ${r1.trap} – last events [type,id,down,value,midi]: ${last.map((a) => JSON.stringify(a.map((v) => +(+v).toFixed(2)))).join(' ')}` : `${SECONDS} s of random events without a crash`);
check('Robustness', !r1.nan, r1.nan ? `${r1.nan} samples NaN/infinite, first after ${(r1.nanAt * BLOCK / SR).toFixed(2)} s (input ${r1.nanIn ? 'on' : 'off'}) – last events [type,id,down,value,midi]: ${(r1.nanLast || []).map((a) => JSON.stringify(a.map((v) => +(+v).toFixed(2)))).join(' ')}` : 'no NaN, no infinity' + (r1.peak > 1 ? ` (peak ${r1.peak.toFixed(2)} above 1 – SHOULD stay below)` : ''));
check('Realtime', !h1.audioStore, h1.audioStore ? `${h1.audioStore}× vibe_host_store in the audio path (MUST NOT)` : 'no storage calls in the audio path');
check('Realtime', h1.audioLog ? null : true, h1.audioLog ? `${h1.audioLog}× vibe_host_log in the audio path (SHOULD NOT)` : 'no logging in the audio path');
check('Realtime', !r1.grew, r1.grew ? 'memory grows after init (MUST NOT)' : 'memory stays the same after init');
const ms = r1.ms.slice(Math.floor(r1.ms.length * 0.02)).sort((a, b) => a - b), budget = BLOCK / SR * 1000;
{ // the start: the first 3 s of blocks – a block over its time there is a crackle on the device every time the firmware loads
  // (measured in a run of its own without events: a key the random run pressed in those 3 s is not the start)
  // (up to three times, the best one counts: a shared machine's hiccup is not the firmware's – one that is slow
  //  every time is)
  let over = Infinity, bmax = 0;
  for (let k = 0; k < 3 && over > 0; k++) {
    const rq = run(host(files), 3, false), boot = (rq.trap ? r1.ms : rq.ms).slice(0, Math.ceil(3 * SR / BLOCK));
    const o = boot.filter((t) => t > budget).length, m = Math.max(0, ...boot);
    if (o < over || (o === over && m < bmax)) { over = o; bmax = m; }
  }
  check('Realtime', over ? false : true, over ? `at start ${over} blocks over their time budget (longest ${(bmax / budget * 100).toFixed(0)} %) – crackles/dropouts on the device while loading (move heavy work into vibe_on_init)`
    : `start without a block over its time budget (longest ${(bmax / budget * 100).toFixed(0)} %)`);
}
if (ms.length) {
  const mean = ms.reduce((a, b) => a + b, 0) / ms.length, p999 = ms[Math.floor(ms.length * 0.999)], max = ms[ms.length - 1];
  check('CPU', null, `mean ${(mean / budget * 100).toFixed(1)} % · 99.9th percentile ${(p999 / budget * 100).toFixed(1)} % · peak ${(max / budget * 100).toFixed(0)} % of a 128-frame block (Node/V8 on this machine; device limit to follow)`);
}
{ // SDK 13: media – only from the control flow, clean paths, the pulled stick survived (Robustness above)
  const c = h1.media.count, used = c.sources + c.list + c.stat + c.read + c.create;
  if (!used) check('Media', null, 'uses no media (vibe_media_*)');
  else {
    check('Media', !h1.mediaAudio, h1.mediaAudio ? `${h1.mediaAudio}× vibe_media_* from the audio (MUST NOT)` : 'media only from events and draw');
    check('Media', !c.badPath, c.badPath ? `${c.badPath}× an invalid path or name` : 'paths valid');
    check('Media', null, `${c.sources}× sources · ${c.list}× folders · ${c.read}× read (${(c.bytesRead / 1024).toFixed(0)} KB, ${c.pending}× answered "not yet") · ${c.written.length} files written · stick pulled after ${(SECONDS / 2).toFixed(0)} s, ${c.gone}× -1 after that`);
    if (h1.media.handles.size) check('Media', null, `${h1.media.handles.size} file(s) still open at the end`);
  }
}
check('Storage', !h1.badNames, h1.badNames ? `${h1.badNames}× invalid file name` : 'file names valid');

// ---------------------------------------------------------------- 4. storage: saved files survive a restart -----------
if (r1.version < 3) check('Storage', null, 'Protokoll < 3: no storage');
else if (!files.size) check('Storage', null, `not checked – the firmware saved nothing in ${SECONDS} s`);
else {
  const h2 = host(new Map(files)), r2 = run(h2, 3, false);
  check('Storage', !r2.trap && !r2.nan, r2.trap ? 'crash on start with its own files: ' + r2.trap : `restart with ${files.size} own files (${[...files.values()].reduce((a, f) => a + f.length, 0)} bytes) without errors, ${h2.reads} read`);
}
// ---------------------------------------------------------------- SDK 9: MIDI out and the Editor's description ---------
if (exports.get('vibe_wasm_midi_out') === 'function') {
  const base = h1.inst.exports.vibe_wasm_midi_out(), dropped = new DataView(h1.inst.exports.memory.buffer).getUint32(base + 8, true);
  if (!h1.midi.n && !dropped) check('MIDI out', null, 'nothing sent during the random playing');
  else {
    check('MIDI out', !dropped, dropped ? `${dropped} messages dropped – the queue (256 a block) overflowed` : `${h1.midi.n} messages, none dropped` + (h1.midi.clock ? ` (${h1.midi.clock} clock)` : ''));
    check('MIDI out', !h1.midi.bad, h1.midi.bad ? `${h1.midi.bad} bad messages (status < 0x80, a data byte >= 0x80 or a frame outside the block)` : 'all messages valid');
  }
}
if (exports.get('vibe_wasm_editor') === 'function') {
  const x = h1.inst.exports, ed = x.vibe_wasm_editor();
  if (!ed) check('Editor', null, 'no Editor description (one card per group, as in SDK 8)');
  else {
    const dv = new DataView(x.memory.buffer), u8 = new Uint8Array(x.memory.buffer);
    const cstr = (p) => { if (!p) return null; let n = 0; while (n < 256 && u8[p + n]) n++; return Buffer.from(x.memory.buffer, p, n).toString(); };
    const cards = dv.getUint32(ed + 4, true), ncards = dv.getInt32(ed + 8, true), ctl = dv.getUint32(ed + 12, true), tabs = dv.getUint32(ed + 20, true), ntabs = dv.getInt32(ed + 24, true), help = dv.getUint32(ed + 28, true), ncurves = dv.getInt32(ed + 32, true);
    const np = x.vibe_wasm_param_count ? x.vibe_wasm_param_count() : 0, groups = [];
    for (let i = 0; i < np; i++) { const g = x.vibe_wasm_param_group ? cstr(x.vibe_wasm_param_group(i)) || '' : ''; const k = g.indexOf(' · '); groups.push({ tab: k > 0 ? g.slice(0, k) : '', card: (k > 0 ? g.slice(k + 3) : g) || 'PARAMETERS' }); }
    const known = new Set(groups.map((g) => g.card)), bad = [];
    for (let i = 0; i < ncards; i++) { const c = cstr(dv.getUint32(cards + i * 12, true)), w = dv.getInt32(cards + i * 12 + 4, true); if (!c || c.length > 40) bad.push(`card ${i}: no name`); else if (!known.has(c)) bad.push(`card "${c}" has no parameters`); if (w < 0 || w > 12) bad.push(`card "${c}": width ${w} (0..12)`); }
    check('Editor', ncards >= 0 && ncards <= 64 && !bad.length, bad.length ? bad.slice(0, 3).join('; ') : `${ncards} cards in your order`);
    if (ctl) { const xy = []; for (let i = 0; i < np; i++) { const c = u8[ctl + i]; if (c > 6) xy.push(`parameter ${i}: control ${c}`); if (c === 5 && (i + 1 >= np || u8[ctl + i + 1] !== 6 || groups[i].card !== groups[i + 1].card || groups[i].tab !== groups[i + 1].tab)) xy.push(`parameter ${i}: XY_X without XY_Y on the same card right after it`); if (c === 4) { const inf = x.vibe_wasm_param_info(i); if (!(dv.getFloat32(inf + 8, true) < 0 && dv.getFloat32(inf + 12, true) > 0)) xy.push(`parameter ${i}: BIPOLAR, but min/max not around 0`); } }
      check('Editor', !xy.length, xy.length ? xy.slice(0, 3).join('; ') : 'controls valid (XY pairs, bipolar around 0)'); }
    if (tabs) { const bt = []; for (let i = 0; i < Math.min(ntabs, 8); i++) { const t = cstr(dv.getUint32(tabs + i * 8, true)); if (t == null || !groups.some((g) => g.tab === t)) bt.push(`tab "${t}" does not exist`); } check('Editor', !bt.length && ntabs <= 8, bt.length ? bt.join('; ') : `${ntabs} tab colours`); }
    if (help) { let long = 0; for (let i = 0; i < np; i++) { const t = cstr(dv.getUint32(help + i * 4, true)); if (t && t.length > 120) long++; } check('Editor', !long, long ? `${long} help texts over 120 characters` : 'help texts within length'); }
    if (ncurves) { const bc = []; let slow = 0; for (let i = 0; i < Math.min(ncurves, 16); i++) { const c = cstr(x.vibe_wasm_editor_curve_card(i)); if (!known.has(c)) bc.push(`curve for "${c}": no such card`); x.vibe_wasm_editor_curve(i, 0); const t0 = performance.now(), n = x.vibe_wasm_editor_curve(i, 0);   /* (once before: the first call compiles) */ if (performance.now() - t0 > 1) slow++; if (n < 2 || n > 64) bc.push(`curve "${c}": ${n} points (2..64)`); }
      check('Editor', !bc.length && !slow && ncurves <= 16, bc.length ? bc.join('; ') : slow ? `${slow} curves take over 1 ms` : `${ncurves} curves, 2..64 points, under 1 ms`); }
    // SDK 15: a curve to draw – every point once, then the curve still 2..64 points; buttons: a label of 1..15 characters
    if (ncurves && x.vibe_wasm_editor_curve_drawable) { let nd = 0, slow = 0; const bd = [];
      for (let i = 0; i < Math.min(ncurves, 16); i++) { if (!x.vibe_wasm_editor_curve_drawable(i)) continue; nd++;
        const c = cstr(x.vibe_wasm_editor_curve_card(i)), n = x.vibe_wasm_editor_curve(i, 0);
        for (let k = 0; k < n; k++) { const t0 = performance.now(); x.vibe_wasm_editor_curve_draw(i, 0, k, n, (k % 3) / 2); if (performance.now() - t0 > 1) slow++; }
        const m = x.vibe_wasm_editor_curve(i, 0); if (m < 2 || m > 64) bd.push(`curve "${c}" after drawing: ${m} points`); }
      if (nd) check('Editor', !bd.length && !slow, bd.length ? bd.join('; ') : slow ? `drawing: ${slow} points over 1 ms` : `${nd} curves to draw, every point under 1 ms`); }
    if (x.vibe_wasm_editor_buttons && x.vibe_wasm_editor_buttons()) { const nb = x.vibe_wasm_editor_buttons(), bb = [];
      for (let i = 0; i < Math.min(nb, 16); i++) { const q = x.vibe_wasm_editor_button(i), c = cstr(dv.getUint32(q, true)), l = cstr(dv.getUint32(q + 4, true));
        if (!c) bb.push(`button ${i}: no card`); if (!l || l.length > 15) bb.push(`button ${i}: label of ${l ? l.length : 0} characters (1..15)`); }
      check('Editor', !bb.length && nb <= 16, bb.length ? bb.slice(0, 3).join('; ') : `${nb} buttons`); }
  }
}
{ // SDK 23: the stage (a second, bigger picture the firmware draws) – size and manifest match, 60 frames under 10 ms each,
  // its events taken, and the same sound whether the stage is drawn or not (a host may never show it)
  const h = host(new Map(files)), x = h.inst.exports;
  if (x.vibe_wasm_stage_info && x.vibe_wasm_stage_draw) {
    try {
      h.inAudio = false; x.vibe_wasm_init(SR);
      const q = x.vibe_wasm_stage_info(), dv = new DataView(x.memory.buffer), w = dv.getUint16(q, true), hh = dv.getUint16(q + 2, true), want = dv.getUint8(q + 8), repl = dv.getUint8(q + 9) & 1;
      if (want) {
        const okSize = w >= 480 && hh >= 270 && w <= 1280 && hh <= 720;
        check('Stage', okSize, okSize ? `${w} x ${hh}${repl ? ' – replaces the device (VIBE_STAGE_REPLACES)' : ''}` : `${w} x ${hh} – allowed: 480 x 270 up to 1280 x 720`);
        const ms = manifest && manifest.stage;
        check('Manifest', !!ms && ms.w === w && ms.h === hh, ms ? (ms.w === w && ms.h === hh ? '"stage" matches the firmware' : `"stage": ${ms.w} x ${ms.h}, the firmware asks for ${w} x ${hh}`) : '"stage" missing in firmware.json (the hosts show the badge from it)');
        x.vibe_wasm_event(19, 8, 0, 0, 0);
        let slow = 0, max = 0, p = 0;
        for (let i = 0; i < 60; i++) {
          if (i % 10 === 3) { const c = Math.floor(hh / 2) * 4096 + Math.floor(w / 2); x.vibe_wasm_event(19, 0, 1, c, 0); x.vibe_wasm_event(19, 1, 1, c + 20, 0); x.vibe_wasm_event(19, 2, 0, c + 20, 0); x.vibe_wasm_event(19, 3, 0, 1, 0); }
          h.inAudio = true; x.vibe_wasm_audio(BLOCK, 0); h.inAudio = false;
          x.vibe_wasm_draw(); const t0 = performance.now(); p = x.vibe_wasm_stage_draw(); const t = performance.now() - t0; max = Math.max(max, t); if (t > 10) slow++;
        }
        x.vibe_wasm_event(19, 9, 0, 0, 0);
        check('Stage', !!p && !hunted, p ? 'drawn; click / drag / wheel / SHOWN / HIDDEN taken' : 'stage_draw returns no picture');
        check('Stage', slow ? null : true, slow ? `${slow} of 60 frames over 10 ms (longest ${max.toFixed(1)} ms) – it will stutter in the browser` : `every frame under 10 ms (longest ${max.toFixed(1)} ms)`);
        const stageRun = (draw) => { const h2 = host(new Map()), x2 = h2.inst.exports; try { h2.inAudio = false; x2.vibe_wasm_init(SR); const io = x2.vibe_wasm_io(), blocks = Math.ceil(4 * SR / BLOCK); let sum = 0, n = 0;
          if (draw) x2.vibe_wasm_event(19, 8, 0, 0, 0);
          for (let b = 0; b < blocks; b++) { const t = b * BLOCK / SR;
            if (Math.abs(t - 2.5) < BLOCK / SR / 2) { x2.vibe_wasm_event(2, 12, 1, 0.8, 0); x2.vibe_wasm_event(5, 0, 0, 0, 0x90 | 60 << 8 | 100 << 16); }
            if (Math.abs(t - 3.5) < BLOCK / SR / 2) { x2.vibe_wasm_event(2, 12, 0, 0, 0); x2.vibe_wasm_event(5, 0, 0, 0, 0x80 | 60 << 8); }
            h2.inAudio = true; x2.vibe_wasm_audio(BLOCK, 0); h2.inAudio = false;
            if (t >= 2.5) { const f = new Float32Array(x2.memory.buffer, io, 768); for (let i = 0; i < BLOCK; i++) { sum += f[256 + i] * f[256 + i]; n++; } }
            if (b % 12 === 0) { x2.vibe_wasm_draw(); if (draw) x2.vibe_wasm_stage_draw(); } }
          return Math.sqrt(sum / Math.max(1, n)); } catch (e) { return -1; } };
        const a = stageRun(true), b = stageRun(false);
        if (a > 0.001 || b > 0.001) check('Stage', Math.abs(a - b) <= Math.max(a, b) * 0.05, Math.abs(a - b) <= Math.max(a, b) * 0.05 ? 'sounds the same with and without the stage' : `sounds different with the stage (level ${a.toFixed(3)}) than without (${b.toFixed(3)}) – the stage is a picture only`);
      } else if (manifest && manifest.stage) check('Manifest', false, '"stage" in firmware.json, but the firmware never calls vibe_stage');
    } catch (e) { check('Stage', false, 'error: ' + String(e && e.message || e)); }
  } else if (manifest && manifest.stage) check('Manifest', false, '"stage" in firmware.json, but the firmware has no stage (SDK 23)');
}
if (hunted) check('Robustness', false, hunted);
report();

// ---------------------------------------------------------------- 5. report ------------------------------------------
function report() {
  const failed = results.filter((r) => r.ok === false).length;
  if (JSON_OUT) console.log(JSON.stringify({ firmware: id, passed: !failed, results }, null, 1));
  else {
    console.log(`vibe-check ${id} – VIBE FM Protokoll (stage 1)`);
    for (const r of results) console.log(`  ${r.ok === true ? '✓' : r.ok === false ? '✗' : '·'} ${r.part.padEnd(11)} ${r.text}`);
    console.log(failed ? `FAILED (${failed} failure${failed === 1 ? '' : 's'})` : 'PASSED (stage 1; block sizes and sample rates to follow)');
  }
  process.exit(failed ? 1 : 0);
}

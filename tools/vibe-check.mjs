// SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE.
// vibe-check – does a firmware keep the VIBE FM Protocol? Stage 1: the parts that need no
// listening – format, robustness, realtime rules, storage, report. (Block sizes and sample rates come next.)
//   node tools/vibe-check.mjs build/groove.wasm [--manifest firmware/groove/firmware.json] [--seconds 60] [--json]
// Exit code 0 = passed, 1 = failed, 2 = could not run.
import fs from 'fs';
import path from 'path';
import { execFileSync } from 'child_process';

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const file = args.find((a, i) => !a.startsWith('--') && !['--manifest', '--seconds', '--files'].includes(args[i - 1]));
if (!file) { console.error('usage: node tools/vibe-check.mjs <firmware.wasm> [--manifest firmware.json] [--seconds 60] [--json]'); process.exit(2); }
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
if (badImp.length || missing.length) report();

// ---------------------------------------------------------------- a host that watches ----------------------------------
const NAME = /^(?:[a-z0-9_-][a-z0-9._-]{0,30}|(?=.{1,127}$)[a-z0-9_-][a-z0-9._-]{0,62}(?:\/[a-z0-9_-][a-z0-9._-]{0,62}){1,4})$/;   // SDK 4: paths
function host(files) {
  const h = { files, inAudio: false, audioStore: 0, audioLog: 0, logs: [], requests: [], badNames: 0, reads: 0, writes: 0, inst: null };
  const mem = () => h.inst.exports.memory.buffer, str = (p, n) => Buffer.from(mem(), p, n).toString();
  h.inst = new WebAssembly.Instance(mod, { env: {
    vibe_host_log: (p, n) => { if (h.inAudio) h.audioLog++; if (h.logs.length < 50) h.logs.push(str(p, n)); },
    vibe_host_request: (w) => h.requests.push(w),
    vibe_host_store: (op, np, nl, dp, len) => {
      if (h.inAudio) h.audioStore++;
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
      if (fuzz) for (let k = pick(3); k > 0; k--) { fuzzEvent(x, held); hunt(x, 'event ' + JSON.stringify(last[last.length - 1])); }
      if (fuzz && rnd() < 0.05) { const io4 = new Float32Array(x.memory.buffer, io, 1024); for (let i = 0; i < BLOCK; i++) { io4[i] = rnd() * 2 - 1; io4[768 + i] = rnd() * 2 - 1; } }
      const has = fuzz && out.version >= 2 ? pick(4) : 0;
      h.inAudio = true; const t0 = performance.now(); x.vibe_wasm_audio(BLOCK, has); out.ms.push(performance.now() - t0); h.inAudio = false; hunt(x, `audio block ${b}`);
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
if (opt('--files')) for (const n of fs.readdirSync(opt('--files'), { recursive: true })) { const k = n.split(path.sep).join('/'), f = path.join(opt('--files'), n); if (fs.statSync(f).isFile() && NAME.test(k)) files.set(k, fs.readFileSync(f)); }
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
check('Storage', !h1.badNames, h1.badNames ? `${h1.badNames}× invalid file name` : 'file names valid');

// ---------------------------------------------------------------- 4. storage: saved files survive a restart -----------
if (r1.version < 3) check('Storage', null, 'Protokoll < 3: no storage');
else if (!files.size) check('Storage', null, `not checked – the firmware saved nothing in ${SECONDS} s`);
else {
  const h2 = host(new Map(files)), r2 = run(h2, 3, false);
  check('Storage', !r2.trap && !r2.nan, r2.trap ? 'crash on start with its own files: ' + r2.trap : `restart with ${files.size} own files (${[...files.values()].reduce((a, f) => a + f.length, 0)} bytes) without errors, ${h2.reads} read`);
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

// host.js – the browser runtime ("the device") for VIBE firmwares:
//   loads a firmware .wasm, starts it in the audio thread (worklet.js), forwards every control from the panel,
//   the computer keyboard and USB-MIDI as SDK events, shows the display frames and LEDs it sends back,
//   connects IN 1/IN 2 when a firmware asks, runs the rear panel (pedal, CV/Gate, clock) and offers the firmware
//   library (HOME at power-on, or the menu).
import { Panel, EV, BTN, ENC, BOX_H, displayTouch, crisp } from './panel.js';
import { Controllers } from './controllers.js';
import { Rear } from './rear.js';
import { Editor } from './editor.js';
let editor = null;                                  // (the plugin's Editor view, editor.js – set in pluginMode)
let lastFb = null;                                  // (the device's picture as it stands: for a view that opens later)

const $ = (s) => document.querySelector(s);
const dev = $('#device'), wrap = $('#stagewrap'), power = $('#power'), powerBtn = $('#power-on');
let ctx = null, node = null, gain = null, micNode = null, micStream = null, meters = null;
let library = [], current = null, running = false, framePending = false, lastFrame = 0;
// device mode: this page served by a VIBE (/emu/ on its runtime) – no sound or firmware here, the picture, lights and
// captions come live from the device, every control goes to it (until the VIBE has its own display and lit keys)
const REMOTE = location.pathname.startsWith('/emu');
// plugin mode: this page is the window of the VIBE FM Plugin (VST3 / AU / CLAP, a WebView) – the firmware runs in the
// plugin; it binds vibe_ev / vibe_library / vibe_load / vibe_current and sends the picture with vibePacket()
const PLUGIN = typeof window.vibe_ev === 'function';
// (the plugin binds its functions once the page has loaded – on macOS it then loads the page again itself; WebView2 on
//  Windows does not: the page started as the browser emulator, rear panel, Mk1 colours and the emulator's firmwares.
//  So the page looks for them a few seconds and loads itself again once they are there.)
// (the plugin marks the page before it loads – window.__VIBE_PLUGIN: then it never shows the power-on screen)
if (!PLUGIN && window.__VIBE_PLUGIN) document.documentElement.classList.add('plugin-wait');
if (!PLUGIN) { let n = 0; const t = setInterval(() => { if (typeof window.vibe_ev === 'function') { clearInterval(t); location.reload(); } else if (++n > 80) clearInterval(t); }, 50); }

// touch screens: a long press must never open the context menu or select text
wrap.addEventListener('contextmenu', (e) => e.preventDefault());
document.addEventListener('selectstart', (e) => { if (e.target.closest && e.target.closest('#stagewrap')) e.preventDefault(); });

const panel = new Panel(dev, (type, id, down, value) => { if (down || type === EV.ENCODER) ctl.touched(type, id); send(type, id, down, value); },
  (v) => { if (PLUGIN) return; if (REMOTE) { fetch('/ev?t=100&v=' + v.toFixed(3)).catch(() => {}); return; } if (gain) gain.gain.setTargetAtTime(v * v, ctx.currentTime, 0.02); });
let actT = 0;
const ctl = new Controllers((type, id, down, value, midi) => { panel.mirror(type, id, down, value); send(type, id, down, value, midi); }, status, (text) => { if (!ctl.learning || !ctl.target) status(text); const b = $('#tb-midi'); b.classList.add('on'); clearTimeout(actT); b.style.outline = '2px solid #6EE7A8'; actT = setTimeout(() => (b.style.outline = ''), 150); });
const rear = new Rear($('#rear'), send, (volts) => { if (node && running) node.port.postMessage({ t: 'cvin', volts }); },
  async () => { await powerOn(); enableInput(); });
ctl.onRaw = (d) => rear.midi(d);   // CC64/CC11 = pedal, MIDI clock = CLK IN
ctl.onVolume = (v) => panel.setVolume(v); ctl.getVolume = () => panel.volume;   // e.g. MPK mini K8
// (3.11.2026, Protokoll 15) HOME held 1 s or SHIFT + HOME = the firmware library, in every firmware – the host does it, as
// on the VIBE (runtime/linux vibe_runtime.c). HOME tapped belongs to the firmware: it shows what every control does.
let homeT = 0, shiftHeld = false;
function send(type, id, down, value, midi = 0) {
  if (type === EV.BUTTON && id === BTN.SHIFT) shiftHeld = !!down;
  if (type === EV.BUTTON && id === BTN.HOME && !down) { clearTimeout(homeT); homeT = 0; }
  if (menuEvent(type, id, down, value)) return;   // (3.11.: the library is open – it takes the controls, as on the VIBE)
  if (type === EV.BUTTON && id === BTN.HOME) {
    clearTimeout(homeT); homeT = 0;
    if (down && shiftHeld) { showMenu(); return; }
    if (down) homeT = setTimeout(() => { homeT = 0; showMenu(); }, 1000);
  }
  if (type === EV.BUTTON && down) ctl.area(id);   // TAP TEMPO ("the area again") follows the area opened last, however it was opened
  if (PLUGIN) { window.vibe_ev(type, id, down ? 1 : 0, value, midi); return; }
  if (REMOTE) { fetch(`/ev?t=${type}&i=${id}&d=${down ? 1 : 0}&v=${value}&m=${midi}`).catch(() => {}); return; }
  if (node && running) node.port.postMessage({ t: 'ev', type, id, down, value, midi, heard: heardFrame() }); }
// Protokoll 5 (7.1): the context frame at the loudspeaker right now – what the player heard when touching a control
function heardFrame() {
  if (!ctx || !ctx.getOutputTimestamp) return undefined;
  const ts = ctx.getOutputTimestamp();
  if (!ts || !ts.contextTime) return undefined;
  return Math.round((ts.contextTime + (performance.now() - ts.performanceTime) / 1000) * ctx.sampleRate);
}

// ---------------------------------------------------------------- firmware library ----------------------------------
async function loadLibrary() {
  try { library = await (await fetch('firmware/index.json', { cache: 'no-cache' })).json(); } catch (e) { library = []; }
  const sel = $('#fw');
  sel.innerHTML = library.map((f) => `<option value="${f.id}">${f.name} ${f.version || ''}</option>`).join('') + '<option value="__file">Load your own .wasm …</option>';
}
// (3.11.2026) a Launchpad as an 8 x 8 grid: the firmware gets 64 pads (vibe_device(), SDK 20) – Controllers ▾, remembered;
// the on-screen pads are pads 0-15 then (the grid's top two rows), the Launchpad's top row and right column stay VIBE buttons
let lpGrid = false; try { lpGrid = localStorage.getItem('vibe.lpgrid') === '1'; } catch (e) { /* private mode */ }
ctl.lpGrid = lpGrid;
ctl.onWire = () => { if (!PLUGIN) ctlRefresh().catch(() => {}); };   // (the menu shows the devices and their profiles)
function deviceBytes() {                             // Protokoll 15b, 52 bytes (sdk/include/vibe.h vibe_device_info)
  if (!lpGrid || PLUGIN) return undefined;
  const b = new Uint8Array(52); b.set([25, 64, 0, 0, 0, 0, 8, 8], 0);
  b[16] = 480 & 255; b[17] = 480 >> 8; b[18] = 320 & 255; b[19] = 320 >> 8;
  const n = 'VIBE FM + Launchpad 8x8'; for (let i = 0; i < n.length; i++) b[20 + i] = n.charCodeAt(i);
  return b;
}
async function boot(fw) {
  // fw = { name, version, bytes } or a library entry
  let bytes = fw.bytes;
  if (!bytes) bytes = await (await fetch(fw.file, { cache: 'no-cache' })).arrayBuffer();
  let module;
  try { module = await WebAssembly.compile(bytes); } catch (e) { status('Not a valid firmware: ' + e.message, true); return; }
  if (node) { node.port.onmessage = null; node.disconnect(); node = null; }
  running = false; current = fw; $('#fw').value = fw.id || '__file';
  panel.setLatch(fw.latch !== false);
  const store = await storeLoad(storeSpace(fw));
  await seed(fw, store);
  mediaAsked = false;
  node = new AudioWorkletNode(ctx, 'vibe-host', { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], processorOptions: { module, store, fwid: storeSpace(fw), media: mediaList(), device: deviceBytes() } });
  node.connect(gain);
  if (micNode) micNode.connect(node);
  node.port.onmessage = (m) => onMessage(m.data);
  node.port.postMessage({ t: 'latency', frames: Math.round(((ctx.outputLatency || 0) + (ctx.baseLatency || 0)) * ctx.sampleRate) });   // Protokoll 5
  hideMenu(); loadCheat(fw);
}
// (28.10.) a firmware's starter files: firmware/<id>.seed/index.json lists them (PLAYER: a few demo tracks of our own
// firmwares) – put into its storage once, the first time it starts with no music of its own; deleting them keeps them gone
// (index.json: a list of names, or { "version": n, "files": [...] } – a new version puts its files in again, over the old
//  ones of the same name, also where the player has music of its own: 28.10. the demos' tempos were wrong)
async function seed(fw, store) {
  if (!fw || !fw.id) return;
  const key = 'vibe.seeded.' + fw.id;
  let done = null; try { done = localStorage.getItem(key); } catch (e) { return; }
  let idx; try { const r = await fetch('firmware/' + fw.id + '.seed/index.json', { cache: 'no-cache' }); if (!r.ok) return; idx = await r.json(); } catch (e) { return; }
  const list = Array.isArray(idx) ? idx : idx.files || [], ver = String(Array.isArray(idx) ? 1 : idx.version || 1);
  if (done === ver) return;
  if (!done && Object.keys(store).some((n) => /\.(mp3|wav)$/i.test(n) && !list.includes(n))) { try { localStorage.setItem(key, ver); } catch (e) { /* storage off */ } return; }   /* (music of its own and never seeded: leave it) */
  status('Loading the demo music …');
  for (const name of list) {
    try {
      const r = await fetch('firmware/' + fw.id + '.seed/' + name); if (!r.ok) continue;
      const data = await r.arrayBuffer(); store[name] = data; await storePut(storeSpace(fw), name, data.slice(0));
    } catch (e) { /* that one not */ }
  }
  try { localStorage.setItem(key, ver); } catch (e) { /* storage off */ }
}
function onMessage(d) {
  if (d.t === 'ready') {
    ctl.setMixer([]); ctl.fwAreas = d.areas || 0;
    running = true; ctl.sdk = d.sdk; status((current.name || 'Firmware') + ' running · VIBE SDK ' + d.sdk);
    node.port.postMessage({ t: 'cvin', volts: +rear.cvKnob.value });
  }
  else if (d.t === 'eddesc') edDescribed(d);         // (the Editor in the browser, 31.10.)
  else if (d.t === 'curveR') { const r = edWait.get(d.id); edWait.delete(d.id); if (r) r(JSON.stringify(d.pts)); }
  else if (d.t === 'frame') { if (d.pv) edVals = d.pv; if (d.posts) edPosts.push(...d.posts); framePending = false; { const fb = new Uint16Array(d.fb); lastFb = fb; panel.showFrame(fb); if (editor && editor.open) editor.frame(fb); } panel.showLeds(d.leds); panel.showLabels(d.labels); ctl.lights(d.leds, d.padx); if (d.mixv) ctl.mixValues(d.mixv, d.leds); rear.show(d.rear); }
  else if (d.t === 'mixer') ctl.setMixer(d.params);   // (SDK 11) the firmware's mix_* parameters
  else if (d.t === 'log') console.log('[firmware]', d.text);
  else if (d.t === 'midi') ctl.midiOut(d.msgs, ctx);   // (SDK 9) the firmware's MIDI out – to the chosen Web MIDI port
  else if (d.t === 'store') { const sp = storeSpace(current); storeQ = storeQ.then(() => storePut(sp, d.name, d.data)); }
  else if (d.t === 'request') { if (d.what === 1) showMenu(); if (d.what === 2) enableInput(); if (d.what === 3) { const fw = current; storeQ.then(() => boot(fw)); } }   // 3: restart, once its writes / deletes are stored
  else if (d.t === 'mreq') mediaRead(d);   // SDK 13: the worklet asks for a list, a size, a block
  else if (d.t === 'mopen' || d.t === 'mwrite' || d.t === 'mclose') mediaWrite(d);
  else if (d.t === 'masked') { mediaAsked = true; $('#tb-media').classList.add('on'); }
  else if (d.t === 'crash') { running = false; status('Firmware stopped: ' + d.error, true); showMenu('The firmware crashed – not the device. Choose a firmware:'); }
}

// ---------------------------------------------------------------- media (SDK 13, Protokoll 13a) ----------------------
// Folders the firmware may read: picked with the Media button (showDirectoryPicker – the first one picked with write
// permission also takes the firmware's own files, "VIBE FM/<id>/") or dropped onto the device while a firmware that uses
// media runs (read-only; Firefox and Safari: this is the way). Kept in IndexedDB (a handle, not the files), asked for
// permission again on the next visit. The worklet keeps the cache; here the reading and writing happen.
const MBLK = 65536;
let mediaSrc = [], mediaAsked = false;
const mediaList = () => mediaSrc.filter((s) => s.ok).map((s) => ({ key: s.key, label: s.label, write: !!s.write }));
const mediaPush = () => { if (node) node.port.postMessage({ t: 'msrc', list: mediaList() }); const b = $('#tb-media'); if (b) b.title = mediaTitle(); };
const mediaTitle = () => 'Media: folders the firmware may read (samples, songs) – SDK 13. ' + (mediaSrc.length ? 'Now: ' + mediaSrc.map((s) => s.label + (s.write ? ' (writes here)' : '') + (s.ok ? '' : ' – click to allow again')).join(', ') + '. Shift+click: forget them all.' : 'Click to pick one, or drop a folder onto the device.');
function mediaKey(name) {   // a stable key from the folder's name: dir-<name>, numbered if taken
  const b = 'dir-' + (name.normalize('NFKD').replace(/[^A-Za-z0-9_-]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 38) || 'folder');
  let k = b; for (let i = 2; mediaSrc.some((s) => s.key === k); i++) k = b + '-' + i;
  return k;
}
let mdbp = null;
function mdb() { if (!mdbp) mdbp = new Promise((res, rej) => { const r = indexedDB.open('vibe-media', 1); r.onupgradeneeded = () => r.result.createObjectStore('dirs'); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); }); return mdbp; }
async function mediaSave() {
  try { const d = await mdb(), tx = d.transaction('dirs', 'readwrite'), os = tx.objectStore('dirs'); os.clear(); for (const s of mediaSrc) if (s.h && s.h.kind === 'directory' && !s.dropped) os.put({ key: s.key, label: s.label, write: !!s.write, h: s.h }, s.key); await new Promise((res) => { tx.oncomplete = res; tx.onerror = res; }); } catch (e) { /* storage off */ }
}
async function mediaRestore() {
  if (PLUGIN || REMOTE) return;
  try {
    const d = await mdb(), all = await new Promise((res, rej) => { const r = d.transaction('dirs').objectStore('dirs').getAll(); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); });
    for (const x of all) { const q = x.h.queryPermission ? await x.h.queryPermission({ mode: x.write ? 'readwrite' : 'read' }) : 'denied'; mediaSrc.push({ ...x, ok: q === 'granted' }); }
    mediaPush(); if (mediaSrc.some((s) => s.ok)) $('#tb-media').classList.add('on');
  } catch (e) { /* none */ }
}
async function mediaPick(ev) {
  if (ev.shiftKey) { mediaSrc = []; await mediaSave(); mediaPush(); $('#tb-media').classList.remove('on'); status('Media folders forgotten'); return; }
  const waiting = mediaSrc.filter((s) => !s.ok && s.h && s.h.requestPermission);
  if (waiting.length) {   // (a new visit: the browser wants a click before it lets the page read them again)
    for (const s of waiting) s.ok = (await s.h.requestPermission({ mode: s.write ? 'readwrite' : 'read' })) === 'granted';
    mediaPush(); status('Media: ' + mediaSrc.filter((s) => s.ok).map((s) => s.label).join(', ')); return;
  }
  if (!window.showDirectoryPicker) { status('This browser cannot open folders – drop a folder onto the device instead (while a firmware that uses media runs)', true); return; }
  let h; try { h = await window.showDirectoryPicker({ id: 'vibe-media', mode: 'readwrite' }); } catch (e) { return; }
  const write = !mediaSrc.some((s) => s.write && s.ok) && (await h.queryPermission({ mode: 'readwrite' })) === 'granted';
  mediaSrc.push({ key: mediaKey(h.name), label: h.name, write, h, ok: true });
  await mediaSave(); mediaPush(); $('#tb-media').classList.add('on');
  status(`Media: ${h.name}` + (write ? ' (the firmware writes its own files into "VIBE FM" there)' : ''));
}
function mediaDrop(entry, handle) {   // a folder dropped: read-only
  const h = handle || entry; if (!h) return;
  mediaSrc.push({ key: mediaKey(h.name), label: h.name, write: false, h, ok: true, dropped: true });
  mediaPush(); $('#tb-media').classList.add('on'); status('Media: ' + h.name + ' (dropped – read only, gone after a reload)');
}
// a folder as a FileSystemDirectoryHandle (picked; dropped in Chrome) or a FileSystemDirectoryEntry (dropped elsewhere)
const fsx = {
  child: (d, name, dir) => d.getDirectoryHandle ? (dir ? d.getDirectoryHandle(name) : d.getFileHandle(name)) : new Promise((res, rej) => (dir ? d.getDirectory : d.getFile).call(d, name, {}, res, rej)),
  async list(d) {
    const out = [];
    if (d.entries) { for await (const [name, h] of d.entries()) out.push({ name, dir: h.kind === 'directory', h }); return out; }
    const r = d.createReader();
    for (;;) { const b = await new Promise((res, rej) => r.readEntries(res, rej)); if (!b.length) break; for (const e of b) out.push({ name: e.name, dir: e.isDirectory, h: e }); }
    return out;
  },
  file: (h) => h.getFile ? h.getFile() : new Promise((res, rej) => h.file(res, rej)),
};
async function mediaDir(s, parts, create) { let d = s.h; for (const p of parts) d = d.getDirectoryHandle ? await d.getDirectoryHandle(p, { create: !!create }) : await fsx.child(d, p, true); return d; }
const mFiles = new Map();   // path -> File (a few seconds: one getFile for many blocks)
async function mediaRead(d) {
  const reply = (v, miss, size, tr) => { if (node) node.port.postMessage({ t: 'mres', kind: d.kind, path: d.path, blk: d.blk, v, miss, size }, tr || []); };
  const [key, ...parts] = d.path.split('/'), s = mediaSrc.find((x) => x.key === key && x.ok);
  if (!s) return reply(null, true);
  try {
    if (d.kind === 'l') {
      const dir = await mediaDir(s, parts), items = (await fsx.list(dir)).filter((e) => !e.name.startsWith('.') && !e.name.includes('\\'));
      await Promise.all(items.map(async (e) => { if (!e.dir) e.size = (await fsx.file(e.h)).size; }));
      items.sort((a, b) => (b.dir - a.dir) || (a.name.toLowerCase() < b.name.toLowerCase() ? -1 : a.name.toLowerCase() > b.name.toLowerCase() ? 1 : 0));
      return reply(items.map((e) => e.dir ? `d\t\t${e.name}\n` : `f\t${e.size}\t${e.name}\n`).join(''));
    }
    const dir = await mediaDir(s, parts.slice(0, -1)), leaf = parts[parts.length - 1];
    if (d.kind === 's') {
      try { const f = await fsx.file(await fsx.child(dir, leaf, false)); return reply([1, f.size, Math.floor(f.lastModified / 1000)]); }
      catch (e) { await fsx.child(dir, leaf, true); return reply([2, 0, 0]); }
    }
    let f = mFiles.get(d.path);
    if (!f || performance.now() - f.t > 5000) { f = { file: await fsx.file(await fsx.child(dir, leaf, false)), t: performance.now() }; mFiles.set(d.path, f); if (mFiles.size > 8) mFiles.delete(mFiles.keys().next().value); }
    const buf = await f.file.slice(d.blk * MBLK, d.blk * MBLK + MBLK).arrayBuffer();
    reply(buf, false, f.file.size, [buf]);
  } catch (e) { mFiles.delete(d.path); reply(null, true); }
}
const mWriters = new Map();   // handle -> { q: a promise chain, ws, dir, name, key, failed }
function mediaWrite(d) {
  const ack = (m) => { if (node) node.port.postMessage(m); };
  if (d.t === 'mopen') {
    const s = mediaSrc.find((x) => x.key === d.key && x.ok), w = { key: d.key, name: d.name, failed: !s };
    w.q = s ? (async () => { w.dir = await mediaDir(s, d.dir, true); w.ws = await (await w.dir.getFileHandle(d.name, { create: true })).createWritable(); })().catch(() => { w.failed = true; }) : Promise.resolve();
    mWriters.set(d.h, w); return;
  }
  const w = mWriters.get(d.h); if (!w) return;
  if (d.t === 'mwrite') { const n = d.data.byteLength; w.q = w.q.then(async () => { if (w.failed) throw 0; await w.ws.write(d.data); }).then(() => ack({ t: 'mack', h: d.h, n, ok: true }), () => { w.failed = true; ack({ t: 'mack', h: d.h, n, ok: false }); }); return; }
  mWriters.delete(d.h);   // mclose
  w.q.then(async () => {
    if (!d.keep) { try { if (w.ws) await w.ws.abort(); if (w.dir) await w.dir.removeEntry(w.name); } catch (e) { /* gone anyway */ } return; }
    if (w.failed) throw 0;
    await w.ws.close();
    ack({ t: 'mdone', h: d.h, key: w.key, ok: true });
  }).catch(() => { try { if (w.ws) w.ws.abort(); } catch (e) { /* */ } ack({ t: 'mdone', h: d.h, key: w.key, ok: false }); status('Media: ' + w.name + ' could not be written', true); });
}

// ---------------------------------------------------------------- storage (SDK 3) ---------------------------------
// One IndexedDB database for all firmwares; a file's key is "<firmware>/<name>". The worklet gets the firmware's files
// before it starts and sends every write back here.
const storeSpace = (fw) => (fw && fw.id) || 'eigene-' + ((fw && fw.name) || 'firmware').toLowerCase().replace(/[^a-z0-9._-]/g, '');
let dbp = null, storeQ = Promise.resolve();   // storeQ: the writes in order (a restart waits for them)
function db() {
  if (!dbp) dbp = new Promise((res, rej) => { const r = indexedDB.open('vibe-store', 1); r.onupgradeneeded = () => r.result.createObjectStore('files'); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); });
  return dbp;
}
async function storeLoad(space) {
  try {
    const d = await db(), out = {}, range = IDBKeyRange.bound(space + '/', space + '/\uffff');
    await new Promise((res, rej) => { const c = d.transaction('files').objectStore('files').openCursor(range);
      c.onsuccess = () => { const k = c.result; if (!k) return res(); out[k.key.slice(space.length + 1)] = k.value; k.continue(); }; c.onerror = () => rej(c.error); });
    return out;
  } catch (e) { console.warn('storage unavailable', e); return {}; }
}
async function storePut(space, name, data) {
  try {
    const d = await db(), tx = d.transaction('files', 'readwrite'), os = tx.objectStore('files');
    if (data) os.put(data, space + '/' + name); else os.delete(space + '/' + name);
    await new Promise((res, rej) => { tx.oncomplete = res; tx.onerror = () => rej(tx.error); });
  } catch (e) { status('Saving in the browser failed: ' + (e.message || e), true); }
}

// the library menu over the display = the VIBE's main menu (3.11.2026, runtime/linux/vibe_runtime.c menu_draw / menu_event):
// 3 x 2 tiles with each firmware's picture, and the same keys – DATA / an encoder / ◀ ▶ / OCT move, ENTER · DATA push ·
// PLAY · F1 start, HOME · RETURN · F6 back, a pad starts that tile. So a controller (the MPK's Undo = HOME) gets out again.
const MENU = { open: false, sel: 0, top: 0, acc: 0, items: [], note: '', icons: {}, cv: null };
const MENU_ICON_W = 148, MENU_ICON_H = 83;
function menuIcon(id) {
  if (id in MENU.icons) return MENU.icons[id];
  const im = new Image(); MENU.icons[id] = null;
  im.onload = () => { MENU.icons[id] = im; if (MENU.open) menuDraw(); };
  im.src = 'firmware/icons/' + id + '.webp';
  return null;
}
function showMenu(note) {
  const m = panel.menu;
  MENU.items = library.map((f) => ({ id: f.id, name: f.name || f.id, version: f.version || '', desc: f.description || '', f }));
  MENU.items.push(PLUGIN ? { id: '__add', name: 'ADD FIRMWARE', version: '', desc: 'choose the .zip from thefirmwarehub.com – or drop it onto this window' }
    : { id: '__file', name: 'YOUR OWN', version: '', desc: 'choose a .wasm – or drop it onto the device' });
  MENU.note = note || ''; MENU.acc = 0; MENU.top = 0;
  MENU.sel = Math.max(0, MENU.items.findIndex((it) => current && it.id === current.id));
  if (!MENU.cv) {
    MENU.cv = document.createElement('canvas'); MENU.cv.width = 960; MENU.cv.height = 640; MENU.cv.className = 'menucv';
    MENU.cv.addEventListener('click', (e) => {   // (the mouse: a tile starts it, a soft-key label does what it says)
      const r = MENU.cv.getBoundingClientRect(), x = (e.clientX - r.left) * 480 / r.width, y = (e.clientY - r.top) * 320 / r.height;
      if (y >= 282) { if (x < 80) menuStart(); else if (x >= 400) hideMenu(); return; }
      for (let k = 0; k < 6; k++) {
        const i = MENU.top * 3 + k, tx = 8 + (k % 3) * 158, ty = 40 + Math.floor(k / 3) * 110;
        if (i < MENU.items.length && x >= tx - 3 && x < tx + MENU_ICON_W + 3 && y >= ty - 3 && y < ty + MENU_ICON_H + 24) { MENU.sel = i; menuStart(); return; }
      }
    });
    MENU.cv.addEventListener('wheel', (e) => { e.preventDefault(); menuEvent(EV.ENCODER, ENC.DATA, 0, e.deltaY > 0 ? 1 : -1); }, { passive: false });
  }
  m.replaceChildren(MENU.cv); m.classList.add('on', 'grid');
  MENU.open = true; menuDraw();
  if (document.fonts) document.fonts.load("600 15px 'Roboto Condensed'").then(() => { if (MENU.open) menuDraw(); }, () => {});
}
function hideMenu() { panel.menu.classList.remove('on', 'grid'); MENU.open = false; }
function menuStart() {
  const it = MENU.items[MENU.sel]; if (!it) return;
  if (it.id === '__add') { pluginPick(); return; }
  if (it.id === '__file') { $('#fwfile').click(); return; }
  if (current && current.id === it.id && running) { hideMenu(); return; }
  if (PLUGIN) pluginLoad(it.id); else powerOn().then(() => boot(it.f));
}
// a control while the menu is open: the menu takes it (true), as on the VIBE – the firmware gets nothing meanwhile
function menuEvent(type, id, down, value) {
  if (!MENU.open) return false;
  const n = MENU.items.length;
  if (type === EV.ENCODER) {
    MENU.acc += value;
    while (MENU.acc >= 1) { MENU.acc -= 1; if (MENU.sel < n - 1) MENU.sel++; }
    while (MENU.acc <= -1) { MENU.acc += 1; if (MENU.sel > 0) MENU.sel--; }
    menuDraw(); return true;
  }
  if (type === EV.PAD && down && id < n) { MENU.sel = id; menuStart(); return true; }
  if (type !== EV.BUTTON) return type === EV.PAD || type === EV.KEY;   // (keys and pads: not into the firmware underneath either)
  if (!down) return true;
  if (id === BTN.HOME || id === BTN.RETURN || id === BTN.F6) hideMenu();
  else if (id === BTN.RIGHT) MENU.sel = Math.min(n - 1, MENU.sel + 1);
  else if (id === BTN.LEFT) MENU.sel = Math.max(0, MENU.sel - 1);
  else if (id === BTN.OCT_UP) MENU.sel = Math.min(n - 1, MENU.sel + 3);
  else if (id === BTN.OCT_DOWN) MENU.sel = Math.max(0, MENU.sel - 3);
  else if (id === BTN.ENTER || id === BTN.DATA_PUSH || id === BTN.PLAY || id === BTN.F1) { menuStart(); return true; }
  if (MENU.open) menuDraw();
  return true;
}
function menuDraw() {
  const c = MENU.cv.getContext('2d'), F = (px, w = 600) => `${w} ${px}px 'Roboto Condensed', 'Archivo', system-ui, sans-serif`;
  const fit = (t, font, max) => { c.font = font; while (t && c.measureText(t).width > max) t = t.slice(0, -1); return t; };
  const round = (x, y, w, h, r, col) => { c.fillStyle = col; c.beginPath(); c.roundRect(x, y, w, h, r); c.fill(); };
  const text = (t, x, y, font, col, align = 'left') => { c.font = font; c.fillStyle = col; c.textAlign = align; c.fillText(t, x, y); };
  c.setTransform(2, 0, 0, 2, 0, 0); c.textBaseline = 'top';
  c.fillStyle = '#0B0D12'; c.fillRect(0, 0, 480, 320);
  text('VIBE FM', 10, 8, F(15), '#FF6A1A'); text('FIRMWARE', 88, 8, F(15), '#E6EAF2');
  text(String(MENU.items.length), 470, 8, F(15), '#8A93A6', 'right');
  c.fillStyle = '#FF6A1A'; c.fillRect(0, 31, 480, 1);
  const n = MENU.items.length, row = Math.floor(MENU.sel / 3);
  if (row < MENU.top) MENU.top = row; if (row > MENU.top + 1) MENU.top = row - 1;
  for (let k = 0; k < 6 && MENU.top * 3 + k < n; k++) {
    const i = MENU.top * 3 + k, x = 8 + (k % 3) * 158, y = 40 + Math.floor(k / 3) * 110, it = MENU.items[i];
    const on = i === MENU.sel, run = current && running && current.id === it.id, icon = it.id[0] === '_' ? null : menuIcon(it.id);
    if (on) round(x - 3, y - 3, MENU_ICON_W + 6, MENU_ICON_H + 6, 6, '#FF6A1A'); else round(x - 1, y - 1, MENU_ICON_W + 2, MENU_ICON_H + 2, 4, '#242C3C');
    if (icon) { c.save(); c.beginPath(); c.roundRect(x, y, MENU_ICON_W, MENU_ICON_H, 4); c.clip(); c.drawImage(icon, x, y, MENU_ICON_W, MENU_ICON_H); c.restore(); }
    else {
      round(x, y, MENU_ICON_W, MENU_ICON_H, 5, '#141A26'); c.fillStyle = it.id[0] === '_' ? '#8A93A6' : '#3FE0E8'; c.fillRect(x + 5, y, MENU_ICON_W - 10, 2);
      text(fit(it.id[0] === '_' ? '+' : it.name, F(22), MENU_ICON_W - 12), x + MENU_ICON_W / 2, y + 30, F(22), '#E6EAF2', 'center');
    }
    const v = run ? '▶ RUNNING' : it.version; c.font = F(14, 500); const vw = v ? c.measureText(v).width + 6 : 0;
    text(fit(it.name, F(15), MENU_ICON_W - vw), x, y + MENU_ICON_H + 5, F(15), on ? '#FFFFFF' : '#B8C0D0');
    if (vw) text(v, x + MENU_ICON_W, y + MENU_ICON_H + 6, F(14, 500), run ? '#6EE7A8' : '#5B6577', 'right');
  }
  const tri = (a, b, d, col) => { c.fillStyle = col; c.beginPath(); c.moveTo(...a); c.lineTo(...b); c.lineTo(...d); c.fill(); };
  if (MENU.top > 0) tri([424, 22], [436, 22], [430, 12], '#FF6A1A');
  if ((MENU.top + 2) * 3 < n) tri([440, 12], [452, 12], [446, 22], '#FF6A1A');
  const it = MENU.items[MENU.sel];
  if (MENU.note) text(fit(MENU.note, F(14, 500), 464), 8, 262, F(14, 500), '#FF8A4A');
  else if (it) text(fit(it.desc, F(14, 500), 464), 8, 262, F(14, 500), '#8A93A6');
  c.fillStyle = '#0E1220'; c.fillRect(0, 282, 480, 38);
  [['START', 0], ['BACK', 5]].forEach(([t, i]) => { const x = 40 + i * 80; round(x - 37, 286, 74, 30, 5, i === 0 ? '#FF6A1A' : '#24314A'); text(t, x, 293, F(15), i === 0 ? '#111214' : '#E6EAF2', 'center'); });
}
$('#fw').addEventListener('change', async (e) => { if (PLUGIN) { if (e.target.value === '__add') { $('#fw').value = current ? current.id : ''; pluginPick(); return; } pluginLoad(e.target.value); return; } if (e.target.value === '__file') { $('#fwfile').click(); return; } await powerOn(); boot(library.find((f) => f.id === e.target.value)); });
$('#fwfile').addEventListener('change', async (e) => { const f = e.target.files[0]; if (!f) return; await powerOn(); boot({ name: f.name.replace(/\.wasm$/, ''), bytes: await f.arrayBuffer() }); e.target.value = ''; });
wrap.addEventListener('dragover', (e) => { e.preventDefault(); if (!PLUGIN) wrap.classList.add('drop'); });
wrap.addEventListener('dragleave', () => wrap.classList.remove('drop'));
wrap.addEventListener('drop', async (e) => {
  e.preventDefault(); wrap.classList.remove('drop');
  if (PLUGIN) return;                                 // (the plugin: the window's own handler installs it, see pluginMode)
  const handles = mediaAsked ? [...e.dataTransfer.items].map((it) => it.getAsFileSystemHandle && it.getAsFileSystemHandle()) : [];   // (SDK 13: before the first await, too)
  const entries = [...e.dataTransfer.items].map((it) => it.webkitGetAsEntry && it.webkitGetAsEntry()).filter(Boolean);   // (now: they expire after the first await)
  if (mediaAsked && entries.length && entries.every((x) => x.isDirectory)) {   // (a firmware that uses media: the folders become media, read-only)
    const hs = await Promise.all(handles.map((p) => p && p.catch(() => null)));
    entries.forEach((x, i) => mediaDrop(x, hs[i] && hs[i].kind === 'directory' ? hs[i] : null)); return;
  }
  const plain = [...e.dataTransfer.files], fw = plain.find((f) => /\.wasm$/i.test(f.name));
  if (!fw && plain.length && plain.every((f) => /\.json$/i.test(f.name))) {   // (0.7.0) a controller profile
    if (await ctlTakeFiles(await Promise.all(plain.map(async (f) => ({ name: f.name, text: await f.text() }))))) return;
  }
  if (fw) { await powerOn(); boot({ name: fw.name.replace(/\.wasm$/i, ''), bytes: await fw.arrayBuffer() }); return; }
  let items = entries.length ? await walkEntries(entries) : plain.map((f) => ({ f, dir: [] }));
  if (entries.length === 1 && entries[0].isDirectory) items = items.map((x) => ({ f: x.f, dir: x.dir.slice(1) }));   // one folder dropped: its content (albums) at the top
  const music = items.filter((x) => AUDIO.test(x.f.name));
  if (music.length) { await powerOn(); addFiles(music); return; }
  status('Drop a .wasm firmware – or music (MP3, WAV, M4A, FLAC …, also whole folders) for PLAYER', true);
});
// files and folders dropped onto the device (SDK 4: names may hold folders) go into the running firmware's storage;
// MP3 / WAV as they are, M4A / AAC / FLAC / OGG / Opus / AIFF decoded by the browser into 24-bit WAV (nothing compressed
// again). Names made storable (a-z 0-9 _ - ., parts up to 63, numbered if taken); each folder gets a name.txt with its real
// name, which PLAYER shows.
const AUDIO = /\.(mp3|wav|m4a|aac|flac|ogg|oga|opus|aif|aiff|mp4)$/i, KEEP = /\.(mp3|wav)$/i;
async function walkEntries(entries) {
  const out = [];
  const walk = async (en, dir) => {
    if (en.isFile) out.push({ f: await new Promise((res, rej) => en.file(res, rej)), dir });
    else if (en.isDirectory) { const rd = en.createReader(); for (;;) { const batch = await new Promise((res, rej) => rd.readEntries(res, rej)); if (!batch.length) break; for (const c of batch) await walk(c, dir.concat(en.name)); } }
  };
  for (const en of entries) await walk(en, []);
  return out;
}
const cleanPart = (n, max) => (n.toLowerCase().replace(/ä/g, 'ae').replace(/ö/g, 'oe').replace(/ü/g, 'ue').replace(/ß/g, 'ss')
  .normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^a-z0-9_-]+/g, '-').replace(/-{2,}/g, '-').replace(/^[-_.]+|[-_]+$/g, '') || 'x').slice(0, max);
function storablePath(dir, fileName, ext, taken) {
  const folder = dir.map((d) => cleanPart(d, 63)).join('/'), max = (folder ? 63 : 31) - ext.length;
  const base = cleanPart(fileName.replace(/\.[^.]*$/, ''), max) || 'track';
  let name = (folder ? folder + '/' : '') + base + ext;
  for (let k = 2; taken.has(name); k++) { const sfx = '-' + k; name = (folder ? folder + '/' : '') + base.slice(0, max - sfx.length) + sfx + ext; }
  taken.add(name); return name.length <= 127 ? name : null;
}
function wav24(buf) {   // an AudioBuffer as a 24-bit PCM WAV
  const ch = Math.min(2, buf.numberOfChannels), n = buf.length, out = new DataView(new ArrayBuffer(44 + n * ch * 3)), w = (o, t) => [...t].forEach((c, i) => out.setUint8(o + i, c.charCodeAt(0)));
  w(0, 'RIFF'); out.setUint32(4, 36 + n * ch * 3, true); w(8, 'WAVEfmt '); out.setUint32(16, 16, true); out.setUint16(20, 1, true); out.setUint16(22, ch, true);
  out.setUint32(24, buf.sampleRate, true); out.setUint32(28, buf.sampleRate * ch * 3, true); out.setUint16(32, ch * 3, true); out.setUint16(34, 24, true); w(36, 'data'); out.setUint32(40, n * ch * 3, true);
  const d = [...Array(ch)].map((_, c) => buf.getChannelData(c));
  for (let i = 0, o = 44; i < n; i++) for (let c = 0; c < ch; c++, o += 3) { const v = Math.max(-8388608, Math.min(8388607, Math.round(d[c][i] * 8388607))); out.setUint8(o, v & 255); out.setUint8(o + 1, (v >> 8) & 255); out.setUint8(o + 2, (v >> 16) & 255); }
  return out.buffer;
}
async function addFiles(items) {
  if (!current || !node || !running) { status('Start a firmware first (PLAYER)', true); return; }
  const space = storeSpace(current), taken = new Set(Object.keys(await storeLoad(space)));
  const put = (name, data) => { storeQ = storeQ.then(() => storePut(space, name, data.slice(0))); node.port.postMessage({ t: 'storeput', name, data }, [data]); };
  let n = 0;
  for (const { f, dir } of items) {
    const keep = KEEP.test(f.name), ext = keep ? f.name.slice(f.name.lastIndexOf('.')).toLowerCase() : '.wav';
    for (let i = 1; i <= dir.length; i++) {   // the folders' real names
      const p = dir.slice(0, i).map((d) => cleanPart(d, 63)).join('/') + '/name.txt';
      if (!taken.has(p)) { taken.add(p); put(p, new TextEncoder().encode(dir[i - 1] + '\n').buffer); }
    }
    const name = storablePath(dir, f.name, ext, taken); if (!name) continue;
    status('Adding music … ' + (n + 1) + ' / ' + items.length + (keep ? '' : ' (' + f.name + ' → WAV)'));
    let data = await f.arrayBuffer();
    if (!keep) { try { data = wav24(await ctx.decodeAudioData(data)); } catch (err) { status('Cannot decode ' + f.name, true); continue; } }
    put(name, data); n++;
  }
  await storeQ;
  status(n + (n === 1 ? ' file' : ' files') + ' added to ' + (current.name || 'the firmware') + (current.id === 'player' ? ' – in the list now' : ''));
  if (current.id === 'player') { send(EV.BUTTON, BTN.MENU, 1, 0); send(EV.BUTTON, BTN.MENU, 0, 0); }   // PLAYER: MENU reads the list again
}

// ---------------------------------------------------------------- power, audio, input ------------------------------
let powering = null;
function powerOn() {
  if (PLUGIN) return Promise.resolve();               // (the plugin runs the firmware: the page never boots one)
  if (REMOTE) return Promise.resolve();          /* (device mode: the VIBE is on, its sound plays there) */
  if (powering) return powering;
  powering = (async () => {
    power.classList.add('gone');
    ctx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
    await ctx.audioWorklet.addModule('js/worklet.js');
    gain = ctx.createGain(); gain.gain.value = panel.volume * panel.volume; gain.connect(ctx.destination);
    if (ctx.state !== 'running') await ctx.resume();
    await loadLibrary();
    // MIDI allowed before? then connect right away (no click on MIDI needed)
    try { const p = await navigator.permissions.query({ name: 'midi', sysex: true }); if (p.state === 'granted') ctl.connect(); } catch (e) { /* no permissions API */ }
    const want = new URLSearchParams(location.search).get('fw');
    if (holdHome) showMenu('HOME held at power-on: choose a firmware');
    else boot(library.find((f) => f.id === want) || library[0]);
  })();
  return powering;
}
let holdHome = false;
powerBtn.addEventListener('click', () => powerOn());
// "Demo hören": switch on and press PLAY as soon as the firmware runs (GROOVE has a demo pattern)
$('#power-demo').addEventListener('click', async () => {
  await powerOn();
  for (let i = 0; i < 60 && !running; i++) await new Promise((r) => setTimeout(r, 50));
  setTimeout(() => { send(EV.BUTTON, BTN.PLAY, 1, 0); send(EV.BUTTON, BTN.PLAY, 0, 0); }, 1900);   // after the boot animation
});
// rear panel: shown on big screens, folded away on phones; the choice is remembered
const rearEl = $('#rear');
let rearOn = matchMedia('(min-width: 1000px) and (min-height: 700px)').matches;
try { const v = localStorage.getItem('vibe.rear'); if (v !== null) rearOn = v === '1'; } catch (e) { /* private mode */ }
function showRear(on) { rearOn = on; rearEl.hidden = !on; $('#tb-rear').classList.toggle('on', on); fit(); }
// cheat sheet: every firmware may bring firmware/<id>.cheat.html (built from firmware/<name>/cheatsheet.html), shown
// right of the device; on wide screens open by default, the choice is remembered
const cheatEl = $('#cheat');
let cheatOn = matchMedia('(min-width: 1300px)').matches;
try { const v = localStorage.getItem('vibe.cheat'); if (v !== null) cheatOn = v === '1'; } catch (e) { /* private mode */ }
async function loadCheat(fw) {
  let html = '';
  if (fw && fw.cheat) { try { const r = await fetch(fw.cheat, { cache: 'no-cache' }); if (r.ok) html = await r.text(); } catch (e) { /* offline */ } }
  cheatEl.innerHTML = html || `<h2>${(fw && fw.name) || 'Firmware'}</h2><p class="none">There is no cheat sheet for this firmware yet. The general controls are under <b>Help</b>.</p>`;
}
function showCheat(on) {
  cheatOn = on; cheatEl.hidden = !on; cheatEl.style.top = $('.topbar').offsetHeight + 8 + 'px';
  $('#tb-cheat').classList.toggle('on', on); fit();
}
$('#tb-cheat').addEventListener('click', () => { showCheat(!cheatOn); try { localStorage.setItem('vibe.cheat', cheatOn ? '1' : '0'); } catch (e) { /* private mode */ } });
$('#tb-rear').addEventListener('click', () => { showRear(!rearOn); try { localStorage.setItem('vibe.rear', rearOn ? '1' : '0'); } catch (e) { /* private mode */ } });
async function enableInput() {
  if (micNode) return;
  try {
    // two channels if the interface has them: channel 1 = IN 1, channel 2 = IN 2
    micStream = await navigator.mediaDevices.getUserMedia({ audio: { channelCount: { ideal: 2 }, echoCancellation: false, noiseSuppression: false, autoGainControl: false } });
    micNode = ctx.createMediaStreamSource(micStream); if (node) micNode.connect(node);
    const split = ctx.createChannelSplitter(2); micNode.connect(split);
    meters = [0, 1].map((i) => { const a = ctx.createAnalyser(); a.fftSize = 512; split.connect(a, i); return { a, buf: new Float32Array(512) }; });
    const stereo = micNode.channelCount > 1 || (micStream.getAudioTracks()[0].getSettings().channelCount || 1) > 1;
    $('#tb-in').classList.add('on'); rear.inputsOn(true); status(stereo ? 'IN 1 + IN 2 active' : 'IN 1 active (the source is mono, IN 2 stays empty)');
  } catch (e) { status('No access to the microphone', true); }
}
$('#tb-in').addEventListener('click', async () => { await powerOn(); enableInput(); });
$('#tb-media').addEventListener('click', (e) => mediaPick(e));
mediaRestore();

// ---------------------------------------------------------------- USB-MIDI ------------------------------------------
$('#tb-midi').addEventListener('click', async () => { await powerOn(); if (await ctl.connect()) $('#tb-midi').classList.add('on'); });
ctl.onWizard = (on) => {
  $('#tb-setup').classList.toggle('on', on);
  if (!on && PLUGIN && window.vibe_ctl_listen) { window.vibe_ctl_listen(0); ctlLearned(false); }   // (0.7.0: what was set up becomes a new profile)
};
const chSel = $('#midich'); chSel.value = String(ctl.channel); chSel.addEventListener('change', () => ctl.setChannel(chSel.value));
// (SDK 9) the firmware's MIDI out: a Web MIDI output, chosen here (the list fills once MIDI is connected)
const outSel = $('#midiout');
// SDK 15: a SysEx from a MIDI input – to the firmware (the plugin: through its queue, vibe_sysex_in; the browser: the worklet)
ctl.onSysex = (d) => {
  if (PLUGIN) { if (window.vibe_sysex_in) { let s = ''; for (const x of d) s += String.fromCharCode(x); window.vibe_sysex_in(btoa(s)); } return; }
  if (node && running) node.port.postMessage({ t: 'sysex', bytes: Array.from(d) });
};
ctl.onOutPorts = (ports, chosen) => { outSel.textContent = ''; const o0 = document.createElement('option'); o0.value = ''; o0.textContent = PLUGIN ? 'SysEx out: auto' : 'MIDI out: off'; outSel.appendChild(o0); for (const n of ports) { const o = document.createElement('option'); o.value = n; o.textContent = (PLUGIN ? 'SysEx out: ' : 'MIDI out: ') + n; outSel.appendChild(o); } outSel.value = ports.includes(chosen) ? chosen : ''; };
outSel.addEventListener('change', async () => { if (!ctl.access) { await powerOn(); await ctl.connect(); } ctl.setOut(outSel.value); });
$('#tb-setup').addEventListener('click', async () => {
  await powerOn(); if (!ctl.access) await ctl.connect();
  if (ctl.wiz) { ctl.stopWizard(false); $('#tb-setup').classList.remove('on'); return; }
  if (PLUGIN && window.vibe_ctl_listen) { window.vibe_ctl_listen(1); status('Setup: your controller into this track in the DAW (its MIDI input), then follow the steps'); }
  ctl.startWizard(); $('#tb-setup').classList.add('on');
});
ctl.onParam = (i, v) => { if (node && running) node.port.postMessage({ t: 'pset', i, v }); };   // (SDK 11: a mixer controller moves a mix_* parameter)
ctl.onLibrary = () => showMenu('Your controller asked for the library:');   // (a profile's "library" entry)
// ---------------------------------------------------------------- controller profiles (Protokoll 15a, 0.7.0) ----------
// Plugin: the controller plays into the DAW, the plugin applies the chosen profile itself (vibe_controller.cpp) – one per
// instance, kept in the project, the last one chosen for new instances; own ones in the "Controllers" folder beside
// "Firmware". Setup / Learn learn from the DAW's MIDI (the plugin hands it over meanwhile) and end in "Save your profile".
// Browser: the profile by the port's name, as before; loaded ones kept in this browser.
const ctlSel = $('#ctlsel'), ctlDlg = $('#ctldlg');
const isProfile = (o) => !!o && typeof o === 'object' && !Array.isArray(o) && Array.isArray(o.map);
let ctlActive = '', ctlKnown = [];
async function ctlRefresh() {
  ctlSel.textContent = '';
  const opt = (v, t, off) => { const o = document.createElement('option'); o.value = v; o.textContent = t; o.disabled = !!off; ctlSel.appendChild(o); };
  if (PLUGIN && window.vibe_ctl_list) {
    let l = { active: '', profiles: [] }; try { l = JSON.parse(await window.vibe_ctl_list()); } catch (e) { /* none */ }
    ctlActive = l.active || ''; ctlKnown = l.profiles || [];
    opt('', 'Controller: none');
    for (const p of ctlKnown) opt('p:' + p.name, 'Controller: ' + p.name + (p.project ? ' (this project\'s)' : p.own ? ' (yours)' : ''));
    opt('-', '──────────', true);
    opt('__setup', 'Set up a new one …'); opt('__load', 'Load a profile …'); opt('__folder', 'Open the profiles folder'); opt('__share', 'Share yours …');
    { const n = learnedIds().length; if (n) opt('__forget', 'Forget the learned knobs (' + n + ')'); }   // (MIDI-Learn in the Editor)
    ctlSel.value = ctlActive ? 'p:' + ctlActive : '';
    if (editor && editor.learned) editor.learned(learnedIds());
  } else {
    opt('', 'Controllers ▾');                         // (the browser takes the profile whose "match" is in the port's name)
    // (3.11.2026) what is plugged in, and the profile it plays with (or none: Setup makes one)
    for (const p of (ctl.ports || []).filter((x) => !/DAW/i.test(x.name))) opt('-', (p.profile ? '✓ ' + p.name + ' → ' + p.profile : '· ' + p.name + ' – no profile: Set up …'), true);
    if ((ctl.ports || []).length) opt('-', '──────────', true);
    opt('__setup', 'Set up a new one …'); opt('__load', 'Load a profile …'); opt('__save', 'Save yours ↓'); opt('__share', 'Share yours …');
    opt('__lpgrid', (lpGrid ? '✓ ' : '') + 'Launchpad as 8 × 8 pads');   // (3.11.2026: for firmwares made for a grid – the SOUNDMASCHINE)
    ctlSel.value = '';
  }
}
const errOf = (r) => { try { return JSON.parse(r).error || r; } catch (e) { return r; } };
// profile files (name, text): into the plugin's folder (and chosen) / this browser – false: none of them was a profile
async function ctlTakeFiles(list) {
  let n = 0, last = '';
  for (const f of list) {
    let o; try { o = JSON.parse(f.text); } catch (e) { continue; }
    if (Array.isArray(o)) o = o[0];
    if (!isProfile(o)) continue;
    if (!o.name) o.name = f.name.replace(/\.json$/i, '');
    if (PLUGIN) { const r = await window.vibe_ctl_save(JSON.stringify(o, null, 2) + '\n'); if (r !== 'ok') { status(f.name + ': ' + errOf(r), true); continue; } }
    else ctl.addOwn(o);
    n++; last = o.name;
  }
  if (!n) return false;
  await ctlRefresh();
  status('Controller profile "' + last + '" loaded – ' + (PLUGIN ? 'it maps your controller now (its MIDI into this track)' : 'it maps the controller whose port name it matches'));
  return true;
}
function ctlLoad() {
  if (window.vibe_pick_json) {                        // (plugin, macOS / Linux: the system's own dialog – WebKit's file input did nothing there)
    window.vibe_pick_json().then(async (s) => { let r; try { r = JSON.parse(s); } catch (e) { r = {}; } if (r.files && !(await ctlTakeFiles(r.files))) status('That was not a controller profile (a .json with a "map")', true); });
    return;
  }
  const inp = document.createElement('input'); inp.type = 'file'; inp.accept = '.json'; inp.multiple = true;
  inp.addEventListener('change', async () => { const fs = await Promise.all([...inp.files].map(async (f) => ({ name: f.name, text: await f.text() }))); if (fs.length && !(await ctlTakeFiles(fs))) status('That was not a controller profile (a .json with a "map")', true); });
  inp.click();
}
function ctlDownload(list) {                          // (browser) profiles as files
  for (const p of list) {
    const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([JSON.stringify(p, null, 2) + '\n'], { type: 'application/json' }));
    a.download = p.name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '') + '.json'; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  }
}
const SHARE = 'https://thefirmwarehub.com/controllers/';
function ctlShare() {
  if (PLUGIN) { window.vibe_ctl_open('share'); window.vibe_ctl_open('folder'); status('The hub\'s page and your profiles folder are open – upload your profile\'s .json there. Thank you!'); return; }
  const list = ctl.exportProfiles(); if (list.length) ctlDownload(list);
  window.open(SHARE, '_blank', 'noopener');
  status(list.length ? 'Your profile is downloaded – upload it on the hub\'s page. Thank you!' : 'Set your controller up first (Setup), then share it');
}
// (plugin) what Setup / Learn just learned from the DAW ("DAW|…" mappings): Setup – a new profile (a new controller);
// Learn – on top of the profile in use. Named in the dialog, saved into the folder and chosen; the mappings then live in it
const srcOf = (e) => (e.cc !== undefined ? 'c' + e.cc : e.note !== undefined ? 'n' + e.note : e.pitchbend !== undefined ? 'b' + e.pitchbend : '?') + '|' + (e.channel || 0);
function ctlLearned(onTop) {
  const got = ctl.exportProfiles().find((p) => p.name === 'DAW');
  if (!got || !got.map.length) return;
  const cur = onTop ? ctlKnown.find((p) => p.name === ctlActive) : null; let base = null; try { base = cur ? JSON.parse(cur.json) : null; } catch (e) { base = null; }
  const keys = new Set(got.map.map(srcOf));
  const map = [...(base && base.map ? base.map.filter((e) => !keys.has(srcOf(e))) : []), ...got.map];
  const prof = { ...(base || { format: 1, knobs: 'auto' }), map };
  $('#ctldlg-name').value = cur && cur.own ? ctlActive : '';
  $('#ctldlg-note').textContent = (base ? got.map.length + ' controls learned, on top of "' + ctlActive + '"' : got.map.length + ' controls learned') + ' – as a profile it maps your controller from now on, in every project.';
  ctlDlg.onclose = null;
  $('#ctldlg-cancel').onclick = () => ctlDlg.close();
  $('#ctldlg-form').onsubmit = async (ev) => {
    ev.preventDefault();
    const name = $('#ctldlg-name').value.trim(); if (!name) return;
    prof.name = name; prof.match = [name];
    const r = await window.vibe_ctl_save(JSON.stringify(prof, null, 2) + '\n');
    if (r !== 'ok') { status(errOf(r), true); return; }
    for (const k of Object.keys(ctl.bind)) if (k.startsWith('DAW|')) delete ctl.bind[k];
    ctl.save(); ctlDlg.close(); await ctlRefresh();
    status('Saved "' + name + '" – it maps your controller now. Controller ▾ → Share yours … sends it to us for everyone');
  };
  ctlDlg.showModal(); $('#ctldlg-name').focus();
}
ctlSel.addEventListener('change', async () => {
  const v = ctlSel.value;
  if (v === '' || v.startsWith('p:')) {
    if (!PLUGIN) { ctlSel.value = ''; return; }
    const r = await window.vibe_ctl_select(v.slice(2));
    if (r !== 'ok') status(errOf(r), true);
    else status(v ? 'Controller: ' + v.slice(2) + ' – plays into this track through the DAW and maps onto the VIBE' : 'No controller profile – the DAW\'s MIDI goes to the firmware as it is');
    await ctlRefresh(); return;
  }
  await ctlRefresh();                                 // (an action, not a choice: the choice shows again)
  if (v === '__setup') $('#tb-setup').click();
  else if (v === '__load') ctlLoad();
  else if (v === '__folder' && window.vibe_ctl_open) window.vibe_ctl_open('folder');
  else if (v === '__share') ctlShare();
  else if (v === '__lpgrid') {                       // the firmware starts again: it asks for the device once (vibe_device)
    lpGrid = !lpGrid; try { localStorage.setItem('vibe.lpgrid', lpGrid ? '1' : '0'); } catch (e) { /* private mode */ }
    ctl.lpGrid = lpGrid; ctl.lpState.clear(); ctl.lpHeld.clear(); await ctlRefresh();
    if (current && running) await boot(current);
    status(lpGrid ? 'Launchpad: its 8 × 8 is the firmware\'s pads now (64) – the top row and the right column stay VIBE buttons; the firmware started again'
                  : 'Launchpad: the VIBE layout again (4 × 4 pads, the buttons, two octaves of keys) – the firmware started again');
  }
  else if (v === '__forget') forgetLearned();
  else if (v === '__save') { const l = ctl.exportProfiles(); if (l.length) ctlDownload(l); else status('Nothing learned yet – connect your controller and press "Setup" first'); }
});
ctlRefresh();

// 0.7.0 – MIDI-Learn in the Editor: a right click on a control, then a knob moved on the controller: from then on it turns
// that parameter (the profile's entry "to":"param" with the firmware's id – the plugin applies it, window open or closed).
// Into the profile in use (an own one of the same name wins over a built-in one); none in use: a new one, named first.
let dawTap = null;                                    // (a learn waiting: the DAW's next CC is the knob)
const fwNow = () => (PLUGIN ? (current && current.id) || '' : '');
function learnedIds() {
  const cur = ctlKnown.find((p) => p.name === ctlActive); let o = null; try { o = cur ? JSON.parse(cur.json) : null; } catch (e) { o = null; }
  return o && Array.isArray(o.map) ? o.map.filter((e) => e.to === 'param' && (!e.firmware || e.firmware === fwNow())).map((e) => e.id) : [];
}
async function saveParamMap(change) {               // change(map) → the new map; into the profile in use, or a new one
  const cur = ctlKnown.find((p) => p.name === ctlActive); let base = null; try { base = cur ? JSON.parse(cur.json) : null; } catch (e) { base = null; }
  const prof = base || { format: 1, knobs: 'auto', map: [] };
  prof.map = change(prof.map || []);
  if (base) { const r = await window.vibe_ctl_save(JSON.stringify(prof, null, 2) + '\n'); if (r !== 'ok') { status(errOf(r), true); return false; } await ctlRefresh(); return true; }
  return new Promise((done) => {                      // (no profile yet: name it – the same dialog as after Setup)
    $('#ctldlg-name').value = ''; $('#ctldlg-note').textContent = 'Your controller has no profile yet – name it, and the knob you just learned is in it.';
    $('#ctldlg-cancel').onclick = () => { ctlDlg.close(); done(false); };
    $('#ctldlg-form').onsubmit = async (ev) => {
      ev.preventDefault(); const name = $('#ctldlg-name').value.trim(); if (!name) return;
      prof.name = name; prof.match = [name];
      const r = await window.vibe_ctl_save(JSON.stringify(prof, null, 2) + '\n'); if (r !== 'ok') { status(errOf(r), true); return; }
      ctlDlg.close(); await ctlRefresh(); done(true);
    };
    ctlDlg.showModal(); $('#ctldlg-name').focus();
  });
}
function forgetLearned() {                         // every knob learned onto this firmware's parameters, out of the profile
  const n = learnedIds().length; if (!n) return;
  saveParamMap((map) => map.filter((e) => !(e.to === 'param' && (!e.firmware || e.firmware === fwNow()))))
    .then((ok) => { if (ok) status(n + ' learned knob' + (n > 1 ? 's' : '') + ' forgotten – the controller turns the VIBE\'s encoders again'); });
}
function learnMenu(p, x, y) {
  document.querySelector('.ed-learnmenu')?.remove();
  const m = document.createElement('div'); m.className = 'ed-learnmenu'; m.style.left = x + 'px'; m.style.top = y + 'px';
  const item = (t, f) => { const b = document.createElement('button'); b.type = 'button'; b.textContent = t; b.onclick = () => { m.remove(); f(); }; m.appendChild(b); };
  item('Learn: move a knob on your controller', () => learnParam(p));
  if (learnedIds().length > 1) item('Forget all learned knobs', forgetLearned);
  if (learnedIds().includes(p.id)) item('Forget the controller\'s knob', async () => { if (await saveParamMap((map) => map.filter((e) => !(e.to === 'param' && e.id === p.id && (!e.firmware || e.firmware === fwNow()))))) status(p.name + ': no knob any more'); });
  document.body.appendChild(m);
  setTimeout(() => document.addEventListener('pointerdown', (e) => { if (!m.contains(e.target)) m.remove(); }, { once: true }), 0);
}
function learnParam(p) {
  if (!window.vibe_ctl_listen) return;
  window.vibe_ctl_listen(1); status('Learn: now move the knob on your controller for ' + p.name + ' …');
  const stop = setTimeout(() => { dawTap = null; window.vibe_ctl_listen(0); status('Learn: nothing came – is your controller on this track in the DAW?', true); }, 15000);
  dawTap = (m) => {
    if ((m[0] & 0xF0) !== 0xB0) return true;         // (only a knob – a CC; the rest is swallowed while learning)
    clearTimeout(stop); dawTap = null; window.vibe_ctl_listen(0);
    const e = { cc: m[1], channel: (m[0] & 15) + 1, to: 'param', id: p.id, firmware: fwNow(), label: p.name };
    saveParamMap((map) => [...map.filter((x) => !(x.cc === e.cc && (x.channel || 0) === e.channel) && !(x.to === 'param' && x.id === p.id && x.firmware === e.firmware)), e])
      .then((ok) => { if (ok) status('CC ' + e.cc + ' (channel ' + e.channel + ') turns ' + p.name + ' now – kept in "' + ctlActive + '"'); });
    return true;
  };
}
$('#tb-learn').addEventListener('click', async (e) => {
  await powerOn(); if (!ctl.access) await ctl.connect();
  if (e.shiftKey) { ctl.clear(); return; }                       // Shift + click: forget all mappings
  if (ctl.wizSkip()) return;                                     // while setting up: skip this step
  ctl.setLearning(!ctl.learning); $('#tb-learn').classList.toggle('on', ctl.learning);
  if (PLUGIN && window.vibe_ctl_listen) { window.vibe_ctl_listen(ctl.learning ? 1 : 0); if (!ctl.learning) ctlLearned(true); }   // (0.7.0: learned on top of the profile in use)
});

// ---------------------------------------------------------------- computer keyboard --------------------------------
const KB_WHITE = ['KeyZ', 'KeyX', 'KeyC', 'KeyV', 'KeyB', 'KeyN', 'KeyM', 'Comma', 'Period', 'Slash'], WOFF = [0, 2, 4, 5, 7, 9, 11];
const KB_BLACK = { KeyS: 1, KeyD: 3, KeyG: 6, KeyH: 8, KeyJ: 10, KeyL: 13, Semicolon: 15 };
const KB_PADS = ['Digit1', 'Digit2', 'Digit3', 'Digit4', 'Digit5', 'Digit6', 'Digit7', 'Digit8', 'KeyQ', 'KeyW', 'KeyE', 'KeyR', 'KeyT', 'KeyY', 'KeyU', 'KeyI'];
const KB_BTN = { ShiftLeft: BTN.SHIFT, ShiftRight: BTN.SHIFT, Tab: BTN.HOME, Enter: BTN.ENTER, Escape: BTN.RETURN, ArrowLeft: BTN.OCT_DOWN, ArrowRight: BTN.OCT_UP, KeyO: BTN.REC, Backspace: BTN.STOP,
  F7: BTN.MENU, F1: BTN.F1, F2: BTN.F2, F3: BTN.F3, F4: BTN.F4, F5: BTN.F5, F6: BTN.F6 };   // (as runtime/linux/panel.html; ↑ ↓ = DATA)
const keyId = (c) => { const w = KB_WHITE.indexOf(c); return w >= 0 ? Math.floor(w / 7) * 12 + WOFF[w % 7] : KB_BLACK[c]; };
let spacePlay = false;
const btnEl = (id) => panel.buttons[id];
// the plugin: the window has the focus (after a click on it), so the computer keyboard reaches the page, not the DAW –
// it plays the firmware as Ableton Live's computer MIDI keyboard would: A S D F G H J K the white keys, W E T Y U the
// black ones, Z / X the octave (25.10.: REC in OberVave took nothing – the keys went nowhere)
const DAW_KEYS = { KeyA: 0, KeyW: 1, KeyS: 2, KeyE: 3, KeyD: 4, KeyF: 5, KeyT: 6, KeyG: 7, KeyY: 8, KeyH: 9, KeyU: 10, KeyJ: 11, KeyK: 12, KeyO: 13, KeyL: 14, KeyP: 15, Semicolon: 16 };
const DAW_BTN = { KeyZ: BTN.OCT_DOWN, KeyX: BTN.OCT_UP };
window.addEventListener('keydown', (e) => {
  if (PLUGIN) {                                   // (all keys taken quietly: a key the page leaves unhandled makes macOS beep)
    if (e.target.closest && e.target.closest('select, input, dialog')) return;
    e.preventDefault();
    if (e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;
    if (DAW_KEYS[e.code] !== undefined) { send(EV.KEY, DAW_KEYS[e.code], 1, 0.8); panel.keyEl(DAW_KEYS[e.code])?.classList.add('down'); }
    else if (DAW_BTN[e.code] !== undefined) { send(EV.BUTTON, DAW_BTN[e.code], 1, 0); btnEl(DAW_BTN[e.code])?.classList.add('down'); }
    return;
  }
  if (e.metaKey || e.ctrlKey || e.altKey) return;
  if (e.target.closest && e.target.closest('select, input, dialog')) return;
  if (e.target.closest && e.target.closest('button, .knobwrap') && !['Tab', 'ShiftLeft', 'ShiftRight'].includes(e.code)) return;
  if (['Tab', 'Space', 'Backspace', 'ArrowUp', 'ArrowDown', 'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7'].includes(e.code)) e.preventDefault();
  if (e.code === 'PageUp' || e.code === 'PageDown') { e.preventDefault(); send(EV.ENCODER, ENC.B, 0, e.code === 'PageUp' ? 1 : -1); return; }   // encoder B (PLAYER: DJ filter)
  if (e.code === 'ArrowUp' || e.code === 'ArrowDown') { send(EV.ENCODER, ENC.DATA, 0, e.code === 'ArrowDown' ? 1 : -1); return; }   // DATA: ↓ = clockwise = down a list (held: keeps turning)
  if (e.repeat) return;
  if (e.code === 'KeyP') { rear.pedal(1); return; }
  if (e.code === 'Space') { spacePlay = !spacePlay; const id = spacePlay ? BTN.PLAY : BTN.STOP; send(EV.BUTTON, id, 1, 0); send(EV.BUTTON, id, 0, 0); return; }
  if (KB_BTN[e.code] !== undefined) { send(EV.BUTTON, KB_BTN[e.code], 1, 0); btnEl(KB_BTN[e.code])?.classList.add('down'); return; }
  const p = KB_PADS.indexOf(e.code); if (p >= 0) { send(EV.PAD, p, 1, 0.9); panel.pads[p].classList.add('down'); return; }
  const k = keyId(e.code); if (k !== undefined) { send(EV.KEY, k, 1, 0.85); panel.keyEl(k)?.classList.add('down'); }
});
window.addEventListener('keyup', (e) => {
  if (PLUGIN) {
    if (e.target.closest && e.target.closest('select, input, dialog')) return;
    e.preventDefault();
    if (DAW_KEYS[e.code] !== undefined) { send(EV.KEY, DAW_KEYS[e.code], 0, 0); panel.keyEl(DAW_KEYS[e.code])?.classList.remove('down'); }
    else if (DAW_BTN[e.code] !== undefined) { send(EV.BUTTON, DAW_BTN[e.code], 0, 0); btnEl(DAW_BTN[e.code])?.classList.remove('down'); }
    return;
  }
  if (e.code === 'KeyP') { rear.pedal(0); return; }
  if (KB_BTN[e.code] !== undefined) { send(EV.BUTTON, KB_BTN[e.code], 0, 0); btnEl(KB_BTN[e.code])?.classList.remove('down'); return; }
  const p = KB_PADS.indexOf(e.code); if (p >= 0) { send(EV.PAD, p, 0, 0); panel.pads[p].classList.remove('down'); return; }
  const k = keyId(e.code); if (k !== undefined) { send(EV.KEY, k, 0, 0); panel.keyEl(k)?.classList.remove('down'); }
});
// HOME held while switching on = firmware library (as on the device)
powerBtn.addEventListener('pointerdown', () => { holdHome = btnEl(BTN.HOME).classList.contains('down'); });

// ---------------------------------------------------------------- display refresh (30 fps) -------------------------
function loop(t) {
  if (node && running && !framePending && t - lastFrame > 32) { framePending = true; lastFrame = t; node.port.postMessage({ t: 'draw' }); }
  if (framePending && t - lastFrame > 500) framePending = false;   // a lost frame never blocks the display
  if (meters) rear.levels(...meters.map((m) => { m.a.getFloatTimeDomainData(m.buf); let p = 0; for (const v of m.buf) p = Math.max(p, Math.abs(v)); return Math.min(1, Math.sqrt(p)).toFixed(2); }));
  requestAnimationFrame(loop);
}
requestAnimationFrame(loop);

// ---------------------------------------------------------------- page bits ----------------------------------------
function status(text, bad) { const s = $('#status'); s.textContent = text; s.classList.toggle('bad', !!bad); }
function fit() {
  const vw = document.documentElement.clientWidth, vh = document.documentElement.clientHeight;
  const pad = vw < 700 || vh < 500 ? 6 : 28, top = $('.topbar').offsetHeight + 8, back = rearEl.hidden ? 0 : rearEl.offsetHeight + 12;
  const st = $('#status').offsetHeight ? $('#status').offsetHeight + 10 : 0;
  const side = !cheatEl.hidden && vw > 900 ? cheatEl.offsetWidth + 24 : 0;   // the cheat sheet takes the right edge
  $('.stage').style.paddingRight = side + 'px';
  const DH = panel.box ? BOX_H : 860;               // (the box view: no keys, no pads – lower)
  const s = Math.max(0.1, Math.min((vw - pad * 2 - side) / 1600, (vh - top - pad - st - back) / DH));
  dev.style.transform = `scale(${s})`; wrap.style.width = 1600 * s + 'px'; wrap.style.height = DH * s + 'px';
}
window.addEventListener('resize', fit); showRear(rearOn); showCheat(cheatOn);
// the box view (VIBE Box): no keys and pads, a bigger display – the plugin's own view by default; the choice is remembered
const BOX_KEY = PLUGIN ? 'vibe.plugin.box' : 'vibe.box';
// the three views, one switch (27.10.): Full view · Box view · Editor (the plugin, a firmware with parameters) – the one
// shown lights up, the names stay
let setEd = null;                                     // (pluginMode: the Editor on / off)
let checkEditor = null;                               // (pluginMode: the Editor only for a firmware it suits)
let bigOn = false;
const markView = () => {                              // (the dropdown shows the view there is)
  $('#tb-view').value = bigOn ? 'display' : editor && editor.open ? 'editor' : panel.box ? 'rack' : 'full';
};
const setBox = (on) => { panel.setBox(on); try { localStorage.setItem(BOX_KEY, on ? '1' : '0'); } catch (e) { /* storage off */ } fit(); markView(); };
const setBig = (on) => { bigOn = on; panel.showBig(on); markView(); };
panel.onBig = setBig;                                 // (a click on the picture: back to the view before)
// the View dropdown (27.10.): Editor (the plugin, a firmware with parameters) · Full · Rack (the box: no keys, no pads) ·
// Display (just the screen – over the view chosen before, which it goes back to)
$('#tb-view').addEventListener('change', async (e) => {
  const v = e.target.value;
  if (v === 'display') { await powerOn(); setBig(true); return; }
  if (bigOn) setBig(false);
  if (v === 'editor') { if (setEd) setEd(true); return; }
  if (setEd && editor.open) setEd(false);
  setBox(v === 'rack');
});
window.addEventListener('resize', () => panel.fitBig());
$('#tb-help').addEventListener('click', () => $('#help').showModal());
$('#help-close').addEventListener('click', () => $('#help').close());
$('#tb-full').addEventListener('click', () => { if (!document.fullscreenElement) document.documentElement.requestFullscreen?.(); else document.exitFullscreen?.(); });
// the Editor view (27.10.; the browser too since 31.10.2026): b = its bindings – the plugin's (window) or the browser's (edB)
function editorSetup(b, key) {
  editor = new Editor(b);
  displayTouch(editor.canvas, () => editor.canvas, (...a) => panel.emit(...a), () => panel.touchable()); crisp(editor.canvas);
  if (PLUGIN && window.vibe_ctl_save) editor.onLearn = (p, x, y) => learnMenu(p, x, y);   // (0.7.0: MIDI-Learn per parameter)
  window.vibeFocusMove = (c, pg, t) => { if (editor && editor.open) editor.focusMove(c || 0, pg || 0, t || 0); };   // (the controller: card, page, track)   // (the Editor's small display, too)
  const ED_KEY = key; { const o = $('#tb-view option[value=editor]'); o.hidden = false; o.disabled = false; }
  setEd = (on) => { document.body.classList.toggle('editing', on);
    editor.show(on); if (on && lastFb) editor.frame(lastFb); if (!PLUGIN && node) node.port.postMessage({ t: 'edwant', on });
    try { localStorage.setItem(ED_KEY, on ? '1' : '0'); } catch (e) { /* storage off */ } if (!on) fit(); markView(); };
  // the Editor is for native firmwares (their parameters in groups, or more than a converted FM-1 firmware's handful
  // of knobs): for the others it is greyed out, and a view on it goes to Rack (27.10.)
  checkEditor = async () => {
    let ok = false;
    let fm1 = false;
    try { const l = JSON.parse(await b.vibe_params()); ok = l.some((p) => p.group) || l.length > 7;
          fm1 = l.some((p) => p.id === 'algorithm') && l.some((p) => p.id === 'presets'); } catch (e) { ok = false; }
    panel.fm1 = fm1;                                  // (1.11.: a converted FM-1 firmware names its soft keys too – its display takes no clicks)
    const o = $('#tb-view option[value=editor]'); o.disabled = !ok; o.textContent = ok ? 'Editor' : 'Editor (native firmwares)';
    panel.touchOk = ok;                               // (31.10.: a native firmware's display takes clicks – soft keys, knob columns)
    if (!ok && editor.open) { setEd(false); setBox(true); }
    return ok;
  };
  let e0 = null; try { e0 = localStorage.getItem(ED_KEY); } catch (e) { /* storage off */ }
  // (31.10.: in the plugin the Editor is the first view of a firmware that has one – a DAW shows its controls; once a
  //  view was chosen, that one; the browser starts with the device)
  checkEditor().then((ok) => { if (ok && (e0 === '1' || (e0 === null && PLUGIN))) setEd(true); });
}
// the browser's answers to the Editor, by the plugin's names – from the worklet: the description once ('eddesc'), the
// values and the firmware's posts with every picture, a curve on asking ('curveR'); a page next to the .wasm
// (firmware/<id>.editor/index.html: tools/build.sh puts it there)
let edDesc = { params: [], editor: null }, edVals = [], edPosts = [], edPage = '', edSeq = 0;
const edWait = new Map();
const edPost = (m) => { if (node && running) node.port.postMessage(m); };
const edB = {
  vibe_params: async () => JSON.stringify(edDesc.params.map((p, i) => Object.assign({}, p, { v: edVals[i] ?? p.v }))),
  vibe_param_values: async () => JSON.stringify(edVals.length ? edVals : edDesc.params.map((p) => p.v)),
  vibe_param_set: (i, v) => { edVals[i] = v; edPost({ t: 'pset', i, v }); },
  vibe_editor: async () => JSON.stringify(edDesc.editor),
  vibe_editor_curve: (i, tab) => new Promise((res) => { if (!node || !running) { res('[]'); return; } const id = ++edSeq; edWait.set(id, res); node.port.postMessage({ t: 'curve', i, tab, id }); }),
  vibe_editor_curve_draw: (i, tab, point, count, value) => edPost({ t: 'cdraw', i, tab, point, count, value }),
  vibe_editor_press: (i, tab) => edPost({ t: 'press', i, tab }),
  vibe_editor_page: async () => edPage,
  vibe_editor_send: (b64) => { const s = atob(b64), u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i); edPost({ t: 'eddata', bytes: u }); },
  vibe_editor_take: async () => { const r = edPosts.map((u) => { let s = ''; for (const x of u) s += String.fromCharCode(x); return btoa(s); }); edPosts = []; return JSON.stringify(r); },
  vibe_ev: (type, id, down, value, midi) => send(type, id, !!down, value, midi),
};
async function edDescribed(d) {                      // (a firmware started: its description, its page – if it has one)
  edDesc = { params: d.params || [], editor: d.editor }; edVals = edDesc.params.map((p) => p.v); edPosts = [];
  edPage = '';
  const f = current && current.file;
  if (f && /\.wasm(\?|$)/.test(f)) {
    const url = f.replace(/\.wasm(\?.*)?$/, '.editor/index.html');
    try { const r = await fetch(url, { method: 'HEAD', cache: 'no-cache' }); if (r.ok) edPage = url + '?v=' + Date.now(); } catch (e) { /* no page */ }
  }
  if (checkEditor && await checkEditor() && editor.open) { node.port.postMessage({ t: 'edwant', on: true }); editor.reload(); }
}
if (!PLUGIN && !REMOTE) editorSetup(edB, 'vibe.browser.editor');

if (PLUGIN) pluginMode(); else if (REMOTE) remote(); else loadLibrary();
window.__vibe = { panel, rear, ctl, get node() { return node; }, get ctx() { return ctx; }, boot, powerOn,
  mediaAdd: (h, write) => { mediaSrc.push({ key: mediaKey(h.name || 'opfs'), label: h.name || 'OPFS', write: !!write, h, ok: true, dropped: true }); mediaPush(); } };   // (tests: an OPFS folder as media)

// ---------------------------------------------------------------- device mode (served by a VIBE) ------------------
async function remote() {
  power.classList.add('gone'); document.body.classList.add('remote');
  for (const id of ['#fw', '#tb-in', '#tb-rear', '#tb-cheat', '#tb-media']) { const el = $(id); if (el) el.style.display = 'none'; }
  showRear(false); showCheat(false);
  try { const p = await navigator.permissions.query({ name: 'midi', sysex: true }); if (p.state === 'granted') ctl.connect(); } catch (e) { /* no permissions API */ }
  const apply = frameSink();
  const inflate = async (u8) => new Uint8Array(await new Response(new Blob([u8]).stream().pipeThrough(new DecompressionStream('deflate'))).arrayBuffer()).buffer;
  try { const v = +(await (await fetch('/vol')).text()); if (v >= 0 && v <= 1) panel.setVolume(Math.sqrt(v) >= 0 ? v : 0.8); } catch (e) { /* older runtime */ }
  for (;;) {
    try {
      const r = await fetch('/stream'); if (!r.ok || !r.body) throw new Error('no stream');
      status('VIBE · live · ' + location.host + ' – sound plays on the device'); const rd = r.body.getReader(); let acc = new Uint8Array(0);
      for (;;) {
        const { value, done } = await rd.read(); if (done) throw new Error('closed');
        const t = new Uint8Array(acc.length + value.length); t.set(acc); t.set(value, acc.length); acc = t;
        while (acc.length >= 5) {
          const len = acc[0] | acc[1] << 8 | acc[2] << 16 | acc[3] << 24; if (acc.length < 4 + len) break;
          const z = acc[4], body = acc.slice(5, 4 + len); acc = acc.slice(4 + len);
          apply(z ? await inflate(body) : body.buffer);
        }
      }
    } catch (e) { status('VIBE not reachable – trying again …', true); await new Promise((res) => setTimeout(res, 1000)); }
  }
}

// one packet of the device's picture (VIBE runtime /stream, or the plugin): u32 frame, u16 tiles, 2 pad; per tile u8 x,
// u8 y, 32 x 32 RGB565; then the lights (384), the rear (24), the captions (25 x 8). Returns the function that takes one.
function frameSink() {
  const fb = new Uint16Array(480 * 320); let leds = null, labels = null, dirty = false;
  lastFb = fb;
  (function paint() { if (dirty) { panel.showFrame(fb); dirty = false; } requestAnimationFrame(paint); })();
  const apply = (buf) => {
    const dv = new DataView(buf), n = dv.getUint16(4, true); let p = 8;
    for (let t = 0; t < n; t++) {
      const tx = dv.getUint8(p), ty = dv.getUint8(p + 1), src = new Uint16Array(buf.slice(p + 2, p + 2 + 2048));
      for (let y = 0; y < 32; y++) fb.set(src.subarray(y * 32, y * 32 + 32), (ty * 32 + y) * 480 + tx * 32);
      p += 2050;
    }
    if (n) { dirty = true; if (editor && editor.open) editor.frame(fb); }
    const l = buf.slice(p, p + 384); if (!leds || !sameBuf(l, leds)) { leds = l; panel.showLeds(l); ctl.lights(l); }
    const lb = buf.slice(p + 384 + 24, p + 384 + 24 + 200); if (!labels || !sameBuf(lb, labels)) { labels = lb; panel.showLabels(lb); }
  };
  const sameBuf = (a, b) => { const x = new Uint8Array(a), y = new Uint8Array(b); if (x.length !== y.length) return false; for (let i = 0; i < x.length; i++) if (x[i] !== y[i]) return false; return true; };
  return apply;
}


// ---------------------------------------------------------------- plugin mode (the VIBE FM Plugin's window) ----------
async function pluginMode() {
  power.classList.add('gone'); document.body.classList.add('remote', 'plugin');
  panel.setBrand('VST');                              // (the plugin: VIBE FM VST)
  panel.printLabels = true; panel.labelKey = null;    // (the firmware's captions printed on the coloured keys and F1–F6)
  { let b = null; try { b = localStorage.getItem(BOX_KEY); } catch (e) { /* storage off */ } if (b === null) setBox(true); }   // (the plugin starts in the box view)
  $('.brand').innerHTML = '<b>VIBE</b> <em>FM</em> <span>VST</span> <small class="claim">MAKE FIRMWARE <i>NOT WAR</i></small>';
  $('.brand').insertAdjacentElement('afterend', $('.viewsel'));   // (31.10.: View on the left, beside the name – in the tools on the right it was hard to find)
  for (const id of ['#tb-in', '#tb-rear', '#tb-midi', '#midiout', '#tb-media', '#midich']) { const el = $(id); if (el) el.style.display = 'none'; }   // (the plugin: its media folder is in its settings; the DAW picks the channels)   // (the DAW has the audio and MIDI – and the firmware's MIDI out goes to its note port)
  showRear(false);
  const apply = frameSink();
  let got = 0;
  window.vibePacket = (b64) => {
    const s = atob(b64), u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i);
    apply(u.buffer);
    if (++got === 1 && window.vibe_log) window.vibe_log('first picture arrived (' + u.length + ' bytes)');
  };
  window.vibeShowLibrary = () => showMenu('The firmware asked for the library:');
  if (window.vibe_params) editorSetup(window, 'vibe.plugin.editor');   // (27.10.) the Editor view: every parameter at once
  {                                                   // (the whole window takes the drop – a file let go anywhere would else open in the view)
    // SDK 14: a firmware that takes samples (vibe_media_dropped) – audio files go to it, onto the pad or the display spot
    // they are let go on (the device stays visible: a slim hint instead of the firmware sheet)
    let n = 0, mark = null; const off = () => { n = 0; document.body.classList.remove('dropping'); if (mark) mark.classList.remove('droptarget'); mark = null; };
    const askTakes = () => (window.vibe_drop ? window.vibe_drop('takes', 0, '', -1, -1, -1) : Promise.resolve('0'))
      .then((r) => { document.body.classList.toggle('drop-media', r === '1'); }, () => {});
    window.addEventListener('dragenter', (e) => { e.preventDefault(); if (n++ === 0) askTakes(); document.body.classList.add('dropping'); });
    window.addEventListener('dragover', (e) => {
      e.preventDefault(); if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy';
      const t = document.body.classList.contains('drop-media') ? dropTarget(e) : null, el = t && t.el;
      if (el !== mark) { if (mark) mark.classList.remove('droptarget'); if (el) el.classList.add('droptarget'); mark = el; }
    });
    window.addEventListener('dragleave', () => { if (--n <= 0) off(); });
    window.addEventListener('drop', (e) => {
      e.preventDefault(); off();
      const n = e.dataTransfer ? e.dataTransfer.files.length : -1;
      if (window.vibe_log) window.vibe_log('drop: ' + n + ' file(s)');   // (0.5.0: a drop that did nothing on macOS 27 – what arrives)
      if (n <= 0) return;
      const files = [...e.dataTransfer.files], sounds = files.filter((f) => AUDIO.test(f.name)), rest = files.filter((f) => !AUDIO.test(f.name));
      if (sounds.length) pluginDropSamples(sounds, dropTarget(e));
      if (rest.length || !sounds.length) pluginInstall(rest.length ? rest : files);
    });
  }
  // the plugin's MIDI: the DAW's (while Setup / Learn listen – 0.7.0, every system) and on the Mac the controllers the
  // window reads itself (CoreMIDI: the Launchpad's lights; the others the plugin maps, see controllers.js generic)
  const ownPorts = pluginMidi(); ctl.plugin = true;   // (the DAW plays the notes: the window only learns)
  ctl.connect().then((ok) => {
    if (ok && ownPorts) $('#tb-midi').classList.add('on');
    if (!ok || !window.vibe_sysex_take) return;
    // (0.7.2) SDK 15: the firmware's SysEx out to the window's MIDI ports (#midiout: one, or auto = every two-way device)
    outSel.style.display = '';
    setInterval(async () => {
      let l = []; try { l = JSON.parse(await window.vibe_sysex_take()); } catch (e) { return; }
      for (const s of l) { const b = atob(s), u = new Uint8Array(b.length); for (let i = 0; i < b.length; i++) u[i] = b.charCodeAt(i); ctl.sendSysex(u); }
    }, 40);
  });
  if (window.vibe_log) window.vibe_log('plugin mode started');
  if (window.vibe_ready) window.vibe_ready();         // (now the whole picture: one sent before this point is lost)
  await pluginLibrary();
}
// Web MIDI as the plugin delivers it (vibe_midi_ports / vibeMidi): the same MIDIAccess shape controllers.js expects
// (0.7.2) no CoreMIDI binding (Windows: WebView2 has Web MIDI itself): its ports as they are, plus the DAW's input – since
// 0.7.0 the window saw only the DAW there, its controllers and an FM-1's SysEx were gone (Andrew GG, issue #2)
function pluginMidi() {
  const inputs = new Map(), outputs = new Map(), access = { inputs, outputs, onstatechange: null };
  const native = !window.vibe_midi_ports && navigator.requestMIDIAccess ? navigator.requestMIDIAccess.bind(navigator) : null;
  let nat = null;
  const LEN = (b) => (b >= 0xF8 || b === 0xF6 || b === 0xF4 || b === 0xF5) ? 1 : (b & 0xF0) === 0xC0 || (b & 0xF0) === 0xD0 || b === 0xF1 || b === 0xF3 ? 2 : 3;
  const sync = async () => {
    let l = { in: [], out: [] }; if (window.vibe_midi_ports) try { l = JSON.parse(await window.vibe_midi_ports()); } catch (e) { /* none */ }
    const keep = new Map(); inputs.forEach((p) => keep.set(p.id, p.onmidimessage));
    inputs.clear(); outputs.clear();
    l.in.forEach((name, i) => inputs.set('in' + i, { id: 'in' + i, name, onmidimessage: keep.get('in' + i) || null }));
    l.out.forEach((name, i) => outputs.set('out' + i, { id: 'out' + i, name, send: (d) => window.vibe_midi_send(i, Array.from(d)) }));
    if (nat) { nat.inputs.forEach((p) => inputs.set(p.id, p)); nat.outputs.forEach((p) => outputs.set(p.id, p)); }   // (the real ports: their handlers stay on them)
    inputs.set('daw', { id: 'daw', name: 'DAW', onmidimessage: keep.get('daw') || null });   // (0.7.0: the DAW's MIDI, while Setup / Learn listen)
  };
  const open = async (o) => {
    if (!native || nat) return;
    try { nat = await native(o || { sysex: true }); } catch (e) { try { nat = await native(); } catch (e2) { return; } }
    nat.onstatechange = () => { sync().then(() => { if (access.onstatechange) access.onstatechange(); }); };
  };
  window.vibeDawMidi = (list) => { const p = inputs.get('daw'); for (const m of list) { if (dawTap && dawTap(m)) continue; if (p && p.onmidimessage) p.onmidimessage({ data: Uint8Array.from(m) }); } };
  window.vibeMidi = (i, bytes) => {                   // one CoreMIDI packet: split into messages (running status kept)
    const port = inputs.get('in' + i); if (!port || !port.onmidimessage) return;
    let k = 0, run = 0;
    while (k < bytes.length) {
      let st = bytes[k], m;
      if (st === 0xF0) { const e = bytes.indexOf(0xF7, k); m = bytes.slice(k, e < 0 ? bytes.length : e + 1); }
      else if (st < 0x80) { if (!run) { k++; continue; } m = [run, ...bytes.slice(k, k + LEN(run) - 1)]; k -= 1; }
      else { if (st < 0xF0) run = st; m = bytes.slice(k, k + LEN(st)); }
      k += m.length; port.onmidimessage({ data: Uint8Array.from(m) });
    }
  };
  window.vibeMidiPorts = async () => { await sync(); if (access.onstatechange) access.onstatechange(); };
  Object.defineProperty(navigator, 'requestMIDIAccess', { configurable: true, value: async (o) => { await open(o); await sync(); return access; } });
  return !!(window.vibe_midi_ports || native);        // (the window reads controllers itself)
}
async function pluginLibrary() {
  try { library = JSON.parse(await window.vibe_library()); } catch (e) { library = []; }
  const cur = await window.vibe_current();
  current = library.find((f) => f.id === cur) || null;
  $('#fw').innerHTML = (current ? '' : `<option value="" disabled selected>${library.length ? 'Choose a firmware …' : 'No firmware yet'}</option>`) +
    library.map((f) => `<option value="${f.id}">${f.name} ${f.version || ''}</option>`).join('') +
    (window.vibe_install ? '<option value="__add">Add firmware …</option>' : '');
  const build = window.vibe_build ? await window.vibe_build() : 'VIBE FM VST';
  if (current) { $('#fw').value = current.id; loadCheat(current); status(build + ' · ' + current.name + ' ' + (current.version || '')); }
  else if (!library.length) status('No firmware yet – get some at thefirmwarehub.com/convert, then drop the .zip onto this window', true);
  else status(cur ? `${cur} is not in the firmware folder – choose one above` : 'Choose a firmware above', !!cur);   // (a new instance plays nothing until one is chosen)
}
// SDK 14: where a drop lands – a pad (the full view), else a spot on the display (480 x 320), else nowhere in particular
function dropTarget(e) {
  const hit = (el) => { if (!el || el.offsetParent === null) return null; const r = el.getBoundingClientRect(); return r.width > 0 && e.clientX >= r.left && e.clientX < r.right && e.clientY >= r.top && e.clientY < r.bottom ? r : null; };
  for (let i = 0; i < panel.pads.length; i++) if (hit(panel.pads[i])) return { pad: i, x: -1, y: -1, el: panel.pads[i] };
  for (const c of [panel.bigCanvas, panel.canvas]) {
    const r = hit(c); if (r) return { pad: -1, x: Math.min(479, Math.floor((e.clientX - r.left) * 480 / r.width)), y: Math.min(319, Math.floor((e.clientY - r.top) * 320 / r.height)), el: c };
  }
  return { pad: -1, x: -1, y: -1, el: null };
}
// SDK 14: samples dropped onto the window go to the firmware (vibe_drop, see the plugin's dropFile): WAV and MP3 as they
// are, the other formats the WebView can read (AIFF, FLAC, M4A …) as a 24-bit WAV. In pieces of 3 MB (base64 over the
// bridge). Several files dropped on a pad: that pad and the ones after it
async function pluginDropSamples(files, at) {
  if (!window.vibe_drop) { status('This plugin cannot take samples yet – update VIBE FM VST', true); return; }
  if (await window.vibe_drop('takes', 0, '', -1, -1, -1) !== '1') { status((current ? current.name : 'This firmware') + ' takes no samples by drag & drop', true); return; }
  const CH = 3 << 20, done = [], errors = [];
  for (let i = 0; i < files.length; i++) {
    const f = files[i], keep = KEEP.test(f.name), pad = at.pad >= 0 ? Math.min(15, at.pad + i) : -1;
    const name = keep ? f.name : f.name.replace(/\.[^.]*$/, '') + '.wav';
    status('Sample ' + (i + 1) + ' / ' + files.length + ': ' + f.name + (keep ? '' : ' → WAV') + ' …');
    let data;
    try { data = new Uint8Array(keep ? await f.arrayBuffer() : wav24(await new OfflineAudioContext(2, 1, 48000).decodeAudioData(await f.arrayBuffer()))); }
    catch (err) { errors.push(f.name + ': cannot read it'); continue; }
    let r = JSON.parse(await window.vibe_drop('begin', data.length, name, -1, -1, -1));
    if (r.error) { errors.push(f.name + ': ' + r.error); continue; }
    const id = r.id;
    for (let o = 0; o < data.length && !r.error; o += CH) {
      const u = data.subarray(o, o + CH); let bin = '';
      for (let k = 0; k < u.length; k += 0x8000) bin += String.fromCharCode.apply(null, u.subarray(k, k + 0x8000));
      r = JSON.parse(await window.vibe_drop('data', id, btoa(bin), -1, -1, -1));
      if (data.length > CH) status('Sample ' + (i + 1) + ' / ' + files.length + ': ' + f.name + ' ' + Math.round(Math.min(100, (o + CH) * 100 / data.length)) + ' %');
    }
    if (!r.error) r = JSON.parse(await window.vibe_drop('end', id, '', pad, at.x, at.y));
    else window.vibe_drop('cancel', id, '', -1, -1, -1);
    if (r.error) errors.push(f.name + ': ' + r.error); else done.push(f.name);
  }
  if (done.length) status((done.length === 1 ? done[0] : done.length + ' samples') + ' → ' + (current ? current.name : 'the firmware') + (at.pad >= 0 ? ', pad ' + (at.pad + 1) : '') + (errors.length ? ' · ' + errors.join(' · ') : ''), !!errors.length);
  else status(errors.join(' · ') || 'Nothing dropped', true);
}
// 0.2.0: firmware into the plugin – the hub's .zip (or an .wasm / .json) dropped onto the window or chosen; the plugin
// writes <id>.wasm + <id>.json into its firmware folder, the list is read again and the (first) new one starts
async function pluginInstall(files) {
  files = [...files].filter((f) => /\.(zip|wasm|json)$/i.test(f.name));
  {                                                   // (0.7.0) a controller profile (.json with a "map"): into the profiles, not the firmwares
    const prof = [];
    for (const f of files.filter((x) => /\.json$/i.test(x.name))) { const text = await f.text(); try { const o = JSON.parse(text); if (isProfile(Array.isArray(o) ? o[0] : o)) prof.push({ f, text }); } catch (e) { /* a firmware's card */ } }
    if (prof.length) { await ctlTakeFiles(prof.map((x) => ({ name: x.f.name, text: x.text }))); files = files.filter((f) => !prof.some((x) => x.f === f)); if (!files.length) return; }
  }
  if (!files.length) { status('Drop the firmware\'s .zip from thefirmwarehub.com (or its .wasm)', true); return; }
  if (!window.vibe_install) { status('This plugin cannot install yet – copy the .wasm into the VIBE FM firmware folder', true); return; }
  hideMenu(); status('installing ' + files.map((f) => f.name).join(', ') + ' …');
  const ids = [], errors = [];
  for (const f of files.sort((a, b) => (/\.json$/i.test(a.name) ? 1 : 0) - (/\.json$/i.test(b.name) ? 1 : 0))) {   // (a card after its .wasm)
    const u = new Uint8Array(await f.arrayBuffer()); let bin = '';
    for (let i = 0; i < u.length; i += 0x8000) bin += String.fromCharCode.apply(null, u.subarray(i, i + 0x8000));
    let r; try { r = JSON.parse(await window.vibe_install(f.name, btoa(bin))); } catch (e) { r = { error: String(e.message || e) }; }
    if (r.ids) ids.push(...r.ids); else if (!/\.json$/i.test(f.name) || files.length === 1) errors.push(f.name + ': ' + r.error);
  }
  await pluginInstalled(ids, errors);
}
async function pluginInstalled(ids, errors) {
  await pluginLibrary();
  if (ids.length) { const r = await window.vibe_load(ids[0]); if (r !== 'ok') { status(r, true); return; } await pluginLibrary(); status('Installed ' + ids.map((id) => (library.find((f) => f.id === id) || { name: id }).name).join(', ') + ' – it is in the list now' + (errors.length ? ' · ' + errors.join(' · ') : ''), !!errors.length); }
  else status(errors.join(' · ') || 'Nothing installed', true);
}
function pluginPick() {
  if (window.vibe_pick) {                             // 0.5.0 (macOS): the Mac's own open panel, the plugin installs what was chosen
    hideMenu();
    window.vibe_pick().then((s) => {
      let r; try { r = JSON.parse(s); } catch (e) { r = { error: String(e.message || e) }; }
      if (!r.cancel) pluginInstalled(r.ids || [], r.error ? [r.error] : []);
    });
    return;
  }
  let inp = $('#fwadd');
  if (!inp) {
    inp = document.createElement('input'); inp.type = 'file'; inp.id = 'fwadd'; inp.accept = '.zip,.wasm,.json'; inp.multiple = true; inp.hidden = true;
    inp.addEventListener('change', () => { const f = [...inp.files]; inp.value = ''; if (f.length) pluginInstall(f); });
    document.body.appendChild(inp);
  }
  inp.click();
}
async function pluginLoad(id) {
  hideMenu(); status('loading …');
  const r = await window.vibe_load(id);
  if (r !== 'ok') { status(r, true); return; }
  await pluginLibrary();
  if (checkEditor && await checkEditor() && editor.open) editor.reload();   // (the new firmware's parameters – or Rack)
}

// host.js – the browser runtime ("the device") for VIBE firmwares:
//   loads a firmware .wasm, starts it in the audio thread (worklet.js), forwards every control from the panel,
//   the computer keyboard and USB-MIDI as SDK events, shows the display frames and LEDs it sends back,
//   connects IN 1/IN 2 when a firmware asks, runs the rear panel (pedal, CV/Gate, clock) and offers the firmware
//   library (HOME at power-on, or the menu).
import { Panel, EV, BTN, ENC, BOX_H } from './panel.js';
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
function send(type, id, down, value, midi = 0) {
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
  node = new AudioWorkletNode(ctx, 'vibe-host', { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], processorOptions: { module, store } });
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
    running = true; ctl.sdk = d.sdk; status((current.name || 'Firmware') + ' running · VIBE SDK ' + d.sdk);
    node.port.postMessage({ t: 'cvin', volts: +rear.cvKnob.value });
  }
  else if (d.t === 'frame') { framePending = false; panel.showFrame(new Uint16Array(d.fb)); panel.showLeds(d.leds); panel.showLabels(d.labels); ctl.lights(d.leds); rear.show(d.rear); }
  else if (d.t === 'log') console.log('[firmware]', d.text);
  else if (d.t === 'midi') ctl.midiOut(d.msgs, ctx);   // (SDK 9) the firmware's MIDI out – to the chosen Web MIDI port
  else if (d.t === 'store') { const sp = storeSpace(current); storeQ = storeQ.then(() => storePut(sp, d.name, d.data)); }
  else if (d.t === 'request') { if (d.what === 1) showMenu(); if (d.what === 2) enableInput(); if (d.what === 3) { const fw = current; storeQ.then(() => boot(fw)); } }   // 3: restart, once its writes / deletes are stored
  else if (d.t === 'crash') { running = false; status('Firmware stopped: ' + d.error, true); showMenu('The firmware crashed – not the device. Choose a firmware:'); }
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

// the library menu is drawn by the runtime over the display (like the ROM boot menu on the device)
function showMenu(note) {
  const m = panel.menu;
  m.innerHTML = `<h4>FIRMWARE · microSD</h4>${note ? `<p>${note}</p>` : ''}`;
  library.forEach((f) => {
    const b = document.createElement('button'); b.type = 'button'; if (current && current.id === f.id) b.className = 'cur';
    b.innerHTML = `<span>${f.name} ${f.version || ''}</span><small>${f.description || ''}</small>`;
    b.addEventListener('click', () => (PLUGIN ? pluginLoad(f.id) : boot(f))); m.appendChild(b);
  });
  const own = document.createElement('button'); own.type = 'button';
  own.innerHTML = PLUGIN ? '<span>ADD FIRMWARE …</span><small>choose the .zip from thefirmwarehub.com – or drop it onto this window</small>'
    : '<span>YOUR OWN FIRMWARE …</span><small>choose a .wasm or drop it onto the device</small>';
  own.addEventListener('click', () => (PLUGIN ? pluginPick() : $('#fwfile').click()));
  m.appendChild(own);
  const back = document.createElement('button'); back.type = 'button'; back.innerHTML = '<span>↩ BACK</span><small></small>';
  back.addEventListener('click', hideMenu); m.appendChild(back);
  m.classList.add('on');
}
function hideMenu() { panel.menu.classList.remove('on'); }
$('#fw').addEventListener('change', async (e) => { if (PLUGIN) { if (e.target.value === '__add') { $('#fw').value = current ? current.id : ''; pluginPick(); return; } pluginLoad(e.target.value); return; } if (e.target.value === '__file') { $('#fwfile').click(); return; } await powerOn(); boot(library.find((f) => f.id === e.target.value)); });
$('#fwfile').addEventListener('change', async (e) => { const f = e.target.files[0]; if (!f) return; await powerOn(); boot({ name: f.name.replace(/\.wasm$/, ''), bytes: await f.arrayBuffer() }); e.target.value = ''; });
wrap.addEventListener('dragover', (e) => { e.preventDefault(); if (!PLUGIN) wrap.classList.add('drop'); });
wrap.addEventListener('dragleave', () => wrap.classList.remove('drop'));
wrap.addEventListener('drop', async (e) => {
  e.preventDefault(); wrap.classList.remove('drop');
  if (PLUGIN) return;                                 // (the plugin: the window's own handler installs it, see pluginMode)
  const entries = [...e.dataTransfer.items].map((it) => it.webkitGetAsEntry && it.webkitGetAsEntry()).filter(Boolean);   // (now: they expire after the first await)
  const plain = [...e.dataTransfer.files], fw = plain.find((f) => /\.wasm$/i.test(f.name));
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

// ---------------------------------------------------------------- USB-MIDI ------------------------------------------
$('#tb-midi').addEventListener('click', async () => { await powerOn(); if (await ctl.connect()) $('#tb-midi').classList.add('on'); });
ctl.onWizard = (on) => $('#tb-setup').classList.toggle('on', on);
const chSel = $('#midich'); chSel.value = String(ctl.channel); chSel.addEventListener('change', () => ctl.setChannel(chSel.value));
// (SDK 9) the firmware's MIDI out: a Web MIDI output, chosen here (the list fills once MIDI is connected)
const outSel = $('#midiout');
ctl.onOutPorts = (ports, chosen) => { outSel.textContent = ''; const o0 = document.createElement('option'); o0.value = ''; o0.textContent = 'MIDI out: off'; outSel.appendChild(o0); for (const n of ports) { const o = document.createElement('option'); o.value = n; o.textContent = 'MIDI out: ' + n; outSel.appendChild(o); } outSel.value = ports.includes(chosen) ? chosen : ''; };
outSel.addEventListener('change', async () => { if (!ctl.access) { await powerOn(); await ctl.connect(); } ctl.setOut(outSel.value); });
$('#tb-setup').addEventListener('click', async () => {
  await powerOn(); if (!ctl.access) await ctl.connect();
  if (ctl.wiz) { ctl.stopWizard(false); $('#tb-setup').classList.remove('on'); return; }
  ctl.startWizard(); $('#tb-setup').classList.add('on');
});
$('#tb-learn').addEventListener('click', async (e) => {
  await powerOn(); if (!ctl.access) await ctl.connect();
  if (e.shiftKey) { ctl.clear(); return; }                       // Shift + click: forget all mappings
  if (ctl.wizSkip()) return;                                     // while setting up: skip this step
  ctl.setLearning(!ctl.learning); $('#tb-learn').classList.toggle('on', ctl.learning);
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
if (PLUGIN) pluginMode(); else if (REMOTE) remote(); else loadLibrary();
window.__vibe = { panel, rear, ctl, get node() { return node; }, get ctx() { return ctx; }, boot, powerOn };

// ---------------------------------------------------------------- device mode (served by a VIBE) ------------------
async function remote() {
  power.classList.add('gone'); document.body.classList.add('remote');
  for (const id of ['#fw', '#tb-in', '#tb-rear', '#tb-cheat']) { const el = $(id); if (el) el.style.display = 'none'; }
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
  for (const id of ['#tb-in', '#tb-rear', '#tb-midi', '#midiout']) { const el = $(id); if (el) el.style.display = 'none'; }   // (the DAW has the audio and MIDI – and the firmware's MIDI out goes to its note port)
  showRear(false);
  const apply = frameSink();
  let got = 0;
  window.vibePacket = (b64) => {
    const s = atob(b64), u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i);
    apply(u.buffer);
    if (++got === 1 && window.vibe_log) window.vibe_log('first picture arrived (' + u.length + ' bytes)');
  };
  window.vibeShowLibrary = () => showMenu('The firmware asked for the library:');
  if (window.vibe_params) {                           // (27.10.) the Editor view: every parameter at once
    editor = new Editor();
    const ED_KEY = 'vibe.plugin.editor'; { const o = $('#tb-view option[value=editor]'); o.hidden = false; o.disabled = false; }
    setEd = (on) => { document.body.classList.toggle('editing', on);
      editor.show(on); if (on && lastFb) editor.frame(lastFb); try { localStorage.setItem(ED_KEY, on ? '1' : '0'); } catch (e) { /* storage off */ } if (!on) fit(); markView(); };
    // the Editor is for native firmwares (their parameters in groups, or more than a converted FM-1 firmware's handful
    // of knobs): for the others it is greyed out, and a view on it goes to Rack (27.10.)
    checkEditor = async () => {
      let ok = false;
      try { const l = JSON.parse(await window.vibe_params()); ok = l.some((p) => p.group) || l.length > 7; } catch (e) { ok = false; }
      const o = $('#tb-view option[value=editor]'); o.disabled = !ok; o.textContent = ok ? 'Editor' : 'Editor (native firmwares)';
      if (!ok && editor.open) { setEd(false); setBox(true); }
      return ok;
    };
    let e0 = null; try { e0 = localStorage.getItem(ED_KEY); } catch (e) { /* storage off */ }
    checkEditor().then((ok) => { if (ok && e0 === '1') setEd(true); });
  }
  {                                                   // (the whole window takes the drop – a file let go anywhere would else open in the view)
    let n = 0; const off = () => { n = 0; document.body.classList.remove('dropping'); };
    window.addEventListener('dragenter', (e) => { e.preventDefault(); n++; document.body.classList.add('dropping'); });
    window.addEventListener('dragover', (e) => { e.preventDefault(); if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy'; });
    window.addEventListener('dragleave', () => { if (--n <= 0) off(); });
    window.addEventListener('drop', (e) => { e.preventDefault(); off(); if (e.dataTransfer && e.dataTransfer.files.length) pluginInstall(e.dataTransfer.files); });
  }
  if (window.vibe_midi_ports) {                       // the plugin reads the MIDI controllers itself (CoreMIDI): mapped as here
    pluginMidi(); ctl.plugin = true;                  // (the DAW plays the notes: only the controller mapping is ours)
    ctl.connect().then((ok) => { if (ok) $('#tb-midi').classList.add('on'); });
  }
  if (window.vibe_log) window.vibe_log('plugin mode started');
  if (window.vibe_ready) window.vibe_ready();         // (now the whole picture: one sent before this point is lost)
  await pluginLibrary();
}
// Web MIDI as the plugin delivers it (vibe_midi_ports / vibeMidi): the same MIDIAccess shape controllers.js expects
function pluginMidi() {
  const inputs = new Map(), outputs = new Map(), access = { inputs, outputs, onstatechange: null };
  const LEN = (b) => (b >= 0xF8 || b === 0xF6 || b === 0xF4 || b === 0xF5) ? 1 : (b & 0xF0) === 0xC0 || (b & 0xF0) === 0xD0 || b === 0xF1 || b === 0xF3 ? 2 : 3;
  const sync = async () => {
    let l = { in: [], out: [] }; try { l = JSON.parse(await window.vibe_midi_ports()); } catch (e) { /* none */ }
    const keep = new Map(); inputs.forEach((p) => keep.set(p.id, p.onmidimessage));
    inputs.clear(); outputs.clear();
    l.in.forEach((name, i) => inputs.set('in' + i, { id: 'in' + i, name, onmidimessage: keep.get('in' + i) || null }));
    l.out.forEach((name, i) => outputs.set('out' + i, { id: 'out' + i, name, send: (d) => window.vibe_midi_send(i, Array.from(d)) }));
  };
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
  Object.defineProperty(navigator, 'requestMIDIAccess', { configurable: true, value: async () => { await sync(); return access; } });
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
// 0.2.0: firmware into the plugin – the hub's .zip (or an .wasm / .json) dropped onto the window or chosen; the plugin
// writes <id>.wasm + <id>.json into its firmware folder, the list is read again and the (first) new one starts
async function pluginInstall(files) {
  files = [...files].filter((f) => /\.(zip|wasm|json)$/i.test(f.name));
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
  await pluginLibrary();
  if (ids.length) { const r = await window.vibe_load(ids[0]); if (r !== 'ok') { status(r, true); return; } await pluginLibrary(); status('Installed ' + ids.map((id) => (library.find((f) => f.id === id) || { name: id }).name).join(', ') + ' – it is in the list now' + (errors.length ? ' · ' + errors.join(' · ') : ''), !!errors.length); }
  else status(errors.join(' · ') || 'Nothing installed', true);
}
function pluginPick() {
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

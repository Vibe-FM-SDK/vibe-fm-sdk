// controllers.js – external USB-MIDI controllers for every firmware. The runtime translates them into VIBE events,
// so a firmware never needs to know which controller is plugged in.
//
//   Novation Launchpad Mini MK3 (programmer mode, lights mirrored from VIBE):
//     upper left 4x4  = the 16 pads            upper right 4x4 = PLAY STOP REC HOME / K1-K4 / K5 K6 F1 F2 / OCT- OCT+ ◀ ▶
//     lower 4 rows    = two octaves of keys (white row, black row above it)
//     top buttons     = F1-F6, MENU, RETURN    right column    = SHIFT, ENTER, DATA push, DATA -, DATA +
//   Akai MPK mini IV (read from the unit's presets, 17.10.2026: every preset has the knobs on CC 24-31 – relative in
//     DAW, absolute in PLUGIN and USER – pads on channel 10, notes 36-51; MK3: CC 70-77): K1-K4 = encoders 1-4,
//     K5 = DATA, K6 = VOLUME, K7 = A, K8 = B; pads = the 16 pads (bank A 36-43 = 1-8, bank B 44-51 = 9-16), keys, wheels
//     and the sustain pedal go to the firmware as USB-MIDI. In the DAW preset the buttons (ARP, LATCH, NOTE REPEAT, TAP,
//     BANK A/B, Undo, Loop, Stop/Play, Record, Overdub, SHIFT, the push encoder) send CCs on the "DAW Port": read for CCs
//     only (its notes are the pads again). Read from the unit 17.10.2026 (Sven pressing, aseqdump listening):
//       − / + (BANK < >) CC 80 / 81 = through the six keys (SOUND ENV LFO ARP FX SEQ; OberVave 2.0: FX ENV LFO ARP EDIT SEQ), F1 MIXER, F5 SAVE, F6 GLOBAL, MENU
//       Stop/Play CC 76 = PLAY · Loop/Global CC 74 = STOP · Record CC 77 = REC · Undo CC 73 = HOME · Overdub CC 78 = ENTER
//       TAP TEMPO CC 82 = the chosen key once more (OberVave: the area's next page) · SHIFT CC 17 = SHIFT (held) · push encoder CC 14 = DATA · BANK A/B CC 12 = (pad bank, nothing)
//       ARP, LATCH, NOTE REPEAT: the unit's own, they send nothing. Transport buttons send no release (value 0):
//       a button that never released once is treated as a tap from then on.
//   Any other controller (e.g. Arturia BeatStep Pro):
//     notes on channel 10, 36-51 = the 16 pads; all other notes and messages go to the firmware as USB-MIDI;
//     knobs and buttons: MIDI-Learn (touch a VIBE control, move the controller's) – kept in the browser.
import { EV, BTN, ENC } from './panel.js';

const LP_SYSEX = [0xF0, 0x00, 0x20, 0x29, 0x02, 0x0D];
const isLaunchpad = (name) => /LPMiniMK3|Launchpad Mini/i.test(name || '');
const LP_BUTTONS = [[BTN.PLAY, BTN.STOP, BTN.REC, BTN.HOME], [BTN.K1, BTN.K2, BTN.K3, BTN.K4], [BTN.K5, BTN.K6, BTN.F1, BTN.F2], [BTN.OCT_DOWN, BTN.OCT_UP, BTN.LEFT, BTN.RIGHT]];
const LP_TOP = [BTN.F1, BTN.F2, BTN.F3, BTN.F4, BTN.F5, BTN.F6, BTN.MENU, BTN.RETURN];          // CC 91..98
const LP_RIGHT = { 89: ['b', BTN.SHIFT], 79: ['b', BTN.ENTER], 69: ['b', BTN.DATA_PUSH], 59: ['e', -1], 49: ['e', 1] };
const WHITE = [0, 2, 4, 5, 7, 9, 11, 12], BLACK = [1, 3, null, 6, 8, 10, null, null];
// what − / + step through (MPK mini and every controller with a learned 'Bereich' button)
const CYCLE = [[BTN.K1, 'colour key 1 (SOUND)'], [BTN.K2, 'colour key 2 (ENV)'], [BTN.K3, 'colour key 3 (LFO)'], [BTN.K4, 'colour key 4 (ARP)'],
  [BTN.K5, 'colour key 5 (FX)'], [BTN.K6, 'colour key 6 (SEQ)'], [BTN.F1, 'F1 MIXER'], [BTN.F5, 'F5 SAVE (DATA: slot, TAP: save)'], [BTN.F6, 'F6 GLOBAL'], [BTN.MENU, 'MENU – overview']];
const isMpk = (name) => /MPK ?mini/i.test(name || '');
// the MPK mini's extra USB-MIDI ports (DAW, Plugin, Software Control, DIN; on Linux "MIDI 2" .. "MIDI 5") carry screen
// and transport data for Akai's software – nothing a firmware should hear
const isSidePort = (name) => isMpk(name) && /Plugin|Software|Control|DIN|MIDI [3-9]\b/i.test(name || '');
const isDawPort = (name) => isMpk(name) && /DAW|MIDI 2\b/i.test(name || '');
// controller profiles (VIBE FM Protokoll 15a, 28.10.2026): controllers.json = the SDK's controllers/*.json. A profile maps a
// controller's CCs / notes to VIBE controls; the same file works on the VIBE, in the plugin and here. What is learned here
// (Setup, Learn) is exported in the same format ("Profile ↓").
const ENC_ID = { 1: ENC.E1, 2: ENC.E2, 3: ENC.E3, 4: ENC.E4, A: ENC.A, B: ENC.B, DATA: ENC.DATA };
const ENC_NAME = ['1', '2', '3', '4', 'A', 'B', 'DATA'];
const BTN_NAME = Object.keys(BTN).filter((k) => k !== 'COUNT');
function binding(e) {   // a profile's entry -> what generic() sends
  const label = e.label || (e.to + (e.id !== undefined ? ' ' + e.id : ''));
  if (e.to === 'encoder' && ENC_ID[e.id] !== undefined) return { type: EV.ENCODER, id: ENC_ID[e.id], label, preset: true };
  if (e.to === 'button' && BTN[e.id] !== undefined) return { type: EV.BUTTON, id: BTN[e.id], label, hold: e.id === 'SHIFT', preset: true };
  if (e.to === 'pad' && e.id >= 1 && e.id <= 16) return { type: EV.PAD, id: e.id - 1, label, preset: true };
  if (e.to === 'volume') return { type: 'volume', label: 'VOLUME', preset: true };
  if (e.to === 'area') return { type: 'cycle', dir: e.dir || 0, label, preset: true };
  if (e.to === 'library') return { type: 'library', label: 'firmware library', preset: true };
  if (e.to === 'mix') return { type: 'mix', what: e.what, track: e.track, label: 'mix ' + (e.track ? e.track + ' ' : '') + e.what, preset: true };
  return null;
}
const REL2C = (v) => (v >= 1 && v <= 15) || (v >= 113 && v <= 127);   // relative, two's complement: 1 = +1, 127 = -1
const REL64 = (v) => v >= 57 && v <= 71 && v !== 64;                   // relative around 64: 63 = -1, 65 = +1

export class Controllers {
  constructor(send, status, activity) {
    this.send = send; this.status = status; this.activity = activity || (() => {});
    this.access = null; this.lpOut = null; this.lpState = new Map(); this.lpHeld = new Set();
    this.learning = false; this.target = null; this.last = new Map(); this.mode = new Map(); this.wiz = null;
    try { this.bind = JSON.parse(localStorage.getItem('vibe.midimap') || '{}'); } catch (e) { this.bind = {}; }
    // the global MIDI channel: 0 = all; else only channel messages on this channel reach the firmware (pads on
    // channel 10, the controller profile and MIDI-Learn are the runtime's and pass regardless) – kept in the browser
    this.channel = 0; try { this.channel = +(localStorage.getItem('vibe.midich') || 0) || 0; } catch (e) { /* private mode */ }
    this.leds = null; this.lastLed = 0; this.onRaw = null;   // onRaw(data) → true = taken by the rear panel (pedal, clock)
    this.onVolume = null; this.getVolume = null; this.hist = new Map();   // VOLUME via a controller knob (0..1); knob value history
    this.releases = new Set(['MPK:17']); this.tapT = new Map(); this.autoHeld = new Set(); this.mirror = new Map();   // CC buttons seen releasing; self-releases; port mirroring
    window.addEventListener('beforeunload', () => this.lpMode(0));
    this.profiles = []; this.onLibrary = null; this.onParam = null;
    this.mix = new Map(); this.mixv = []; this.pick = new Map(); this.ledSent = new Map(); this.faderT = new Map();
    fetch('controllers.json', { cache: 'no-cache' }).then((r) => (r.ok ? r.json() : [])).then((p) => { this.profiles = Array.isArray(p) ? p : []; if (this.access) this.wire(); }).catch(() => {});
  }
  profileOf(dev) {   // the first profile whose "match" is part of the port's name
    const d = (dev || '').toLowerCase();
    return this.profiles.find((p) => (p.match || []).some((m) => d.includes(String(m).toLowerCase()))) || null;
  }

  async connect() {
    if (!navigator.requestMIDIAccess) { this.status('Web MIDI does not work in this browser (use Chrome or Edge)', true); return false; }
    try { this.access = await navigator.requestMIDIAccess({ sysex: true }); }
    catch (e) { try { this.access = await navigator.requestMIDIAccess(); } catch (e2) { this.status('MIDI not allowed', true); return false; } }
    this.access.onstatechange = () => this.wire();
    this.wire();
    return true;
  }
  // (SDK 9) the firmware's MIDI out: the port chosen in #midiout (by name, remembered), or none
  setOut(name) { this.outName = name || ''; try { localStorage.setItem('vibe.midiout', this.outName); } catch (e) { /* private mode */ } this.pickOut(); }
  pickOut() { this.outPort = null; if (this.access && this.outName) this.access.outputs.forEach((o) => { if (o.name === this.outName) this.outPort = o; }); }
  outPorts() { const l = []; if (this.access) this.access.outputs.forEach((o) => { if (!isLaunchpad(o.name)) l.push(o.name); }); return l; }
  midiOut(msgs, ctx) {                              // msgs: [[contextSeconds, b0, b1, b2?], …] from the worklet, after its block
    if (!this.outPort) return;
    const now = performance.now(), t0 = ctx ? ctx.currentTime : 0, lat = ctx && ctx.outputLatency ? ctx.outputLatency : 0;
    for (const m of msgs) {
      const at = ctx ? now + Math.max(0, (m[0] - t0 + lat) * 1000) : 0;
      try { this.outPort.send(m.slice(1), at || undefined); } catch (e) { /* a bad message or the port gone */ }
    }
  }
  wire() {
    const names = [];
    this.lpOut = null;
    if (this.outName === undefined) { try { this.outName = localStorage.getItem('vibe.midiout') || ''; } catch (e) { this.outName = ''; } }
    this.pickOut();
    if (this.onOutPorts) this.onOutPorts(this.outPorts(), this.outName);
    this.access.outputs.forEach((o) => { if (isLaunchpad(o.name) && !/DAW/i.test(o.name)) this.lpOut = o; });
    let mpk = false; const all = [];
    const debug = /[?&]debug/.test(location.search);   // ?debug: every port and every message in the status line
    this.access.inputs.forEach((i) => {
      all.push(i.name + (isSidePort(i.name) ? ' [off]' : isDawPort(i.name) ? ' [DAW: buttons]' : ''));
      if (debug) { const h = i.onmidimessage; i.onmidimessage = (m) => { this.status('RAW ' + i.name + ': ' + Array.from(m.data).map((b) => b.toString(16).padStart(2, '0')).join(' ')); }; }
      if (isSidePort(i.name)) { if (!debug) i.onmidimessage = null; return; }
      names.push(i.name);
      if (isLaunchpad(i.name)) i.onmidimessage = /DAW/i.test(i.name) ? null : (m) => this.launchpad(m.data);
      else if (isDawPort(i.name)) i.onmidimessage = (m) => { if (debug) this.status('RAW ' + i.name + ': ' + Array.from(m.data).join(' ')); if ((m.data[0] & 0xF0) === 0xB0) this.generic(i.name, m.data); };
      else i.onmidimessage = (m) => { if (debug) this.status('RAW ' + i.name + ': ' + Array.from(m.data).join(' ')); this.generic(i.name, m.data); };
    if (debug) console.log('MIDI inputs:', all);
      if (isMpk(i.name)) mpk = true;
    });
    this.hasMpk = mpk; this.claim();
    const unknown = names.filter((n) => !isLaunchpad(n) && !this.profileOf(n) && !Object.keys(this.bind).some((k) => k.startsWith(n + '|')));
    if (this.lpOut) { this.lpMode(1); this.lpState.clear(); }
    this.status(debug ? 'MIDI-Ports: ' + all.join(' · ') : names.length ? 'MIDI: ' + names.filter((n) => !/DAW/i.test(n)).join(', ') + (mpk ? ' · MPK mini: K1–K4 = encoders 1–4, K5 = DATA, K6 = volume, K7 = A, K8 = B, pads (channel 10) = pads 1–16, buttons: "Setup" (DAW preset)' : '')
      : 'No MIDI device found – plug one in and press MIDI again');
    if (!debug && unknown.length) this.status('New controller: ' + unknown.join(', ') + ' – its keys and drum pads play; for its knobs and buttons press "Setup"');
    return names.length;
  }

  // ---------------------------------------------------------------- Launchpad Mini MK3
  lpMode(programmer) { try { if (this.lpOut) this.lpOut.send([...LP_SYSEX, 0x0E, programmer, 0xF7]); } catch (e) { /* port gone */ } }
  launchpad(d) {
    this.report('Launchpad', d);
    const st = d[0] & 0xF0, n = d[1], down = (st === 0x90 && d[2] > 0) || (st === 0xB0 && d[2] > 0) ? 1 : 0;
    if (st === 0x90 || st === 0x80) {
      const row = Math.floor(n / 10), col = n % 10;
      if (row < 1 || row > 8 || col < 1 || col > 8) return;
      if (down) this.lpHeld.add(n); else this.lpHeld.delete(n);
      if (row >= 5) {
        const r = 8 - row;
        if (col <= 4) this.send(EV.PAD, r * 4 + col - 1, down, down ? 0.9 : 0);
        else this.send(EV.BUTTON, LP_BUTTONS[r][col - 5], down, 0);
      } else {
        const base = row >= 3 ? 12 : 0, black = row === 2 || row === 4, semi = (black ? BLACK : WHITE)[col - 1];
        if (semi !== null && semi !== undefined) this.send(EV.KEY, base + semi, down, down ? 0.85 : 0);
      }
    } else if (st === 0xB0) {
      if (n >= 91 && n <= 98) this.send(EV.BUTTON, LP_TOP[n - 91], down, 0);
      const r = LP_RIGHT[n];
      if (r && r[0] === 'b') this.send(EV.BUTTON, r[1], down, 0);
      if (r && r[0] === 'e' && down) this.send(EV.ENCODER, ENC.DATA, 0, r[1]);
    }
  }
  // the firmware's lights -> the Launchpad (only what changed, ~20 times a second)
  lights(buf) {
    {   // where the firmware is: the one colour key it lights (OberVave: SEQ in the sequencer, however it was entered) –
        // TAP TEMPO repeats it, − / + go on from it; none lit (MIXER, GLOBAL): the area key pressed last counts
      const f = new Float32Array(buf), lit = [0, 1, 2, 3, 4, 5].filter((i) => f[32 + BTN.COUNT + BTN.K1 + i] >= 0.9);
      if (lit.length === 1 && !(this.cycleAt >= 6)) this.area(BTN.K1 + lit[0]);   // (on MIXER … MENU a colour key may stay lit: keep ours)
    }
    if (!this.lpOut) return;
    const now = performance.now(); if (now - this.lastLed < 50) return; this.lastLed = now;
    const u = new Uint32Array(buf), f = new Float32Array(buf), P = 16, B = BTN.COUNT;
    const want = new Map();
    const rgb = (c, lvl, min = 0) => { const l = Math.max(min, Math.min(1, lvl)); return [((c >> 16) & 255) * l / 2 | 0, ((c >> 8) & 255) * l / 2 | 0, (c & 255) * l / 2 | 0]; };
    const btn = (id) => rgb(u[2 * P + id], f[2 * P + B + id], 0) .map((v, i) => Math.max(v, 6 + (i === 2 ? 2 : 0)));   // unlit = faint, so you see where the buttons are
    for (let r = 0; r < 4; r++) for (let c = 0; c < 4; c++) {
      const p = r * 4 + c; want.set((8 - r) * 10 + c + 1, rgb(u[p], f[P + p]));
      want.set((8 - r) * 10 + c + 5, btn(LP_BUTTONS[r][c]));
    }
    for (let row = 1; row <= 4; row++) for (let col = 1; col <= 8; col++) {
      const n = row * 10 + col, black = row === 2 || row === 4, semi = (black ? BLACK : WHITE)[col - 1];
      if (semi === null || semi === undefined) { want.set(n, [0, 0, 0]); continue; }
      want.set(n, this.lpHeld.has(n) ? [127, 52, 12] : black ? [6, 10, 24] : [22, 22, 22]);
    }
    LP_TOP.forEach((b, i) => want.set(91 + i, btn(b)));
    Object.keys(LP_RIGHT).forEach((k) => want.set(+k, LP_RIGHT[k][0] === 'b' ? btn(LP_RIGHT[k][1]) : [10, 10, 14]));
    const msg = [...LP_SYSEX, 0x03];
    want.forEach((v, led) => { const key = v.join(','); if (this.lpState.get(led) === key) return; this.lpState.set(led, key); msg.push(0x03, led, v[0] & 127, v[1] & 127, v[2] & 127); });
    if (msg.length > 7) { msg.push(0xF7); try { this.lpOut.send(msg); } catch (e) { this.lpOut = null; } }
  }

  // ---------------------------------------------------------------- everything else: drum pads, MIDI-Learn
  generic(dev, d) {
    const st = d[0] & 0xF0, ch = d[0] & 15, n = d[1], v = d[2] ?? 0;
    if (!this.learning && !this.wiz && this.onRaw && this.onRaw(d)) return;
    // transport of a hardware sequencer (BeatStep Pro, drum machines, DAWs): MIDI clock, Start/Continue, Stop – to the
    // firmware as TRANSPORT (SDK 7, as the VIBE FM Plugin and the device do); older firmwares: PLAY / STOP
    if (d[0] === 0xF8) { if (!this.plugin) this.clockTick(); return; }
    if (d[0] === 0xFA || d[0] === 0xFB || d[0] === 0xFC) {
      if (this.plugin) return;                         // (in the plugin the DAW's own transport leads)
      const stop = d[0] === 0xFC;
      this.activity('MIDI ← ' + dev.replace(/Arturia /g, '') + ': ' + (stop ? 'Stop' : 'Start'));
      this.transport(0, stop ? 0 : 1, 0); return;
    }
    if (st === 0xF0 || d[0] >= 0xF8) return;
    if (st === 0xE0 && !this.learning && !this.wiz) {   // (SDK 11) a mixer's fader: pitch bend on the strip's channel
      const p = this.profileOf(dev), e = p && (p.map || []).find((x) => x.pitchbend === ch + 1 && x.to === 'mix');
      if (e) { this.mixFader(dev, e.track, ((v << 7) | n) / 16383); return; }
    }
    if (isMpk(dev) && st === 0xB0) {   // the MPK mirrors a knob or button onto two ports: the same CC twice within 20 ms counts once
      const now = performance.now(), dk = ch + ':' + n + ':' + v, prev = this.mirror.get(dk);
      this.mirror.set(dk, { t: now, dev });
      if (prev && prev.dev !== dev && now - prev.t < 20) return;
    }
    this.report(dev, d);
    const isNote = st === 0x90 || st === 0x80, down = st === 0x90 && v > 0, isPC = st === 0xC0;   // PC: e.g. BeatStep Pro step buttons in CNTRL mode
    const key = dev + '|' + (isNote ? 'n' : st === 0xB0 ? 'c' : isPC ? 'p' : 'x') + '|' + ch + '|' + n;
    if (this.learning && this.target && (isNote || st === 0xB0 || isPC) && (down || st === 0xB0 || isPC)) {
      this.bind[key] = this.target; this.save();
      this.status('Learned: ' + (isNote ? 'Note ' : isPC ? 'Program Change ' : 'CC ') + n + ' (channel ' + (ch + 1) + ') → ' + this.target.label);
      this.target = null; return;
    }
    if (this.wiz && (isNote ? down : st === 0xB0)) { this.wizStep(key, isNote, n, ch); return; }
    const b = this.bind[key] || this.preset(dev, st, n, ch);
    if (b && b.type === 'cycle') {   // − / +: through the six keys, then F1 MIXER, F5 SAVE, F6 GLOBAL and MENU (the overview) –
                                     // a controller without F keys reaches every area; 'again' = the same key once more
      if (!(isNote ? down : v > 0)) return;
      if (b.dir === 0 && this.cycleAt === undefined) return;   // 'again' before any area was picked: nothing to repeat
      this.cycleAt = ((this.cycleAt ?? -1) + b.dir + CYCLE.length) % CYCLE.length;
      const [id, name] = CYCLE[this.cycleAt];
      this.send(EV.BUTTON, id, 1, 0.9); setTimeout(() => this.send(EV.BUTTON, id, 0, 0), 60);
      this.status('Area: ' + name);
      return;
    }
    if (b && b.type === 'mix') { if ((isNote ? down : v > 0)) this.mixButton(b.what, b.track); return; }
    if (b && b.type === 'library') { if ((isNote ? down : v > 0) && this.onLibrary) this.onLibrary(); return; }
    if (b && isPC) {   // a program change is a single "click": press and release
      if (b.type === EV.ENCODER) this.send(EV.ENCODER, b.id, 0, 1);
      else { this.send(b.type, b.id, 1, 0.9); setTimeout(() => this.send(b.type, b.id, 0, 0), 60); }
      return;
    }
    if (b) {
      if (b.type === EV.ENCODER || b.type === 'volume') {
        if (isNote) { if (down) this.send(EV.ENCODER, b.id, 0, 1); return; }
        const det = (this.profileOf(dev) || {}).knobs === 'relsm' ? (v < 64 ? v : 64 - v) : this.knobDelta(key, v);   // (relsm: Mackie V-pots, sign and size)
        if (b.type === 'volume') {   // VOLUME is the runtime's (a real pot on the device): absolute knob = the position, relative = nudges
          const cur = this.getVolume ? this.getVolume() : 0.8;
          if (this.onVolume) this.onVolume(this.mode.get(key) === 'abs' ? v / 127 : Math.min(1, Math.max(0, cur + det / 48)));
          return;
        }
        if (det) this.send(EV.ENCODER, b.id, 0, det);
        return;
      }
      const isDown = isNote ? down : v > 0;
      if (!isNote && !isDown) { this.releases.add(key); clearTimeout(this.tapT.get(key)); }
      // a CC button that repeats while held (no release of its own): each repeat keeps it held a little longer
      if (!isNote && isDown && !b.hold && !this.releases.has(key) && this.autoHeld.has(key)) {
        clearTimeout(this.tapT.get(key));
        this.tapT.set(key, setTimeout(() => { this.autoHeld.delete(key); this.send(b.type, b.id, 0, 0); }, 250));
        return;
      }
      this.send(b.type, b.id, isDown ? 1 : 0, isDown ? (isNote ? Math.max(0.2, v / 127) : 0.9) : 0);
      // a CC button that never sent a release (MPK mini transport: Note-On-only style): let go by ourselves
      if (!isNote && isDown && !b.hold && !this.releases.has(key)) { clearTimeout(this.tapT.get(key)); this.autoHeld.add(key); this.tapT.set(key, setTimeout(() => { this.autoHeld.delete(key); if (!this.releases.has(key)) this.send(b.type, b.id, 0, 0); }, 150)); }
      return;
    }
    const pads = (this.profileOf(dev) || {}).pads || { channel: 10, note: 36, rows: 'top-down' };   // (no profile: channel 10, 36-51)
    if (isNote && ch === pads.channel - 1 && n >= pads.note && n < pads.note + 16) {   // ("bottom-up": in each row pair the lower row comes first – the MPK mini; VIBE counts from the top)
      const p = n - pads.note, id = pads.rows === 'bottom-up' ? (((p >> 2) ^ 1) << 2) + (p & 3) : p;
      this.send(EV.PAD, id, down ? 1 : 0, down ? v / 127 : 0); return;
    }
    if (this.plugin) return;                           // (in the VIBE FM Plugin the DAW hands the notes to the firmware)
    if (this.channel && st < 0xF0 && ch !== this.channel - 1) return;   // the global channel filter
    if (d.length >= 2) this.send(EV.MIDI, 0, 0, 0, d[0] | (d[1] << 8) | ((d[2] || 0) << 16));
  }
  setChannel(n) {
    this.channel = Math.max(0, Math.min(16, +n || 0));
    try { localStorage.setItem('vibe.midich', String(this.channel)); } catch (e) { /* private mode */ }
    this.status(this.channel ? 'MIDI channel ' + this.channel + ': only this channel reaches the firmware (pads on channel 10 and the controller mapping always do)' : 'MIDI channel: all (omni)');
  }
  // ---------------------------------------------------------------- SDK 11: a mixer controller (Protokoll 7.2a)
  // the firmware's mix_* parameters (worklet) and their values (with every frame)
  setMixer(params) { this.mix = new Map((params || []).map((p, k) => [p.id, { ...p, k }])); this.mixv = []; this.pick.clear(); this.ledSent.clear(); }
  mixGet(id) { const m = this.mix.get(id); return m ? this.mixv[m.k] : undefined; }
  mixSet(id, v) { const m = this.mix.get(id); if (!m || !this.onParam) return false; v = Math.min(m.max, Math.max(m.min, v)); this.mixv[m.k] = v; this.onParam(m.i, v); return true; }
  mixButton(what, t) {
    if (!this.mix.size) { this.status('This firmware has no mixer parameters (mix_*) for the controller'); return; }
    if (what === 'mute' || what === 'solo' || what === 'arm') { const id = 'mix_' + t + '_' + what; this.mixSet(id, (this.mixGet(id) || 0) >= 0.5 ? 0 : 1); return; }
    if (what === 'select') { this.mixSet('mix_select', t); return; }
    if (what === 'prev' || what === 'next') this.mixSet('mix_select', Math.round(this.mixGet('mix_select') || 1) + (what === 'next' ? 1 : -1));
  }
  // a fader picks the level up first: it acts once it comes within 2 % of it, or crosses it (no jump)
  mixFader(dev, t, v) {
    const id = 'mix_' + t + '_level', cur = this.mixGet(id); if (cur === undefined) return;
    const k = dev + '|' + t, p = this.pick.get(k) || {};
    if (!p.on && (Math.abs(v - cur) <= 0.02 || (p.last !== undefined && (p.last < cur) !== (v < cur)))) p.on = true;
    p.last = v; this.pick.set(k, p); this.faderT.set(t, performance.now());
    if (p.on) { this.mixSet(id, v); p.set = v; }
    else this.status('Fader ' + t + ': pick up the level at ' + Math.round(cur * 100) + ' %');
  }
  // the controller's LEDs and fader positions follow the firmware ("led": true in its profile): only what changed
  mixValues(vals, leds) {
    this.mixv = vals; if (!this.access || this.plugin) return;
    for (const [k, p] of this.pick) { const t = +k.split('|').pop(), cur = this.mixGet('mix_' + t + '_level'); if (p.on && p.set !== undefined && Math.abs(cur - p.set) > 0.01) p.on = false; }   // (moved elsewhere: pick up again)
    const f = leds ? new Float32Array(leds) : null;
    this.access.outputs.forEach((o) => {
      const prof = this.profileOf(o.name); if (!prof || isLaunchpad(o.name)) return;
      const msgs = [];
      for (const e of prof.map || []) {
        if (!e.led) continue;
        if (e.to === 'mix' && e.what === 'level' && e.pitchbend) {
          const v = this.mixGet('mix_' + e.track + '_level'); if (v === undefined) continue;
          if (performance.now() - (this.faderT.get(e.track) ?? -1e9) < 300) continue;   // (being moved: no echo)
          const x = Math.round(v * 16383), key = o.name + '|pb|' + e.pitchbend;
          if (this.ledSent.get(key) !== x) { this.ledSent.set(key, x); msgs.push([0xE0 | (e.pitchbend - 1), x & 127, x >> 7]); }
          continue;
        }
        if (e.note === undefined) continue;
        let on;
        if (e.to === 'mix') on = e.what === 'select' ? Math.round(this.mixGet('mix_select') || 0) === e.track : (this.mixGet('mix_' + e.track + '_' + e.what) || 0) >= 0.5;
        else if (e.to === 'button' && f && BTN[e.id] !== undefined) on = f[32 + BTN.COUNT + BTN[e.id]] >= 0.5;
        else continue;
        const key = o.name + '|n|' + (e.channel || 1) + '|' + e.note, vel = on ? 127 : 0;
        if (this.ledSent.get(key) !== vel) { this.ledSent.set(key, vel); msgs.push([0x90 | ((e.channel || 1) - 1), e.note, vel]); }
      }
      for (const m of msgs) { try { o.send(m); } catch (err) { /* port gone */ } }
    });
  }
  // the controller's profile (Protokoll 15a), for a message no learned binding claims: an entry for this port first
  // ("port": part of its name; "DAW" also the MPK's second port on Linux), then one for any port
  preset(dev, st, n, ch) {
    const p = this.profileOf(dev); if (!p || !Array.isArray(p.map)) return null;
    const isNote = st === 0x90 || st === 0x80; if (!isNote && st !== 0xB0) return null;
    const hit = (e) => (isNote ? e.note === n : e.cc === n) && (!e.channel || ch === undefined || e.channel - 1 === ch);
    const onPort = (e) => (dev || '').toLowerCase().includes(String(e.port).toLowerCase()) || (e.port === 'DAW' && isDawPort(dev));
    const e = p.map.find((x) => x.port && onPort(x) && hit(x)) || p.map.find((x) => !x.port && hit(x));
    return e ? binding(e) : null;
  }
  // what was learned here (Setup, Learn), as a profile file per controller – to keep, share or put into the SDK's controllers/
  exportProfiles() {
    const by = new Map();
    for (const [k, b] of Object.entries(this.bind)) {
      const [dev, kind, ch, n] = k.split('|'); if (kind !== 'c' && kind !== 'n') continue;
      const e = kind === 'c' ? { cc: +n } : { note: +n }; e.channel = +ch + 1;
      if (b.type === EV.ENCODER) Object.assign(e, { to: 'encoder', id: ENC_NAME[b.id] });
      else if (b.type === EV.BUTTON) Object.assign(e, { to: 'button', id: BTN_NAME.find((x) => BTN[x] === b.id) });
      else if (b.type === EV.PAD) Object.assign(e, { to: 'pad', id: b.id + 1 });
      else if (b.type === 'volume') e.to = 'volume';
      else if (b.type === 'cycle') Object.assign(e, { to: 'area', dir: b.dir || 0 });
      else if (b.type === 'library') e.to = 'library';
      else continue;
      const base = dev.replace(/\s+(MIDI|DAW|Port)\b.*$/i, '').trim() || dev;
      if (!by.has(base)) by.set(base, { format: 1, name: base, match: [base], knobs: 'auto', map: [] });
      by.get(base).map.push(e);
    }
    return [...by.values()];
  }
  // a knob's turn in detents. Knobs send either absolute values (0..127) or relative steps: around 64 (63 = -1,
  // 65 = +1) or two's complement (1 = +1, 127 = -1). The mode is decided from the first values and then remembered:
  // relative when the values only ever sit in a relative range (two in a row), absolute otherwise. Until then small
  // differences are passed on, so the knob never feels dead and an absolute knob that starts near 64 is not mistaken.
  knobDelta(key, v) {
    let mode = this.mode.get(key);
    const last = this.last.get(key); this.last.set(key, v);
    const h = this.hist.get(key) || []; h.push(v); if (h.length > 4) h.shift(); this.hist.set(key, h);
    // relative knobs repeat a value (65, 65, 65 …) or sit on both sides of their centre (63 / 65, 127 / 1);
    // an absolute knob sends ever new values – it is never the same twice and does not jump across the centre
    const twice = h.length >= 2 && h[h.length - 1] === h[h.length - 2];
    const both = (f, lo) => h.every(f) && h.some((x) => x < lo) && h.some((x) => x > lo);
    if (mode !== 'rel64' && mode !== 'rel2c') {
      if ((twice && REL64(v)) || both(REL64, 64)) mode = 'rel64';
      else if ((twice && REL2C(v)) || both(REL2C, 64)) mode = 'rel2c';
      else if (!mode && h.length >= 3) mode = 'abs';
      if (mode) this.mode.set(key, mode);
      if (!mode) return last === undefined || Math.abs(v - last) > 12 ? 0 : v - last;
    }
    if (mode === 'rel64') return v - 64;
    if (mode === 'rel2c') return v < 64 ? v : v - 128;
    return last === undefined ? 0 : v - last;   // absolute: 1 value step = 1 detent, fine and without stair steps
  }
  // the panel reports every VIBE control the user touches; while learning, the first one becomes the target
  // an area key was pressed (panel, computer keyboard, any controller): the − / + cycle and TAP TEMPO go on from it
  area(id) { const i = CYCLE.findIndex((c) => c[0] === id); if (i >= 0) this.cycleAt = i; }
  touched(type, id) {
    if (!this.learning || this.target) return;
    const names = { [EV.ENCODER]: ['Encoder 1', 'Encoder 2', 'Encoder 3', 'Encoder 4', 'Encoder A', 'Encoder B', 'DATA'], [EV.PAD]: Array.from({ length: 16 }, (_, i) => 'Pad ' + (i + 1)) };
    const bn = Object.keys(BTN).find((k) => BTN[k] === id);
    const label = type === EV.BUTTON ? bn : type === EV.KEY ? 'Key ' + (id + 1) : (names[type] || [])[id];
    if (!label) return;
    this.target = { type, id, label };
    this.status('MIDI learn: now turn or press a control on your controller → ' + label);
  }
  report(dev, d) {
    const st = d[0] & 0xF0, ch = (d[0] & 15) + 1, what = st === 0x90 && d[2] ? 'Note ' + d[1] : st === 0x80 || st === 0x90 ? 'Note off ' + d[1] : st === 0xB0 ? 'CC ' + d[1] + ' = ' + d[2] : st === 0xE0 ? 'Pitchbend' : st === 0xC0 ? 'Program Change ' + d[1] : 'Status ' + d[0].toString(16);
    let to = '';
    if ((st === 0x90 || st === 0x80) && ch === 10 && d[1] >= 36 && d[1] <= 51) { const p = d[1] - 36; to = ' → Pad ' + ((isMpk(dev) ? (((p >> 2) ^ 1) << 2) + (p & 3) : p) + 1); }
    const p = this.bind[dev + '|' + (st === 0xB0 ? 'c' : 'n') + '|' + (ch - 1) + '|' + d[1]] || this.preset(dev, st, d[1], ch - 1);
    if (p && !to) to = ' → ' + p.label;
    this.activity('MIDI ← ' + dev.replace(/Arturia /g, '').replace(/ Arturia Be$| BeatStepPr$/, '') + ': ' + what + ' · channel ' + ch + to);
  }
  // guided setup: walks through the VIBE controls one by one; the next knob turned / button pressed on the
  // controller is bound to it. Click "Einrichten" again to stop early.
  startWizard() {
    const E = (id, label) => ({ type: EV.ENCODER, id, label, knob: true }), K = (id, label) => ({ type: EV.BUTTON, id, label });
    let mpk = false; if (this.access) this.access.inputs.forEach((i) => { if (isMpk(i.name)) mpk = true; });
    if (mpk) {   // the knobs and pads are fixed: only the buttons (DAW preset: ARP, LATCH, NOTE REPEAT, TAP, BANK A/B, Undo, Loop, Stop/Play, Record, Overdub)
      this.wiz = { i: 0, used: new Set(), list: [
        { type: 'cycle', dir: 1, label: 'next area (SOUND → … → SEQ → MIXER → TRACKS → GLOBAL → OVERVIEW), e.g. +' }, { type: 'cycle', dir: -1, label: 'previous area, e.g. −' }, { type: 'cycle', dir: 0, label: 'same area again (the chosen key once more), e.g. TAP TEMPO' },
        K(BTN.K1, 'colour key 1 (SOUND)'), K(BTN.K2, 'colour key 2 (ENV)'), K(BTN.K3, 'colour key 3 (LFO)'), K(BTN.K4, 'colour key 4 (ARP)'), K(BTN.K5, 'colour key 5 (FX)'), K(BTN.K6, 'colour key 6 (SEQ)'),
        K(BTN.PLAY, 'PLAY'), K(BTN.STOP, 'STOP'), K(BTN.REC, 'REC'), K(BTN.HOME, 'HOME'), K(BTN.ENTER, 'ENTER'), K(BTN.RETURN, 'BACK'), K(BTN.F1, 'F1'), K(BTN.F2, 'F2'), K(BTN.F5, 'F5'), K(BTN.F6, 'F6') ] };
      this.wizPrompt(); return;
    }
    this.wiz = { i: 0, used: new Set(), list: [
      E(ENC.E1, 'Encoder 1'), E(ENC.E2, 'Encoder 2'), E(ENC.E3, 'Encoder 3'), E(ENC.E4, 'Encoder 4'),
      E(ENC.A, 'Encoder A'), E(ENC.B, 'Encoder B'), E(ENC.DATA, 'DATA wheel'),
      K(BTN.REC, 'REC'), K(BTN.OCT_DOWN, 'OCT −'), K(BTN.OCT_UP, 'OCT +'), K(BTN.HOME, 'HOME'), K(BTN.SHIFT, 'SHIFT'), K(BTN.ENTER, 'ENTER'),
      K(BTN.K1, 'colour key 1'), K(BTN.K2, 'colour key 2'), K(BTN.K3, 'colour key 3'), K(BTN.K4, 'colour key 4'), K(BTN.K5, 'colour key 5'), K(BTN.K6, 'colour key 6'),
      K(BTN.F1, 'F1'), K(BTN.F2, 'F2'), K(BTN.F3, 'F3'), K(BTN.F4, 'F4'), K(BTN.LEFT, '◀'), K(BTN.RIGHT, '▶'), K(BTN.RETURN, 'BACK') ] };
    this.wizPrompt();
  }
  wizPrompt() {
    const w = this.wiz, t = w.list[w.i];
    if (!t) { this.stopWizard(true); return; }
    this.status('Setup ' + (w.i + 1) + '/' + w.list.length + ': ' + (t.knob ? 'turn a knob for ' : 'press a button for ') + t.label + ' ("Learn" = skip, "Setup" = done)');
  }
  wizStep(key, isNote, n, ch) {
    const w = this.wiz, t = w.list[w.i];
    if (w.used.has(key)) return;                      // the knob just bound is still turning
    if (t.knob && isNote) return;                     // a knob needs a CC
    if (this.preset(key.split('|')[0], isNote ? 0x90 : 0xB0, n, ch)) return;   // (a knob / SHIFT the profile already has)
    w.used.add(key); this.bind[key] = { type: t.type, id: t.id, dir: t.dir, label: t.label }; this.save();
    w.i++; this.wizPrompt();
  }
  wizSkip() { if (!this.wiz) return false; this.wiz.i++; this.wizPrompt(); return true; }
  stopWizard(done) { const n = this.wiz ? this.wiz.i : 0; this.wiz = null; if (this.onWizard) this.onWizard(false); this.status((done ? 'Setup done: ' : 'Setup ended: ') + n + ' controls mapped'); }
  setLearning(on) { this.learning = on; this.target = null; this.status(on ? 'MIDI learn: tap a VIBE knob, button or pad on screen (e.g. REC) …' : 'MIDI learn off · ' + Object.keys(this.bind).length + ' mappings saved'); }
  clear() { this.bind = {}; this.save(); this.status('All MIDI mappings cleared'); }
  transport(id, down, value) {   // id 0 PLAY, 1 TEMPO (BPM)
    if ((this.sdk || 0) >= 7) { this.send(EV.TRANSPORT, id, down, value); return; }
    if (id === 0) { const b = down ? BTN.PLAY : BTN.STOP; this.send(EV.BUTTON, b, 1, 0); this.send(EV.BUTTON, b, 0, 0); }
  }
  // the MIDI clock's tempo, as the device's runtime measures it (runtime/linux/vibe_runtime.c clock_tick): once a quarter
  // over up to two bars, 0.1 BPM, refined while the window grows, then only moves of 0.3 or more; a quarter more than 2 %
  // off starts afresh (the change is taken a quarter later); gone for a second: the firmware's own tempo again
  clockTick() {
    const N = 192, t = performance.now(), c = this.clk || (this.clk = { t: new Float64Array(N), n: 0, from: 0, last: 0, bpm: 0, watch: 0 });
    if (c.n && t - c.last > 500) c.n = c.from = 0;
    c.last = t; c.t[c.n % N] = t; c.n++;
    if (!c.watch) c.watch = setInterval(() => { if (c.bpm > 0 && performance.now() - c.last > 1000) { c.bpm = 0; c.n = c.from = 0; this.transport(1, 0, 0); } }, 250);
    if (c.n < 25 || c.n % 24) return;
    const last = c.n - 1; let n = last - c.from;
    if (n > N - 1) { n = N - 1; c.from = last - n; }
    const q = t - c.t[(last - 24) % N], span = t - c.t[(last - n) % N], avg = span * 24 / n;
    if (n > 24 && Math.abs(q - avg) > avg * 0.02) { c.from = last; return; }
    if (span <= 0) return;
    const bpm = Math.round(60000 * n / 24 / span * 10) / 10;
    if (bpm < 20 || bpm > 400 || Math.abs(bpm - c.bpm) < (n < N - 1 ? 0.05 : 0.3)) return;
    c.bpm = bpm; this.transport(1, 0, bpm); this.activity('MIDI clock: ' + bpm.toFixed(1) + ' BPM');
  }
  save() { try { localStorage.setItem('vibe.midimap', JSON.stringify(this.bind)); } catch (e) { /* private mode */ } this.claim(); }
  // the VIBE FM Plugin: the CCs mapped here (the MPK's knobs and buttons, learned ones) do not reach the firmware from the DAW as well
  claim() {
    if (!this.plugin || !window.vibe_midi_claim) return;
    const cc = new Set();
    if (this.hasMpk) { for (let i = 0; i < 8; i++) cc.add(24 + i); [12, 13, 14, 17, 73, 74, 76, 77, 78, 80, 81, 82].forEach((c) => cc.add(c)); }
    Object.keys(this.bind).forEach((k) => { const f = k.split('|'); if (f[1] === 'c') cc.add(+f[3]); });
    window.vibe_midi_claim([...cc]);
  }
}

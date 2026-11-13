// worklet.js – runs the firmware (.wasm) inside the browser's audio thread. Everything the firmware does happens
// here, one call after the other, exactly like on the device: control events, audio blocks of 128 frames, and a
// display frame when the page asks for one. A firmware crash stops only the firmware, never the page.
// SDK 2 adds the rear panel: IN 1/IN 2 (input channels 0/1), CV/Gate out, CV IN (struct vibe_rear in vibe_sdk.c).
// SDK 3 adds storage: the page hands over the firmware's saved files at start (processorOptions.store), reads are
// answered from that copy, every write goes back to the page, which keeps it in IndexedDB.
// UTF-8 bytes of a string – the AudioWorkletGlobalScope has no TextEncoder (28.10.: PLAYER's file list crashed with files in it)
function utf8(str) { const s = unescape(encodeURIComponent(str)), b = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i); return b; }
// SDK 13 (Protokoll 13a): media – the page holds the folders (picked or dropped), the worklet a cache of what the firmware
// asked for: folder lists, sizes, 64 KB blocks (16 MB, the least used go). A miss asks the page and answers -2 ("not yet")
// – the page's reply comes between two audio blocks; a read asks for the 8 blocks after it too. Writes go to the page in
// pieces, it writes them with createWritable (the file appears complete when it is closed).
const MBLK = 65536, MAHEAD = 8, MCACHE = 16 << 20, MFRESH = 3;
function mPartOk(b) {
  if (b.length < 1 || b.length > 255 || (b[0] === 46 && (b.length === 1 || (b.length === 2 && b[1] === 46)))) return false;
  for (let i = 0; i < b.length;) {
    const c = b[i]; if (c < 32 || c === 127 || c === 92 || c === 47) return false;
    const k = c < 0x80 ? 1 : c >= 0xC2 && c < 0xE0 ? 2 : c >= 0xE0 && c < 0xF0 ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (!k || i + k > b.length) return false;
    for (let j = 1; j < k; j++) if ((b[i + j] & 0xC0) !== 0x80) return false;
    if ((k === 3 && ((c === 0xE0 && b[i + 1] < 0xA0) || (c === 0xED && b[i + 1] >= 0xA0))) || (k === 4 && ((c === 0xF0 && b[i + 1] < 0x90) || (c === 0xF4 && b[i + 1] >= 0x90)))) return false;
    i += k;
  }
  return true;
}
function mPath(b) {   // bytes of "key/a/b" -> the key's length, 0 = refused
  let n = b.length; while (n > 1 && b[n - 1] === 47) n--;
  if (n < 1 || n > 1023) return 0;
  let k = 0; for (; k < n && b[k] !== 47; k++) { const c = b[k]; if (!((c >= 97 && c <= 122) || (c >= 65 && c <= 90) || (c >= 48 && c <= 57) || c === 95 || c === 45)) return 0; }
  if (k < 1 || k > 47) return 0;
  for (let i = k + 1; i < n;) { let j = i; while (j < n && b[j] !== 47) j++; if (!mPartOk(b.subarray(i, j))) return 0; i = j + 1; }
  return k;
}
class Media {
  constructor(send, fw) { this.send = send; this.fw = fw; this.src = []; this.c = new Map(); this.bytes = 0; this.tick = 0; this.asked = false; this.ev = []; this.h = new Map(); this.next = 1; this.recent = []; }
  sources(list) {   // the page's folders changed
    const had = new Set(this.src.map((s) => s.key)), has = new Set(list.map((s) => s.key));
    this.src = list;
    for (const k of had) if (!has.has(k)) { this.drop(k); for (const w of this.h.values()) if (w.key === k) w.failed = true; if (this.asked) this.ev.push([0, 0, list.length]); }
    for (const k of has) if (!had.has(k) && this.asked) this.ev.push([0, 1, list.length]);
  }
  drop(k) { for (const [ck, e] of this.c) if (e.path === k || e.path.startsWith(k + '/')) { this.bytes -= e.n || 0; this.c.delete(ck); } }
  get(kind, path, blk) {   // the entry, asked for when new; lists and sizes older than 3 s asked again (the old one stays)
    const ck = kind + '|' + path + '|' + blk; let e = this.c.get(ck);
    if (!e) { e = { kind, path, blk, st: 0, n: 0 }; this.c.set(ck, e); this.send({ t: 'mreq', kind, path, blk }); }
    else if (e.st !== 0 && !e.again && (kind !== 'b' || e.st < 0) && currentTime - e.t > MFRESH) { e.again = true; this.send({ t: 'mreq', kind, path, blk }); }
    e.used = ++this.tick; return e;
  }
  result(d) {   // the page's answer: d.v (list text / [type, size, mtime] / ArrayBuffer + size), d.miss
    const e = this.c.get(d.kind + '|' + d.path + '|' + d.blk); if (!e) return;
    if (e.waited) { e.waited = false; this.ready = true; }   // (READY: the firmware asks again)
    this.bytes -= e.n; e.st = d.miss ? -1 : 1; e.v = d.v; e.size = d.size; e.t = currentTime; e.again = false;
    e.n = d.v && d.v.byteLength ? d.v.byteLength : typeof d.v === 'string' ? d.v.length : 0; this.bytes += e.n;
    if (this.bytes > MCACHE) for (const x of [...this.c.values()].filter((x) => x.kind === 'b' && x.st === 1).sort((a, b) => a.used - b.used)) { if (this.bytes <= MCACHE) break; this.bytes -= x.n; this.c.delete(x.kind + '|' + x.path + '|' + x.blk); }
  }
  store(op, u8, dv, np, nl, dp, len, text, utf8) {
    if (op === 6) {
      if (!this.asked) this.send({ t: 'masked' });   // (the page: a dropped folder is media now)
      this.asked = true; let w = 0;
      for (const s of this.src) { const b = utf8(`${s.key}\t${s.write ? 'w' : ''}${s.fixed ? 'k' : ''}\t${s.label}\n`); if (w + b.length > len) break; u8.set(b, dp + w); w += b.length; }
      return w;
    }
    if (op >= 7 && op <= 9) {
      const nb = u8.slice(np, np + nl), kl = mPath(nb); if (!kl) return -1;
      let n = nb.length; while (n > kl + 1 && nb[n - 1] === 47) n--; if (n === kl + 1) n = kl;
      const path = text(np, n), key = path.slice(0, kl);
      if (!this.src.some((s) => s.key === key)) return -1;
      if (op === 7) {
        if (len < 4) return -1;
        const e = this.get('l', path, 0); if (e.st === 0) { e.waited = true; return -2; } if (e.st < 0) return -1;
        const skip = dv.getUint32(dp, true), lines = e.v; let at = 0, w = 0;
        for (let i = 0; i < skip && at < lines.length; i++) { const x = lines.indexOf('\n', at); at = x < 0 ? lines.length : x + 1; }
        while (at < lines.length) { const x = lines.indexOf('\n', at); if (x < 0) break; const b = utf8(lines.slice(at, x + 1)); if (w + b.length > len) break; u8.set(b, dp + w); w += b.length; at = x + 1; }
        return w;
      }
      if (op === 8) {
        if (len < 16) return -1;
        if (n === kl) { u8.fill(0, dp, dp + 16); return 2; }
        const e = this.get('s', path, 0); if (e.st === 0) { e.waited = true; return -2; } if (e.st < 0) return -1;
        dv.setBigUint64(dp, BigInt(e.v[1]), true); dv.setBigInt64(dp + 8, BigInt(e.v[2]), true); return e.v[0];
      }
      if (len < 8 || n === kl) return -1;
      const off = Number(dv.getBigUint64(dp, true));
      let b = Math.floor(off / MBLK), w = 0, size = Infinity, gone = false;
      while (w < len) {
        const e = this.get('b', path, b); if (e.st === 0) { if (!w) e.waited = true; break; } if (e.st < 0) { gone = true; break; }
        size = e.size; const at = off + w - b * MBLK, data = e.v;
        if (at >= data.byteLength) break;
        const c = Math.min(data.byteLength - at, len - w); u8.set(new Uint8Array(data, at, c), dp + w); w += c;
        if (data.byteLength < MBLK) break; b++;
      }
      for (let a = b; a < b + MAHEAD && a * MBLK < size; a++) this.get('b', path, a);   // (read ahead)
      return w ? w : gone ? -1 : off >= size ? 0 : -2;
    }
    if (op === 10) {
      const nb = u8.slice(np, np + nl), s = this.src.find((x) => x.write);
      if (!s || this.h.size >= 8 || len < 2 || nl > 200 || nb[0] === 46 || !mPartOk(nb)) return -1;
      const dir = `${s.key}/VIBE FM/${this.fw}`, e = this.get('l', dir, 0);
      if (e.st === 0) { e.waited = true; return -2; }
      const name = text(np, nl), dot = name.lastIndexOf('.'), base = dot > 0 ? name.slice(0, dot) : name, ext = dot > 0 ? name.slice(dot) : '';
      const have = new Set([...(e.st > 0 ? e.v.split('\n').map((l) => l.split('\t')[2]).filter(Boolean) : []), ...this.recent, ...[...this.h.values()].map((x) => x.name)].map((x) => x.toLowerCase()));
      let cand = name; for (let k = 2; have.has(cand.toLowerCase()); k++) cand = `${base} (${k})${ext}`;
      const out = utf8(`${dir}/${cand}\0`); if (out.length > len) return -1;
      u8.set(out, dp);
      const id = this.next++; this.h.set(id, { key: s.key, name: cand, queued: 0, failed: false });
      this.recent.push(cand);
      this.send({ t: 'mopen', h: id, key: s.key, dir: ['VIBE FM', this.fw], name: cand });
      return id;
    }
    if (op === 11 || op === 12) {
      const w = this.h.get(nl); if (!w) return -1;
      if (op === 12) { this.h.delete(nl); this.send({ t: 'mclose', h: nl, keep: len !== 0 }); return 0; }
      if (w.failed) return -1;
      if (w.queued + len > 8 << 20) { w.waited = true; return -2; }
      if (len) { const c = u8.slice(dp, dp + len).buffer; w.queued += len; this.send({ t: 'mwrite', h: nl, data: c }, [c]); }
      return 0;
    }
    return -1;
  }
  answer(d) {   // the page: mres (a read), mack (bytes written), mdone (a file complete or failed)
    if (d.t === 'mres') this.result(d);
    else if (d.t === 'mack') { const w = this.h.get(d.h); if (w) { w.queued -= d.n; if (!d.ok) w.failed = true; if (w.waited && w.queued < 4 << 20) { w.waited = false; this.ready = true; } } }
    else if (d.t === 'mdone') { const dir = this.src.find((s) => s.key === d.key); if (dir) { const e = this.c.get('l|' + d.key + '/VIBE FM/' + this.fw + '|0'); if (e) e.t = -1e9; } if (this.asked) this.ev.push([1, d.ok ? 1 : 0, d.h]); }
  }
}
class VibeHost extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.ok = false;
    const mod = options.processorOptions.module;
    const store = new Map(Object.entries(options.processorOptions.store || {}));   // name -> ArrayBuffer
    const send = (m, transfer) => this.port.postMessage(m, transfer || []);
    this.media = new Media(send, options.processorOptions.fwid || 'firmware');   // SDK 13
    if (options.processorOptions.media) this.media.sources(options.processorOptions.media);
    try {
      let mem = null;
      const text = (p, n) => { let s = ''; const b = new Uint8Array(mem.buffer, p, n); for (const c of b) s += String.fromCharCode(c); try { return decodeURIComponent(escape(s)); } catch (e) { return s; } };
      this.inst = new WebAssembly.Instance(mod, { env: {
        vibe_host_log: (p, n) => send({ t: 'log', text: text(p, n) }),
        vibe_host_request: (what) => send({ t: 'request', what }),
        vibe_host_store: (op, np, nl, dp, len) => {          // 0 read, 1 write, 2 size, 3 delete (name checked by the SDK)
          const name = text(np, nl), f = store.get(name);
          if (op === 0) { if (!f) return -1; const n = Math.min(len, f.byteLength); new Uint8Array(mem.buffer, dp, n).set(new Uint8Array(f, 0, n)); return n; }
          if (op === 1) { const copy = new Uint8Array(mem.buffer, dp, len).slice().buffer; store.set(name, copy); send({ t: 'store', name, data: copy.slice(0) }); return 0; }
          if (op === 2) return f ? f.byteLength : -1;
          if (op === 3) { store.delete(name); send({ t: 'store', name, data: null }); return f ? 0 : -1; }   // (the page's copy goes too, known here or not)
          if (op === 4) {                                    // SDK 4: the names, one per line, as many whole ones as fit
            const out = new Uint8Array(mem.buffer, dp, len); let w = 0;
            for (const k of [...store.keys()].sort()) { const b = utf8(k + '\n'); if (w + b.length > len) break; out.set(b, w); w += b.length; }   // (no TextEncoder in an AudioWorklet)
            return w;
          }
          if (op === 14) {                                   // SDK 19/20: the device (52 bytes, Protokoll 15b) – the page's, e.g. a Launchpad as an 8 x 8 grid
            const dv = options.processorOptions.device; if (!dv || len < dv.length) return -1;
            new Uint8Array(mem.buffer, dp, dv.length).set(dv); return 1;
          }
          if (op === 15) { if (this.media.src.some((s) => s.fixed)) return -1; if (len) send({ t: 'madd' }); return 1; }   // (5.11.: a page with a fixed set – the Jukebox – offers no folder dialog)   // SDK 22: vibe_media_add – the page opens its folder dialog
          if (op >= 6) return this.media.store(op, new Uint8Array(mem.buffer), new DataView(mem.buffer), np, nl, dp, len, text, utf8);   // SDK 13: media
          if (op === 5) {                                    // SDK 4: read at an offset (in the data's first 4 bytes)
            if (!f) return -1; const off = new DataView(mem.buffer).getUint32(dp, true);
            const n = Math.max(0, Math.min(len, f.byteLength - off)); if (n) new Uint8Array(mem.buffer, dp, n).set(new Uint8Array(f, off, n)); return n;
          }
          return -1;
        },
      } });
      const x = this.inst.exports; mem = x.memory; this.x = x;
      this.grid = !!options.processorOptions.device;   // (SDK 20: a grid bigger than 16 – its pads' lights go out with every frame)
      this.sdk = x.vibe_sdk_version();
      if (this.sdk < 1 || this.sdk > 8) throw new Error('firmware needs VIBE SDK ' + this.sdk);
      x.vibe_wasm_init(sampleRate);
      this.io = x.vibe_wasm_io();
      this.rear = x.vibe_wasm_rear ? x.vibe_wasm_rear() : 0;   // SDK 2: cv_out, gate_out, cv_in, in_mode[2], in_link
      this.vt = this.sdk >= 5 && x.vibe_wasm_time ? x.vibe_wasm_time() : 0;   // SDK 5: the time table
      // (SDK 27, 13.11.2026) a MIDI clock's position: the page follows the clock (js/clockfollow.js) and sends its line in the
      // context's heard time ('vcf'); before each block it is written into the firmware's vibe_transport_info (SDK 8)
      this.tp = x.vibe_wasm_transport ? x.vibe_wasm_transport() : 0; this.vcf = null; this.vcfWas = 0;
      this.ok = true;
      // SDK 23: the stage – a second, bigger picture the firmware draws (vibe_stage in its init: size, background)
      this.stage = null; this.stageOn = false;
      if (x.vibe_wasm_stage_info && x.vibe_wasm_stage_draw) { const q = x.vibe_wasm_stage_info(), dv = new DataView(mem.buffer);
        if (dv.getUint8(q + 8)) this.stage = { w: dv.getUint16(q, true), h: dv.getUint16(q + 2, true), bg: dv.getUint32(q + 4, true), replaces: !!(dv.getUint8(q + 9) & 1) }; }
      send({ t: 'ready', sdk: this.sdk, sampleRate, areas: x.vibe_wasm_areas ? x.vibe_wasm_areas() : 0, stage: this.stage });   // (SDK 12: its own areas)
      // SDK 11 (Protokoll 7.2a): the firmware's mixer parameters (mix_*) – the page maps a mixer controller to them
      this.mix = [];
      if (x.vibe_wasm_param_count && x.vibe_wasm_param_info) {
        const dv = new DataView(mem.buffer), cstr = (p) => { let e = p; const b = new Uint8Array(mem.buffer); while (b[e]) e++; return text(p, e - p); };
        for (let i = 0, n = x.vibe_wasm_param_count(); i < n; i++) {
          const q = x.vibe_wasm_param_info(i); if (!q) continue;
          const id = cstr(dv.getUint32(q, true));
          if (id.startsWith('mix_')) this.mix.push({ i, id, min: dv.getFloat32(q + 8, true), max: dv.getFloat32(q + 12, true) });
        }
        if (this.mix.length) send({ t: 'mixer', params: this.mix });
      }
      this.edText = text; this.edDescribe(send);       // (SDK 8–16) the Editor in the browser: what the plugin reads

    } catch (e) { send({ t: 'crash', error: String(e && e.message || e) }); }
    this.port.onmessage = (m) => {
      const d = m.data; if (!this.ok) return;
      try {
        if (d.t === 'ev') {
          if (this.vt) {                                   // Protokoll 5: the frame heard, on the firmware's timeline
            const f = d.heard !== undefined && this.f0 !== undefined ? d.heard - this.f0 : -1, vt = new Uint32Array(this.x.memory.buffer, this.vt, 8);
            vt[0] = f >= 0 ? f >>> 0 : 0; vt[1] = f >= 0 ? Math.floor(f / 4294967296) : 0; vt[4] = f >= 0 ? 1 : 0;
          }
          this.mediaEvents();
          this.x.vibe_wasm_event(d.type, d.id, d.down, d.type === 3 ? 0 : d.value, d.midi || 0);   // (Protokoll 3.2, 3.11.2026: a button's value is always 0 – pressed is only down; every host alike, so a firmware that tests the value fails at once, not only without a controller)
        }
        else if (d.t === 'sysex') {                       // SDK 15 (Protokoll 7.5): a SysEx from a MIDI input → VIBE_EV_SYSEX
          const x = this.x, b = d.bytes;
          if (x.vibe_wasm_sysex_buf && x.vibe_wasm_sysex_in && b.length >= 2 && b.length <= 4096 && b[0] === 0xF0) {
            new Uint8Array(x.memory.buffer, x.vibe_wasm_sysex_buf(), b.length).set(b); x.vibe_wasm_sysex_in(b.length);
          }
        }
        else if (d.t === 'vcf') this.vcf = d.line;          // (SDK 27) the MIDI clock's line, in seconds of heard context time
        else if (d.t === 'latency') { if (this.vt) new Uint32Array(this.x.memory.buffer, this.vt, 8)[2] = d.frames; }
        else if (d.t === 'storeput') store.set(d.name, d.data);
        else if (d.t === 'msrc') this.media.sources(d.list);   // SDK 13: the page's folders
        else if (d.t === 'mres' || d.t === 'mack' || d.t === 'mdone') { this.media.answer(d); this.mediaEvents(); }   // a file dropped onto the device (music for PLAYER), already in IndexedDB
        else if (d.t === 'cvin') { if (this.rear) new Float32Array(this.x.memory.buffer, this.rear, 6)[2] = d.volts; }
        else if (d.t === 'pset') { if (this.x.vibe_wasm_param_set) this.x.vibe_wasm_param_set(d.i, d.v); }   // (SDK 11) a mixer controller; the Editor
        else if (d.t === 'edwant') this.edWant = !!d.on;  // (the Editor is open: values and posts with every picture)
        else if (d.t === 'stage') { if (this.stage && this.stageOn !== !!d.on) { this.stageOn = !!d.on; this.x.vibe_wasm_event(19, d.on ? 8 : 9, 0, 0, 0); } }   // SDK 23: shown / hidden
        else if (d.t === 'curve') {                        // (SDK 9) a card's curve – answered at once (the browser has one flow)
          let pts = [];
          if (this.x.vibe_wasm_editor_curve && this.x.vibe_wasm_editor_points) {
            const n = this.x.vibe_wasm_editor_curve(d.i, this.edTab(d.tab));
            if (n >= 2) pts = Array.from(new Float32Array(this.x.memory.buffer, this.x.vibe_wasm_editor_points(), Math.min(64, n)));
          }
          this.port.postMessage({ t: 'curveR', id: d.id, pts });
        }
        else if (d.t === 'cdraw') { if (this.x.vibe_wasm_editor_curve_draw) this.x.vibe_wasm_editor_curve_draw(d.i, this.edTab(d.tab), d.point, d.count, d.value); }   // (SDK 15)
        else if (d.t === 'press') { if (this.x.vibe_wasm_editor_press) this.x.vibe_wasm_editor_press(d.i, this.edTab(d.tab)); }
        else if (d.t === 'eddata') {                       // (SDK 16) bytes from the firmware's own page
          const b = d.bytes, x = this.x;
          if (x.vibe_wasm_editor_inbox && x.vibe_wasm_editor_data_in && b.length && b.length <= 4096) { new Uint8Array(x.memory.buffer, x.vibe_wasm_editor_inbox(), b.length).set(b); x.vibe_wasm_editor_data_in(b.length); }
        }
        else if (d.t === 'scrwant') this.scrWant = true;   // (SDK 25) the page sent the last picture to the FM-1: the next one, when it changed
        else if (d.t === 'draw') {
          this.lastDraw = currentTime; this.mediaEvents();
          const fbp = this.x.vibe_wasm_draw();
          const fb = new Uint16Array(this.x.memory.buffer, fbp, 480 * 320).slice();
          const leds = new Uint8Array(this.x.memory.buffer, this.x.vibe_wasm_leds(), 384).slice();
          const rear = this.rear ? Array.from(new Float32Array(this.x.memory.buffer, this.rear, 6)) : null;
          const labels = this.x.vibe_wasm_labels ? new Uint8Array(this.x.memory.buffer, this.x.vibe_wasm_labels(), 25 * 8).slice() : null;   // SDK 2 (18.10.): button captions
          const mixv = this.mix && this.mix.length ? this.mix.map((m) => this.x.vibe_wasm_param_value(m.i)) : null;   // (SDK 11: for the controller's LEDs and faders)
          let pv = null, posts = null;                     // (the Editor: every value, and what the firmware posted to its page)
          if (this.edWant && this.edN) {
            pv = []; for (let i = 0; i < this.edN; i++) pv.push(this.x.vibe_wasm_param_value(i));
            if (this.x.vibe_wasm_editor_outbox) {
              const o = this.x.vibe_wasm_editor_outbox(), dv = new DataView(this.x.memory.buffer), n = dv.getUint32(o, true);
              if (n > 0 && n <= 4096) { posts = [new Uint8Array(this.x.memory.buffer, o + 4, n).slice()]; dv.setUint32(o, 0, true); }
            }
          }
          const padx = this.grid && this.x.vibe_wasm_pad_leds ? new Uint8Array(this.x.memory.buffer, this.x.vibe_wasm_pad_leds(), 4 + 128 * 8).slice() : null;   // (SDK 20: n, rgb[128], lvl[128])
          const tr = labels ? [fb.buffer, leds.buffer, labels.buffer] : [fb.buffer, leds.buffer]; if (padx) tr.push(padx.buffer);
          let stage = null;                                // SDK 23: the stage, only while the page shows it
          if (this.stage && this.stageOn) { const sp = this.x.vibe_wasm_stage_draw(); if (sp) { stage = new Uint16Array(this.x.memory.buffer, sp, this.stage.w * this.stage.h).slice(); tr.push(stage.buffer); } }
          let link = null;                                 // SDK 24 (Protokoll 15c): the page and the encoders, as SysEx for a controller with a
          if (this.x.vibe_wasm_link) {                     // screen (an FM-1 under VIBE FM Link) – only when it changed
            const q = this.x.vibe_wasm_link(), ser = new DataView(this.x.memory.buffer).getUint32(q, true);
            if (ser !== this.linkSer) { this.linkSer = ser; link = linkSysex(new Uint8Array(this.x.memory.buffer, q + 4, 20 + 7 * 24)); }
          }
          let scr = null, scrOn = false;                   // SDK 25 (Protokoll 15d): the firmware's 240 x 240 picture (a converted FM-1 firmware) for
          if (this.x.vibe_wasm_link_screen) {              // an FM-1 under VIBE FM Link 0.4 – copied only when the page asks for the next one and it changed
            const q = this.x.vibe_wasm_link_screen(), dv = new DataView(this.x.memory.buffer), ser = dv.getUint32(q, true), px = dv.getUint32(q + 4, true);
            scrOn = px !== 0;
            if (px && this.scrWant && ser !== this.scrSer) { this.scrSer = ser; this.scrWant = false; scr = new Uint16Array(this.x.memory.buffer, px, 240 * 240).slice(); tr.push(scr.buffer); }
          }
          send({ t: 'frame', fb: fb.buffer, leds: leds.buffer, rear, labels: labels && labels.buffer, mixv, pv, posts, padx: padx && padx.buffer, stage: stage && stage.buffer, link, scr: scr && scr.buffer, scrOn }, tr);
        }
      } catch (e) { this.crash(e); }
    };
  }
  // the Editor's description, as the plugin reads it (Protokoll 9.2–9.3): the parameters with their groups and choices,
  // the cards, controls, colours, help, curves, drawable curves, buttons – one message after init ('eddesc')
  edDescribe(send) {
    const x = this.x; this.edN = 0;
    if (!x.vibe_wasm_param_count || !x.vibe_wasm_param_info) { send({ t: 'eddesc', params: [], editor: null }); return; }
    const dv = () => new DataView(x.memory.buffer), u8 = () => new Uint8Array(x.memory.buffer);
    const cstr = (p, max = 256) => { if (!p) return ''; const b = u8(); let e = p; while (b[e] && e - p < max) e++; return this.edText(p, e - p); };
    const params = [];
    for (let i = 0, n = x.vibe_wasm_param_count(); i < n && i < 256; i++) {
      const q = x.vibe_wasm_param_info(i); if (!q) continue; const d = dv();
      const steps = d.getInt32(q + 24, true), cp = d.getUint32(q + 28, true), choices = [];
      if (cp && steps > 0) for (let k = 0; k < steps && k < 128; k++) choices.push(cstr(d.getUint32(cp + 4 * k, true), 40));
      params.push({ i, id: cstr(d.getUint32(q, true), 40), name: cstr(d.getUint32(q + 4, true), 40), min: d.getFloat32(q + 8, true), max: d.getFloat32(q + 12, true),
                    def: d.getFloat32(q + 16, true), unit: cstr(d.getUint32(q + 20, true), 16), steps, choices,
                    group: x.vibe_wasm_param_group ? cstr(x.vibe_wasm_param_group(i), 40) : '', v: x.vibe_wasm_param_value(i) });
    }
    this.edN = params.length;
    let editor = null;
    const ed = x.vibe_wasm_editor ? x.vibe_wasm_editor() : 0;
    if (ed) {
      const d = dv(), cards = [], tabs = {}, help = [], curves = [], draw = [], buttons = [];
      const cp = d.getUint32(ed + 4, true), nc = d.getInt32(ed + 8, true), ctl = d.getUint32(ed + 12, true), tp = d.getUint32(ed + 20, true), nt = d.getInt32(ed + 24, true), hp = d.getUint32(ed + 28, true), ncv = d.getInt32(ed + 32, true);
      for (let i = 0; i < nc && i < 64 && cp; i++) { const c = cp + i * 12, name = cstr(d.getUint32(c, true), 40); if (name) cards.push({ card: name, width: d.getInt32(c + 4, true), note: cstr(d.getUint32(c + 8, true), 120) }); }
      for (let i = 0; i < nt && i < 8 && tp; i++) tabs[cstr(d.getUint32(tp + i * 8, true), 31)] = d.getUint32(tp + i * 8 + 4, true) & 0xFFFFFF;
      if (hp) for (let i = 0; i < params.length; i++) help.push(cstr(d.getUint32(hp + 4 * i, true), 120));
      for (let i = 0; i < ncv && i < 16; i++) { curves.push(x.vibe_wasm_editor_curve_card ? cstr(x.vibe_wasm_editor_curve_card(i), 40) : ''); draw.push(x.vibe_wasm_editor_curve_drawable && x.vibe_wasm_editor_curve_drawable(i) ? 1 : 0); }
      const nb = x.vibe_wasm_editor_buttons ? x.vibe_wasm_editor_buttons() : 0;
      for (let i = 0; i < nb && i < 16; i++) { const q = x.vibe_wasm_editor_button(i); if (!q) continue; const card = cstr(dv().getUint32(q, true), 40), label = cstr(dv().getUint32(q + 4, true), 15); if (card && label) buttons.push({ card, label, i }); }
      editor = { cards, controls: ctl ? Array.from(u8().slice(ctl, ctl + params.length)) : [], accent: d.getUint32(ed + 16, true) & 0xFFFFFF, tabs, help, curves, draw, buttons };
    }
    send({ t: 'eddesc', params, editor });
  }
  edTab(tab) {                                         // (the tab's name into the firmware's buffer – its address, 0 = none)
    const x = this.x; if (!tab || !x.vibe_wasm_editor_tabbuf) return 0;
    const p = x.vibe_wasm_editor_tabbuf(), b = utf8(String(tab)).slice(0, 31), m = new Uint8Array(x.memory.buffer, p, 32);
    m.set(b); m[b.length] = 0; return p;
  }
  mediaEvents() { const m = this.media; if (m.ready && m.asked) { m.ready = false; m.ev.push([2, 1, 0]); } if (!m.ev.length) return; const ev = m.ev; m.ev = []; for (const [id, down, v] of ev) this.x.vibe_wasm_event(12, id, down, v, 0); }   // SDK 13: VIBE_EV_MEDIA
  crash(e) { this.ok = false; this.port.postMessage({ t: 'crash', error: String(e && e.message || e) }); }
  // SDK 9 (Protokoll 7.4): the firmware's MIDI out queue, drained after the block – to the page, each message with the
  // context time of its frame (the page schedules it on the chosen Web MIDI output). w @0, r @4, q[256] @16 (6 bytes),
  // SysEx: len 0 + F0 → the buffer @1552, sysex_len @12
  midiDrain(frame0) {
    const x = this.x; if (!x.vibe_wasm_midi_out) return;
    if (this.mo === undefined) this.mo = x.vibe_wasm_midi_out();
    const dv = new DataView(x.memory.buffer), base = this.mo, w = dv.getUint32(base, true);
    let r = dv.getUint32(base + 4, true);
    if (r === w) return;
    const msgs = [];
    for (; r !== w; r = (r + 1) >>> 0) {
      const q = base + 16 + (r % 256) * 6, frame = dv.getUint16(q, true), st = dv.getUint8(q + 2), len = dv.getUint8(q + 5);
      const at = (frame0 + frame) / sampleRate;
      if (!len && st === 0xF0) { const n = dv.getUint32(base + 12, true); if (n >= 2 && n <= 256) msgs.push([at, ...new Uint8Array(x.memory.buffer, base + 1552, n)]); dv.setUint32(base + 12, 0, true); continue; }
      if (len < 1 || len > 3 || st < 0x80) continue;
      msgs.push(len === 1 ? [at, st] : len === 2 ? [at, st, dv.getUint8(q + 3)] : [at, st, dv.getUint8(q + 3), dv.getUint8(q + 4)]);
    }
    dv.setUint32(base + 4, w, true);
    msgs.sort((a, b) => a[0] - b[0]);
    this.port.postMessage({ t: 'midi', msgs });
  }
  // (SDK 27) vibe_transport_info from the MIDI clock's line (runtime/common/vibe_clock_follow.h's vcf_beat, inline: no import in
  // the audio thread): has@0 playing@4 bpm f32@8 sig 4/4 @12/@16 beat f64@24 bar_start f64@32 loop@40; has 0 once when it stops
  // leading. t: the block's first frame in heard context seconds (the line's own time base)
  transportFill(t) {
    const L = this.vcf, dv = new DataView(this.x.memory.buffer), a = this.tp;
    if (!L || !L.has) { if (this.vcfWas) { dv.setInt32(a, 0, true); dv.setInt32(a + 4, 0, true); this.vcfWas = 0; } return; }
    const beat = L.moving && t > L.a_t ? L.a_beat + (t - L.a_t) * L.slope : L.a_beat;
    dv.setInt32(a, 1, true); dv.setInt32(a + 4, L.playing ? 1 : 0, true); dv.setFloat32(a + 8, L.bpm, true);
    dv.setInt32(a + 12, 4, true); dv.setInt32(a + 16, 4, true);
    dv.setFloat64(a + 24, beat, true); dv.setFloat64(a + 32, Math.floor(beat / 4) * 4, true); dv.setInt32(a + 40, 0, true);
    this.vcfWas = 1;
  }
  process(inputs, outputs) {
    if (!this.ok) return true;
    if (this.f0 === undefined) this.f0 = currentFrame;   // (frame 0 of the firmware's timeline, in context frames)
    const m = this.media;   // (SDK 13: a hidden tab draws no more – READY at least every 33 ms keeps a media firmware's buffers full)
    if (m.asked && currentTime - (this.lastDraw || 0) > 0.05 && currentTime - (this.lastTick || 0) > 0.033) { this.lastTick = currentTime; m.ready = true; try { this.mediaEvents(); } catch (e) { this.crash(e); return true; } }
    const out = outputs[0], n = out[0].length, in1 = inputs[0] && inputs[0][0], in2 = this.sdk >= 2 && inputs[0] && inputs[0][1];
    try {
      const buf = new Float32Array(this.x.memory.buffer, this.io, 256 * 4);
      if (in1) buf.set(in1.subarray(0, n), 0);
      if (in2) buf.set(in2.subarray(0, n), 768);
      if (this.tp) this.transportFill(currentFrame / sampleRate);   // (SDK 27) where the master is when this block is heard
      this.x.vibe_wasm_audio(n, (in1 ? 1 : 0) | (in2 ? 2 : 0));
      out[0].set(buf.subarray(256, 256 + n));
      if (out[1]) out[1].set(buf.subarray(512, 512 + n));
      this.midiDrain(currentFrame);                      // (SDK 9) what the firmware sent, with its frames' times
    } catch (e) { this.crash(e); }
    return true;
  }
}
// SDK 24 (Protokoll 15c): the firmware's Link state (page[20], name[7][12], value[7][12]) as one SysEx:
// F0 7D 56 4C 01, the page, then name and value of encoders 1-4, A, B, DATA, each ended by 00, F7 (7-bit ASCII only)
function linkSysex(b) {
  const out = [0xF0, 0x7D, 0x56, 0x4C, 0x01];
  const str = (o, n) => { for (let i = 0; i < n && b[o + i]; i++) out.push(b[o + i] & 0x7F); out.push(0); };
  str(0, 20);
  for (let e = 0; e < 7; e++) { str(20 + e * 12, 12); str(20 + 84 + e * 12, 12); }
  out.push(0xF7);
  return out;
}
registerProcessor('vibe-host', VibeHost);

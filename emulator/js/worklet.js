// worklet.js – runs the firmware (.wasm) inside the browser's audio thread. Everything the firmware does happens
// here, one call after the other, exactly like on the device: control events, audio blocks of 128 frames, and a
// display frame when the page asks for one. A firmware crash stops only the firmware, never the page.
// SDK 2 adds the rear panel: IN 1/IN 2 (input channels 0/1), CV/Gate out, CV IN (struct vibe_rear in vibe_sdk.c).
// SDK 3 adds storage: the page hands over the firmware's saved files at start (processorOptions.store), reads are
// answered from that copy, every write goes back to the page, which keeps it in IndexedDB.
// UTF-8 bytes of a string – the AudioWorkletGlobalScope has no TextEncoder (28.10.: PLAYER's file list crashed with files in it)
function utf8(str) { const s = unescape(encodeURIComponent(str)), b = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i); return b; }
class VibeHost extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.ok = false;
    const mod = options.processorOptions.module;
    const store = new Map(Object.entries(options.processorOptions.store || {}));   // name -> ArrayBuffer
    const send = (m, transfer) => this.port.postMessage(m, transfer || []);
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
          if (op === 5) {                                    // SDK 4: read at an offset (in the data's first 4 bytes)
            if (!f) return -1; const off = new DataView(mem.buffer).getUint32(dp, true);
            const n = Math.max(0, Math.min(len, f.byteLength - off)); if (n) new Uint8Array(mem.buffer, dp, n).set(new Uint8Array(f, off, n)); return n;
          }
          return -1;
        },
      } });
      const x = this.inst.exports; mem = x.memory; this.x = x;
      this.sdk = x.vibe_sdk_version();
      if (this.sdk < 1 || this.sdk > 8) throw new Error('firmware needs VIBE SDK ' + this.sdk);
      x.vibe_wasm_init(sampleRate);
      this.io = x.vibe_wasm_io();
      this.rear = x.vibe_wasm_rear ? x.vibe_wasm_rear() : 0;   // SDK 2: cv_out, gate_out, cv_in, in_mode[2], in_link
      this.vt = this.sdk >= 5 && x.vibe_wasm_time ? x.vibe_wasm_time() : 0;   // SDK 5: the time table
      this.ok = true;
      send({ t: 'ready', sdk: this.sdk, sampleRate });
    } catch (e) { send({ t: 'crash', error: String(e && e.message || e) }); }
    this.port.onmessage = (m) => {
      const d = m.data; if (!this.ok) return;
      try {
        if (d.t === 'ev') {
          if (this.vt) {                                   // Protokoll 5: the frame heard, on the firmware's timeline
            const f = d.heard !== undefined && this.f0 !== undefined ? d.heard - this.f0 : -1, vt = new Uint32Array(this.x.memory.buffer, this.vt, 8);
            vt[0] = f >= 0 ? f >>> 0 : 0; vt[1] = f >= 0 ? Math.floor(f / 4294967296) : 0; vt[4] = f >= 0 ? 1 : 0;
          }
          this.x.vibe_wasm_event(d.type, d.id, d.down, d.value, d.midi || 0);
        }
        else if (d.t === 'latency') { if (this.vt) new Uint32Array(this.x.memory.buffer, this.vt, 8)[2] = d.frames; }
        else if (d.t === 'storeput') store.set(d.name, d.data);   // a file dropped onto the device (music for PLAYER), already in IndexedDB
        else if (d.t === 'cvin') { if (this.rear) new Float32Array(this.x.memory.buffer, this.rear, 6)[2] = d.volts; }
        else if (d.t === 'draw') {
          const fbp = this.x.vibe_wasm_draw();
          const fb = new Uint16Array(this.x.memory.buffer, fbp, 480 * 320).slice();
          const leds = new Uint8Array(this.x.memory.buffer, this.x.vibe_wasm_leds(), 384).slice();
          const rear = this.rear ? Array.from(new Float32Array(this.x.memory.buffer, this.rear, 6)) : null;
          const labels = this.x.vibe_wasm_labels ? new Uint8Array(this.x.memory.buffer, this.x.vibe_wasm_labels(), 25 * 8).slice() : null;   // SDK 2 (18.10.): button captions
          send({ t: 'frame', fb: fb.buffer, leds: leds.buffer, rear, labels: labels && labels.buffer }, labels ? [fb.buffer, leds.buffer, labels.buffer] : [fb.buffer, leds.buffer]);
        }
      } catch (e) { this.crash(e); }
    };
  }
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
  process(inputs, outputs) {
    if (!this.ok) return true;
    if (this.f0 === undefined) this.f0 = currentFrame;   // (frame 0 of the firmware's timeline, in context frames)
    const out = outputs[0], n = out[0].length, in1 = inputs[0] && inputs[0][0], in2 = this.sdk >= 2 && inputs[0] && inputs[0][1];
    try {
      const buf = new Float32Array(this.x.memory.buffer, this.io, 256 * 4);
      if (in1) buf.set(in1.subarray(0, n), 0);
      if (in2) buf.set(in2.subarray(0, n), 768);
      this.x.vibe_wasm_audio(n, (in1 ? 1 : 0) | (in2 ? 2 : 0));
      out[0].set(buf.subarray(256, 256 + n));
      if (out[1]) out[1].set(buf.subarray(512, 512 + n));
      this.midiDrain(currentFrame);                      // (SDK 9) what the firmware sent, with its frames' times
    } catch (e) { this.crash(e); }
    return true;
  }
}
registerProcessor('vibe-host', VibeHost);

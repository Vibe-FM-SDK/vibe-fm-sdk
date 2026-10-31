// SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK.
// vibe-editor.js – SDK 16 (31.10.2026): the API of a firmware's own Editor page. The host serves this file next to the
// page (fw-editor/<id>/vibe-editor.js); the page loads it with <script src="vibe-editor.js"></script> and then has
// window.vibe. The page runs in a sandbox: everything goes as postMessage to the host's window (editor.js, PageHost),
// which checks it and passes it on. See EDITOR-PAGE.md.
(function () {
  'use strict';
  const listeners = { values: [], data: [], frame: [] };
  const waiting = new Map(); let seq = 0, wantFrames = false;
  const post = (m) => window.parent.postMessage(Object.assign({ vibe: 1 }, m), '*');
  const ask = (op, args) => new Promise((res) => { const id = ++seq; waiting.set(id, res); post({ op, args, id }); });
  let ready; const vibe = {
    ready: new Promise((r) => { ready = r; }),
    params: [], values: [], curves: [], tabs: [], theme: {},
    // a parameter's value (min..max): to the firmware and to the DAW, which records it
    set(i, v) {
      const p = vibe.params[i]; if (!p) return;
      v = Math.min(p.max, Math.max(p.min, +v));
      if (p.steps > 1) { const st = (p.max - p.min) / (p.steps - 1); v = p.min + Math.round((v - p.min) / st) * st; }
      vibe.values[i] = v; post({ op: 'set', args: [i, v] });
    },
    byId(id) { return vibe.params.find((p) => p.id === id); },
    onValues(cb) { listeners.values.push(cb); if (vibe.values.length) cb(vibe.values); },
    // a card's curve (vibe_editor_curve) – points 0..1, [] while the firmware has not made it yet
    curve: (card, tab = '') => ask('curve', [card, tab]),
    draw(card, tab, point, count, value) { post({ op: 'draw', args: [card, tab || '', point | 0, count | 0, +value] }); },
    // bytes to the firmware (VIBE_EV_EDITOR_DATA) and from it (vibe_editor_post) – up to 4096 a message
    send(bytes) { const b = bytes instanceof Uint8Array ? bytes : typeof bytes === 'string' ? new TextEncoder().encode(bytes) : Uint8Array.from(bytes); if (b.length && b.length <= 4096) post({ op: 'send', args: [b] }); },
    onData(cb) { listeners.data.push(cb); },
    // a note, as from the keys (velocity 0..1; 0 = off)
    note(n, vel = 0.8) { post({ op: 'note', args: [n | 0, +vel] }); },
    // the display (480 × 320, RGB565 in a Uint16Array) – only while someone listens
    onFrame(cb) { listeners.frame.push(cb); if (!wantFrames) { wantFrames = true; post({ op: 'frames', args: [true] }); } },
    // RGB565 → a canvas (a helper for onFrame)
    paint(canvas, fb) {
      const g = canvas.getContext('2d'), img = g.createImageData(480, 320), d = img.data;
      for (let i = 0, j = 0; i < fb.length; i++, j += 4) { const v = fb[i]; d[j] = (v >> 8) & 0xF8; d[j + 1] = (v >> 3) & 0xFC; d[j + 2] = (v << 3) & 0xF8; d[j + 3] = 255; }
      g.putImageData(img, 0, 0);
    },
  };
  window.addEventListener('message', (e) => {
    if (e.source !== window.parent || !e.data || e.data.vibe !== 1) return;
    const m = e.data;
    if (m.reply) { const r = waiting.get(m.reply); waiting.delete(m.reply); if (r) r(m.value); return; }
    if (m.op === 'values') { vibe.values = m.args[0]; for (const cb of listeners.values) cb(vibe.values); }
    else if (m.op === 'data') { for (const cb of listeners.data) cb(m.args[0]); }
    else if (m.op === 'frame') { for (const cb of listeners.frame) cb(m.args[0]); }
  });
  ask('hello', []).then((h) => {
    Object.assign(vibe, { params: h.params, values: h.values, curves: h.curves, tabs: h.tabs, theme: h.theme });
    const r = document.documentElement.style;          // (the host's look as CSS variables – use them or not)
    for (const [k, v] of Object.entries(h.theme)) r.setProperty('--vibe-' + k, v);
    ready(vibe);
  });
  window.vibe = vibe;
})();

// SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE.
// fm1-midi.js – (5.11.2026, test) an FM-1 firmware's own web editor, inside the Web Player / the plugin: Web MIDI as the
// editor knows it, but its one port is the converted firmware running right here. The host serves this file next to the
// editor page (firmware/<id>.editor/) and loads it before the editor's own scripts; the page is in a sandboxed iframe, so
// everything goes by postMessage (editor.js pageMsg op 'midi' → the firmware; its MIDI out back as {vibe:1, midi:[…]}).
// The firmware's editor code is the same as on the FM-1 (Felucca's SysEx F0 7D 46 4C …), so the editor notices nothing.
(() => {
  const NAME = (document.currentScript && document.currentScript.dataset.port) || 'FM-1 (VIBE FM)';
  const toHost = (bytes) => parent.postMessage({ vibe: 1, op: 'midi', args: [Uint8Array.from(bytes)] }, '*');
  const listeners = new Set();
  const input = {
    id: 'vibe-fm1-in', name: NAME, manufacturer: 'VIBE FM', type: 'input', state: 'connected', connection: 'open', version: '1',
    onmidimessage: null, onstatechange: null,
    open() { return Promise.resolve(this); }, close() { return Promise.resolve(this); },
    addEventListener(t, f) { if (t === 'midimessage') listeners.add(f); }, removeEventListener(t, f) { if (t === 'midimessage') listeners.delete(f); },
  };
  const output = {
    id: 'vibe-fm1-out', name: NAME, manufacturer: 'VIBE FM', type: 'output', state: 'connected', connection: 'open', version: '1',
    onstatechange: null,
    open() { return Promise.resolve(this); }, close() { return Promise.resolve(this); }, clear() {},
    send(data) {                                       // (one call may hold several messages: split, SysEx whole)
      const b = Array.from(data); let k = 0;
      while (k < b.length) {
        const st = b[k];
        if (st === 0xF0) { const e = b.indexOf(0xF7, k); const m = b.slice(k, e < 0 ? b.length : e + 1); toHost(m); k += m.length; continue; }
        const n = st >= 0xF8 ? 1 : (st & 0xF0) === 0xC0 || (st & 0xF0) === 0xD0 ? 2 : 3;
        if (st >= 0x80) toHost(b.slice(k, k + n)); k += n;
      }
    },
    addEventListener() {}, removeEventListener() {},
  };
  addEventListener('message', (e) => {
    if (e.source !== parent || !e.data || e.data.vibe !== 1 || !Array.isArray(e.data.midi)) return;
    for (const m of e.data.midi) {
      const ev = { data: Uint8Array.from(m), timeStamp: performance.now(), target: input };
      if (typeof input.onmidimessage === 'function') input.onmidimessage(ev);
      for (const f of listeners) f(ev);
    }
  });
  const access = { inputs: new Map([[input.id, input]]), outputs: new Map([[output.id, output]]), sysexEnabled: true,
                   onstatechange: null, addEventListener() {}, removeEventListener() {} };
  const req = () => Promise.resolve(access);
  try { Object.defineProperty(navigator, 'requestMIDIAccess', { value: req, configurable: true }); } catch (e) { navigator.requestMIDIAccess = req; }
  try { navigator.permissions.query = ((q) => (n) => n && n.name === 'midi' ? Promise.resolve({ state: 'granted', onchange: null }) : q(n))(navigator.permissions.query.bind(navigator.permissions)); } catch (e) { /* none */ }
  // the page is sandboxed (no same origin): its localStorage throws – a store in memory instead, so an editor that keeps
  // its settings there still runs
  try { localStorage.length; } catch (e) {
    const m = new Map(), st = { getItem: (k) => (m.has(String(k)) ? m.get(String(k)) : null), setItem: (k, v) => m.set(String(k), String(v)),
      removeItem: (k) => m.delete(String(k)), clear: () => m.clear(), key: (i) => [...m.keys()][i] ?? null, get length() { return m.size; } };
    try { Object.defineProperty(window, 'localStorage', { value: st, configurable: true }); } catch (x) { /* stays as it is */ }
    try { Object.defineProperty(window, 'sessionStorage', { value: st, configurable: true }); } catch (x) { /* stays as it is */ }
  }
})();

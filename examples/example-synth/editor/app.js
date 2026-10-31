// SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK.
// example-synth's own page: everything goes through window.vibe (vibe-editor.js). The firmware stays the truth – the page
// shows its values (vibe.onValues) and sends what you do (vibe.set, vibe.note).
(async () => {
  await vibe.ready;
  const P = (id) => vibe.byId(id);
  const cutoff = P('cutoff'), reso = P('reso');
  const norm = (p, v) => (v - p.min) / (p.max - p.min), plain = (p, n) => p.min + n * (p.max - p.min);
  const fmt = (p, v) => (v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2)) + (p.unit ? ' ' + p.unit : '');

  // the filter pad: cutoff across, resonance up
  const pad = document.getElementById('pad'), dot = document.getElementById('dot');
  const padAt = (e) => {
    const r = pad.getBoundingClientRect(), x = Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)), y = Math.min(1, Math.max(0, 1 - (e.clientY - r.top) / r.height));
    vibe.set(cutoff.i, plain(cutoff, x * x));       // (squared: more room for the low cutoffs)
    vibe.set(reso.i, plain(reso, y));
    show(vibe.values);
  };
  pad.addEventListener('pointerdown', (e) => { pad.setPointerCapture(e.pointerId); padAt(e); });
  pad.addEventListener('pointermove', (e) => { if (e.buttons) padAt(e); });

  // three sliders
  const sliders = ['attack', 'release', 'volume'].map((id) => {
    const p = P(id), el = document.getElementById(id), out = document.getElementById(id + '-v');
    el.addEventListener('input', () => { vibe.set(p.i, plain(p, el.value / 1000)); show(vibe.values); });
    return { p, el, out };
  });

  // the values as they are (the DAW's automation, the device's knobs, a preset) – not the slider in your hand
  function show(v) {
    dot.style.left = Math.sqrt(norm(cutoff, v[cutoff.i])) * 100 + '%';
    dot.style.top = (1 - norm(reso, v[reso.i])) * 100 + '%';
    document.getElementById('cut').textContent = 'Cutoff ' + fmt(cutoff, v[cutoff.i]);
    document.getElementById('res').textContent = 'Reso ' + fmt(reso, v[reso.i]);
    for (const s of sliders) {
      if (document.activeElement !== s.el) s.el.value = Math.round(norm(s.p, v[s.p.i]) * 1000);
      s.out.textContent = fmt(s.p, v[s.p.i]);
    }
  }
  vibe.onValues(show);

  // two octaves from C3: a note as from the keys
  const keys = document.getElementById('keys');
  for (let n = 48; n < 72; n++) {
    const b = document.createElement('button'); if ([1, 3, 6, 8, 10].includes(n % 12)) b.className = 'black';
    b.addEventListener('pointerdown', () => { b.classList.add('on'); vibe.note(n, 0.8); });
    for (const ev of ['pointerup', 'pointerleave']) b.addEventListener(ev, () => { if (b.classList.contains('on')) { b.classList.remove('on'); vibe.note(n, 0); } });
    keys.appendChild(b);
  }

  // the device's display, small
  const disp = document.getElementById('disp');
  vibe.onFrame((fb) => vibe.paint(disp, fb));
})();

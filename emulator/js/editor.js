// editor.js – the Editor view of the VIBE FM VST (27.10.2026): every parameter of the playing firmware at once – no pages, no SHIFT
// layers. Built from what the firmware declares (SDK 8: vibe_params + vibe_param_groups): groups "A · FILTER" become a
// tab (A) and a card (FILTER); a group without "·" is a card of its own. The display runs alongside, small.
// The plugin's bindings: vibe_params() (the list as JSON, with values), vibe_param_values() (the values now),
// vibe_param_set(i, value) (a plain value – the plugin passes it to the firmware and to the DAW, which records it).
// SDK 9 (28.10.): vibe_editor() – how the firmware wants it: its cards in its order and width, a control per parameter
// (slider, toggle, menu, bipolar, an XY pad of two), its accent and tab colours, a curve per card (vibe_editor_curve(i, tab),
// polled: the plugin makes it on the audio thread), a help line per parameter. Without it: as before.

const h = (tag, cls, parent, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; if (parent) parent.appendChild(e); return e; };

export class Editor {
  constructor() {
    this.el = h('section', 'editor', document.body); this.el.hidden = true;
    const side = h('div', 'ed-side', this.el);
    this.canvas = h('canvas', 'ed-disp', side); this.canvas.width = 480; this.canvas.height = 320;
    this.ctx = this.canvas.getContext('2d'); this.img = this.ctx.createImageData(480, 320);
    this.tabs = h('div', 'ed-tabs', side);
    this.note = h('p', 'ed-note', side);
    this.main = h('div', 'ed-main', this.el);
    this.params = []; this.knobs = new Map(); this.tab = null; this.pending = new Map(); this.held = -1; this.touched = new Map();
    this.timer = 0;
    this.ed = null; this.curves = new Map(); this.curveLast = new Map();   // (SDK 9) the description; card -> canvas; card -> last points JSON
  }
  get open() { return !this.el.hidden; }

  async show(on) {
    this.el.hidden = !on;
    clearInterval(this.timer); this.timer = 0;
    if (on) { await this.reload(); this.timer = setInterval(() => this.poll(), 120); }
  }
  // the firmware changed (or the Editor opened): its list again
  async reload() {
    let list = [];
    try { list = JSON.parse(await window.vibe_params()); } catch (e) { list = []; }
    this.params = list;
    this.ed = null;
    if (window.vibe_editor) { try { const d = JSON.parse(await window.vibe_editor()); if (d && d.cards) this.ed = d; } catch (e) { this.ed = null; } }
    const tabs = [];
    for (const p of list) { const t = this.tabOf(p); if (!tabs.includes(t)) tabs.push(t); }
    if (!tabs.includes(this.tab)) this.tab = tabs[0] ?? '';
    this.tabs.textContent = '';
    if (tabs.length > 1) for (const t of tabs) {
      const b = h('button', 'ed-tab' + (t === this.tab ? ' on' : ''), this.tabs, t || 'ALL'); b.type = 'button';
      b.addEventListener('click', () => { this.tab = t; this.reloadView(); [...this.tabs.children].forEach((c) => c.classList.toggle('on', c === b)); });
    }
    this.note.textContent = !list.length ? 'This firmware declares no parameters – play it in the device view.'
      : list.length <= 7 && list.every((p) => !p.group) ? 'A converted FM-1 firmware: its seven knobs. Its own pages are on the device view.'
      : `${list.length} parameters · drag (Shift: fine), wheel, double-click = default`;
    this.reloadView();
  }
  tabOf(p) { const g = p.group || ''; const k = g.indexOf(' · '); return k > 0 ? g.slice(0, k) : ''; }
  cardOf(p) { const g = p.group || ''; const k = g.indexOf(' · '); return (k > 0 ? g.slice(k + 3) : g) || 'PARAMETERS'; }
  // the cards in a synth's order (sources, filter, envelopes, modulation, voice, arp, output, effects) – a firmware may
  // list its parameters otherwise (OberVave keeps its first 32 where DAW projects expect them); the rest as listed
  rank(card, first) {
    if (this.ed && this.ed.cards.length) { const k = this.ed.cards.findIndex((c) => c.card === card); if (k >= 0) return k; return 100000 + first; }   // (SDK 9: the firmware's order; the rest after)
    const R = [[/^INPUT/, 0], [/^OSC|^OSCILL|^SOURCE|^TONE/, 1], [/MOD$|^OSC MOD/, 2], [/^FILTER ENV|^FENV/, 4], [/^FILTER|^VCF/, 3],
               [/^AMP|^ENV|^VCA/, 5], [/^LFO DEST|^LFO \+/, 7], [/^LFO/, 6], [/^TUNE|^PITCH/, 8], [/^VOICE/, 9], [/^ARP|^SEQ/, 10],
               [/^OUTPUT|^MIX|^MASTER/, 12], [/^FX|^CHORUS|^DELAY|^REVERB|^HALL/, 11]];
    for (const [re, r] of R) if (re.test(card)) return r * 1000 + first;
    return 500 + first;
  }
  reloadView() {
    this.main.textContent = ''; this.knobs.clear(); this.main.scrollTop = 0;
    // where you are: a heading and the tab's colour (B, C and D look alike otherwise – the same cards)
    const tabs = [...new Set(this.params.map((p) => this.tabOf(p)))], ti = Math.max(0, tabs.indexOf(this.tab));
    const hex = (n) => '#' + (n >>> 0).toString(16).padStart(6, '0');
    const own = this.ed && (this.ed.tabs[this.tab] || this.ed.accent);   // (SDK 9: the firmware's colours – the tab's, else its accent)
    this.el.style.setProperty('--ed-acc', own ? hex(own) : ['#FF60BE', '#40E0D0', '#FFC94A', '#A98BFF', '#FF8A3D', '#7CE07C'][ti % 6]);
    this.main.classList.toggle('ed-grid12', !!(this.ed && this.ed.cards.length));
    this.curves.clear();
    if (this.tab) {
      const head = h('div', 'ed-head', this.main);
      h('h2', '', head, /^[A-Z]$/.test(this.tab) ? 'TRACK ' + this.tab : this.tab === 'FX' ? 'EFFECTS' : this.tab);
      h('span', '', head, this.params.filter((p) => this.tabOf(p) === this.tab).length + ' parameters');
    }
    const cards = new Map(), order = new Map();
    this.params.forEach((p, n) => { if (this.tabOf(p) === this.tab && !order.has(this.cardOf(p))) order.set(this.cardOf(p), n); });
    const sorted = [...order.keys()].sort((a, b) => this.rank(a, order.get(a)) - this.rank(b, order.get(b)));
    for (const c of sorted) {
      const card = h('div', 'ed-card', this.main); h('h3', '', card, (this.tab ? this.tab + ' · ' : '') + c);
      const d = this.ed && this.ed.cards.find((x) => x.card === c);
      if (d) { card.style.gridColumn = `span ${d.width >= 1 && d.width <= 12 ? d.width : 4}`; if (d.note) h('p', 'ed-cnote', card, d.note); }
      else if (this.ed && this.ed.cards.length) card.style.gridColumn = 'span 4';
      if (this.ed && this.ed.curves.includes(c)) { const cv = h('canvas', 'ed-curve', card); cv.width = 256; cv.height = 48; this.curves.set(c, cv); this.curveLast.delete(c); }
      cards.set(c, h('div', 'ed-ctrls', card));
    }
    let skip = -1;
    for (const p of this.params) {
      if (this.tabOf(p) !== this.tab || p.i === skip) continue;
      const c = this.cardOf(p), ctl = this.ed && this.ed.controls ? this.ed.controls[p.i] | 0 : 0;
      if (ctl === 5) {                                 // (SDK 9) XY: this one and the next as one pad
        const q = this.params[p.i + 1];
        if (q && this.cardOf(q) === c && this.tabOf(q) === this.tab) { const [kx, ky] = this.xyPad(p, q, cards.get(c)); kx.update(p.v); ky.update(q.v); this.knobs.set(p.i, kx); this.knobs.set(q.i, ky); skip = q.i; continue; }
      }
      const k = this.control(p, cards.get(c), ctl); k.update(p.v); this.knobs.set(p.i, k);
    }
    this.pollCurves(true);
  }
  // (SDK 9) a card's curve: asked from the plugin, which makes it on the audio thread – polled until it answers,
  // again when a value of that card changed (the knobs' values are what we see; force: after a rebuild)
  async pollCurves(force) {
    if (!this.ed || !this.curves.size || !window.vibe_editor_curve) return;
    for (const [c, cv] of this.curves) {
      const i = this.ed.curves.indexOf(c); if (i < 0) continue;
      const key = this.params.filter((p) => this.tabOf(p) === this.tab && this.cardOf(p) === c).map((p) => (this.knobs.get(p.i) || {}).v).join(',');
      const last = this.curveLast.get(c);
      if (!force && last && last.key === key && last.pts) continue;
      let pts; try { pts = JSON.parse(await window.vibe_editor_curve(i, this.tab)); } catch (e) { pts = []; }
      if (!pts.length) { this.curveLast.set(c, { key, pts: null }); continue; }
      const js = JSON.stringify(pts);
      if (last && last.js === js && last.pts) { last.key = key; continue; }
      this.curveLast.set(c, { key, pts, js });
      const g = cv.getContext('2d'), W = cv.width, H = cv.height, acc = getComputedStyle(this.el).getPropertyValue('--ed-acc').trim() || '#FF60BE';
      g.clearRect(0, 0, W, H); g.fillStyle = '#1c1f24'; g.fillRect(0, 0, W, H);
      g.beginPath(); pts.forEach((y, k) => { const x = k / (pts.length - 1) * (W - 4) + 2, yy = H - 4 - y * (H - 8); if (k) g.lineTo(x, yy); else g.moveTo(x, yy); });
      g.strokeStyle = acc; g.lineWidth = 2; g.lineJoin = 'round'; g.stroke();
      g.lineTo(W - 2, H - 2); g.lineTo(2, H - 2); g.closePath(); g.fillStyle = acc + '33'; g.fill();
    }
  }
  // (SDK 9) two parameters as one pad: X across, Y up
  xyPad(px, py, parent) {
    const box = h('div', 'ed-ctl ed-ctl-xy', parent);
    const pad = h('div', 'ed-xy', box), dot = h('div', 'ed-dot', pad);
    const lab = h('span', 'ed-lab', box, `${this.short(px)} × ${this.short(py)}`), out = h('span', 'ed-val', box);
    const help = this.ed.help && (this.ed.help[px.i] || this.ed.help[py.i]); if (help) box.title = help;
    const norm = (p, v) => p.max > p.min ? (v - p.min) / (p.max - p.min) : 0;
    const kx = { p: px, v: px.v }, ky = { p: py, v: py.v };
    const show = () => { dot.style.left = (norm(px, kx.v) * 100) + '%'; dot.style.top = ((1 - norm(py, ky.v)) * 100) + '%'; out.textContent = `${this.fmt(px, kx.v)} · ${this.fmt(py, ky.v)}`; };
    kx.update = (v) => { kx.v = v; show(); }; ky.update = (v) => { ky.v = v; show(); };
    const at = (e) => { const r = pad.getBoundingClientRect(); const x = Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)), y = Math.min(1, Math.max(0, 1 - (e.clientY - r.top) / r.height)); this.set(px, px.min + x * (px.max - px.min)); this.set(py, py.min + y * (py.max - py.min)); };
    pad.addEventListener('pointerdown', (e) => { pad.setPointerCapture(e.pointerId); this.held = px.i; this.heldY = py.i; at(e); e.preventDefault(); });
    pad.addEventListener('pointermove', (e) => { if (this.held === px.i) at(e); });
    const up = () => { if (this.held === px.i) { this.held = -1; this.heldY = -1; } };
    pad.addEventListener('pointerup', up); pad.addEventListener('pointercancel', up);
    pad.addEventListener('dblclick', () => { this.set(px, px.def); this.set(py, py.def); });
    return [kx, ky];
  }
  short(p) {
    return (this.tab && p.name.startsWith(this.tab + ' ') ? p.name.slice(this.tab.length + 1) : p.name)
      .replace(new RegExp('^' + this.cardOf(p).replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + ' '), '');
  }

  // a control: a knob, or for named steps a row of buttons (up to 6) or a menu – or what the firmware asked for (SDK 9,
  // ctl: 1 slider, 2 toggle, 3 menu, 4 bipolar knob)
  control(p, parent, ctl = 0) {
    const box = h('div', 'ed-ctl', parent);
    const short = this.short(p);
    if (this.ed && this.ed.help && this.ed.help[p.i]) box.title = this.ed.help[p.i];
    if (ctl === 2 && (p.steps === 2 || (p.choices && p.choices.length === 2))) {   // a toggle: one button, lit when on
      const b = h('button', 'ed-tog', box, short); b.type = 'button';
      b.addEventListener('click', () => this.set(p, this.step(p, this.knobs.get(p.i)?.v ?? p.v) ? p.min : p.max));
      return { p, v: p.v, update: (v) => { const k = { p, v }; this.knobs.get(p.i) && (this.knobs.get(p.i).v = v); b.classList.toggle('on', this.step(p, v) > 0); b.textContent = short + ' · ' + this.fmt(p, v); void k; } };
    }
    if (ctl === 1 && !(p.choices && p.choices.length)) {                      // a slider across the card
      box.classList.add('ed-ctl-sl');
      const row = h('div', 'ed-slrow', box); h('span', 'ed-lab', row, short); const out = h('span', 'ed-val', row);
      const r = h('input', 'ed-slider', box); r.type = 'range'; r.min = 0; r.max = 1000; r.step = 1;
      r.addEventListener('input', () => this.set(p, p.min + (+r.value / 1000) * (p.max - p.min)));
      r.addEventListener('dblclick', () => this.set(p, p.def));
      const k = { p, v: p.v, update: (v) => { k.v = v; if (document.activeElement !== r) r.value = Math.round((p.max > p.min ? (v - p.min) / (p.max - p.min) : 0) * 1000); out.textContent = this.fmt(p, v); } };
      return k;
    }
    if (p.choices && p.choices.length) {
      h('span', 'ed-lab', box, short);
      if (p.choices.length <= 6 && ctl !== 3) {
        const row = h('div', 'ed-seg', box);
        const btns = p.choices.map((c, k) => { const b = h('button', '', row, c); b.type = 'button'; b.addEventListener('click', () => this.set(p, p.min + k * (p.max - p.min) / Math.max(1, p.choices.length - 1))); return b; });
        return { p, update: (v) => { const k = this.step(p, v); btns.forEach((b, n) => b.classList.toggle('on', n === k)); } };
      }
      const sel = h('select', '', box); p.choices.forEach((c, k) => { const o = h('option', '', sel, c); o.value = k; });
      sel.addEventListener('change', () => this.set(p, p.min + (+sel.value) * (p.max - p.min) / Math.max(1, p.choices.length - 1)));
      return { p, update: (v) => { sel.value = this.step(p, v); } };
    }
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg'); svg.setAttribute('viewBox', '0 0 64 64'); svg.classList.add('ed-knob'); box.appendChild(svg);
    const arc = (cls) => { const a = document.createElementNS('http://www.w3.org/2000/svg', 'path'); a.setAttribute('class', cls); svg.appendChild(a); return a; };
    const track = arc('trk'), val = arc('val'); track.setAttribute('d', this.arcPath(0, 1));
    const dot = document.createElementNS('http://www.w3.org/2000/svg', 'line'); dot.setAttribute('class', 'ptr'); svg.appendChild(dot);
    h('span', 'ed-lab', box, short);
    const out = h('span', 'ed-val', box);
    const k = { p, v: p.v, update: (v) => {
      k.v = v; const n = p.max > p.min ? (v - p.min) / (p.max - p.min) : 0;
      const z = (p.min < 0 && p.max > 0) || ctl === 4 ? (p.min < 0 && p.max > 0 ? -p.min / (p.max - p.min) : 0.5) : 0;      // (bipolar: the arc from the middle)
      val.setAttribute('d', this.arcPath(Math.min(z, n), Math.max(z, n)));
      const a = (-135 + 270 * n) * Math.PI / 180;
      dot.setAttribute('x1', 32 + 12 * Math.sin(a)); dot.setAttribute('y1', 34 - 12 * Math.cos(a));
      dot.setAttribute('x2', 32 + 22 * Math.sin(a)); dot.setAttribute('y2', 34 - 22 * Math.cos(a));
      out.textContent = this.fmt(p, v);
    } };
    let y0 = 0, v0 = 0;
    svg.addEventListener('pointerdown', (e) => { svg.setPointerCapture(e.pointerId); y0 = e.clientY; v0 = k.v; this.held = p.i; e.preventDefault(); });
    svg.addEventListener('pointermove', (e) => {
      if (this.held !== p.i) return;
      const span = p.max - p.min, d = (y0 - e.clientY) / (e.shiftKey ? 900 : 180) * span;
      this.set(p, v0 + d);
    });
    const up = () => { if (this.held === p.i) this.held = -1; };
    svg.addEventListener('pointerup', up); svg.addEventListener('pointercancel', up);
    svg.addEventListener('wheel', (e) => { e.preventDefault(); const st = p.steps > 1 ? (p.max - p.min) / (p.steps - 1) : (p.max - p.min) / 100; this.set(p, k.v - Math.sign(e.deltaY) * st * (e.shiftKey ? 1 : (p.steps > 32 ? 2 : 1))); }, { passive: false });
    svg.addEventListener('dblclick', () => this.set(p, p.def));
    return k;
  }
  arcPath(a, b) {
    const pt = (n) => { const r = (-135 + 270 * n) * Math.PI / 180; return [32 + 24 * Math.sin(r), 34 - 24 * Math.cos(r)]; };
    if (b - a < 1e-4) b = a + 1e-4;
    const [x1, y1] = pt(a), [x2, y2] = pt(b);
    return `M ${x1.toFixed(2)} ${y1.toFixed(2)} A 24 24 0 ${(b - a) * 270 > 180 ? 1 : 0} 1 ${x2.toFixed(2)} ${y2.toFixed(2)}`;
  }
  step(p, v) { const n = Math.max(1, (p.choices?.length || p.steps) - 1); return Math.round((v - p.min) / ((p.max - p.min) / n || 1)); }
  fmt(p, v) {
    if (p.choices && p.choices.length) return p.choices[this.step(p, v)] ?? '';
    const s = p.steps > 1 ? String(Math.round(v)) : Math.abs(v) >= 100 ? v.toFixed(0) : Math.abs(v) >= 10 ? v.toFixed(1) : v.toFixed(2);
    return (p.min < 0 && v > 0.0005 ? '+' : '') + s + (p.unit ? ' ' + p.unit : '');
  }

  // a value from here: shown at once, sent once a frame (a drag sends many), then the DAW has it
  set(p, v) {
    v = Math.min(p.max, Math.max(p.min, v));
    if (p.steps > 1) { const st = (p.max - p.min) / (p.steps - 1); v = p.min + Math.round((v - p.min) / st) * st; }
    const k = this.knobs.get(p.i); if (k) k.update(v);
    this.touched.set(p.i, performance.now());
    this.pending.set(p.i, v);
    if (!this.raf) this.raf = requestAnimationFrame(() => { this.raf = 0; for (const [i, x] of this.pending) window.vibe_param_set(i, x); this.pending.clear(); });
  }
  // the values now (the panel, a preset, the DAW's automation): the knobs follow – not the one in your hand
  async poll() {
    if (!this.open) return;
    let vals; try { vals = JSON.parse(await window.vibe_param_values()); } catch (e) { return; }
    if (vals.length !== this.params.length) { this.reload(); return; }
    const now = performance.now();
    for (const [i, k] of this.knobs) {
      if (i === this.held || i === this.heldY || now - (this.touched.get(i) || 0) < 400) continue;
      k.update(vals[i]);
    }
    this.pollCurves(false);
  }
  // the device's picture (RGB565), small
  frame(fb) {
    if (!this.open) return;
    const d = this.img.data;
    for (let i = 0, j = 0; i < fb.length; i++, j += 4) { const v = fb[i]; d[j] = (v >> 8) & 0xF8; d[j + 1] = (v >> 3) & 0xFC; d[j + 2] = (v << 3) & 0xF8; d[j + 3] = 255; }
    this.ctx.putImageData(this.img, 0, 0);
  }
}

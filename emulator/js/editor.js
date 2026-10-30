// editor.js – the Editor view of the VIBE FM VST: every parameter of the playing firmware at once.
const h = (tag, cls, parent, text) => { const e = document.createElement(tag); if (tag === 'button') e.type = 'button'; if (cls) e.className = cls; if (text != null) e.textContent = text; if (parent) parent.appendChild(e); return e; };
const ns = (tag, cls, parent) => { const e = document.createElementNS('http:\x2f\x2fwww.w3.org/2000/svg', tag); if (cls) e.setAttribute('class', cls); if (parent) parent.appendChild(e); return e; };
const clp = (v, a, b) => Math.max(a, Math.min(b, v));
const nrm = (p, v) => (v - p.min) / (p.max - p.min || 1);
const drag = (el, fn, up) => {
  let dn = false;
  el.onpointerdown = (e) => { el.setPointerCapture(e.pointerId); dn = true; e.preventDefault(); fn(e, 1); };
  el.onpointermove = (e) => { if (dn) fn(e, 0); };
  el.onpointerup = el.onpointercancel = () => { if (dn) { dn = false; up?.(); } };
};

export class Editor {
  constructor() {
    this.el = h('section', 'editor', document.body); this.el.hidden = true;
    const side = h('div', 'ed-side', this.el);
    this.canvas = h('canvas', 'ed-disp', side); this.canvas.width = 480; this.canvas.height = 320;
    this.ctx = this.canvas.getContext('2d'); this.img = this.ctx.createImageData(480, 320);
    this.buildHw(side);
    this.tabs = h('div', 'ed-tabs', side); this.note = h('p', 'ed-note', side); this.main = h('div', 'ed-main', this.el);
    this.params = []; this.knobs = new Map(); this.tab = null; this.pending = new Map();
    this.held = this.heldY = -1; this.touched = new Map(); this.timer = 0; this.ed = null;
    this.curves = new Map(); this.curveLast = new Map();
    this.ccMap = new Map(); this.paramCC = new Map(); this.syxWait = null; this.ccCnt = 0;
    this.midiIn = this.midiOut = null; this.initMidi();
  }
  get open() { return !this.el.hidden; }
  byId(id) { return this.params.find((x) => x.id === id); }
  prId() { return this.byId('preset') || this.byId('preset_a'); }
  toCC(p, v) { return clp(p.id === 'tempo' ? (v & 127) : Math.round(nrm(p, v) * (p.steps > 1 && p.steps <= 128 ? p.steps - 1 : 127)), 0, 127); }
  async initMidi() {
    try {
      const m = await navigator.requestMIDIAccess?.({ sysex: true }), re = /fm-?1|m-?vave|s1|ober|midi/i;
      if (!m) return;
      const sc = () => {
        for (const p of m.inputs.values()) if (re.test(p.name) || !this.midiIn) { this.midiIn = p; p.onmidimessage = (e) => this.onHwMidi(e.data); }
        for (const p of m.outputs.values()) if (re.test(p.name) || !this.midiOut) this.midiOut = p;
      };
      m.onstatechange = sc; sc();
    } catch (e) {}
  }
  sendHw(b) { try { this.midiOut?.send(b); } catch (e) {} }
  pack7(m) {
    const o = [240];
    for (let i = 0; i < m.length; i += 7) {
      let b = 0, n = Math.min(7, m.length - i);
      for (let j = 0; j < n; j++) b |= (m[i + j] & 128) >> (7 - j);
      o.push(b); for (let j = 0; j < n; j++) o.push(m[i + j] & 127);
    }
    o.push(247); return new Uint8Array(o);
  }
  unpack7(w) {
    let s = w.indexOf(240), e = w.indexOf(247, s + 1); if (s < 0 || e < 0) return null;
    const d = [];
    for (let i = s + 1; i < e; i += 8) for (let j = 1, m = w[i]; j < 8 && i + j < e; j++) d.push((w[i + j] & 127) | (((m >> (j - 1)) & 1) << 7));
    return d.length >= 7 && !d[0] && d[1] === 89 ? { cmd: d[2], body: new Uint8Array(d.slice(6, -1)) } : null;
  }
  makeSyx(cmd, b) {
    const n = b.length, m = new Uint8Array(7 + n); m[1] = 89; m[2] = cmd; m[3] = n & 255;
    let c = 0; for (let i = 0; i < n; i++) { m[6 + i] = b[i]; c = (c + b[i]) & 255; }
    m[6 + n] = c; return this.pack7(m);
  }
  async rdSlot(s) {
    try {
      const a = JSON.parse(await window.vibe_editor_curve(0, `R0:${s}`)), b = JSON.parse(await window.vibe_editor_curve(0, `R1:${s}`));
      return a.length < 61 || b.length < 61 ? null : Uint8Array.from([...a, ...b], (x) => Math.round(x * 255));
    } catch (e) { return null; }
  }
  async wrSlot(bytes) {
    if (!window.vibe_editor_curve || bytes.length < 82) return false;
    const b = new Uint8Array(122); b.set(bytes.subarray(0, Math.min(122, bytes.length)));
    for (let c = 0; c < 9; c++) {
      let x = ''; for (let i = 0; i < 14 && c * 14 + i < 122; i++) x += b[c * 14 + i].toString(16).padStart(2, '0');
      await window.vibe_editor_curve(0, `W${c}:${x}`);
    }
    await window.vibe_editor_curve(0, 'WC'); await this.reload(); return true;
  }
  onHwMidi(d) {
    if (!d?.length) return;
    if (d[0] === 240) {
      const u = this.unpack7(d);
      if (u?.cmd === 33 && u.body.length >= 82) {
        if (this.syxWait) { const r = this.syxWait; this.syxWait = null; r(u.body); }
        else this.wrSlot(u.body).then(() => { this.note.textContent = `Preset ${u.body[0] + 1} <- FM-1`; });
      }
      return;
    }
    const st = d[0] & 240, c1 = d[1] & 127, c2 = d[2] & 127;
    if (st === 176 && d.length >= 3 && this.ccMap.has(c1)) {
      const p = this.ccMap.get(c1);
      this.set(p, p.id === 'tempo' ? c2 : p.min + (c2 / Math.max(1, p.steps > 1 ? p.steps - 1 : 127)) * (p.max - p.min), true);
      this.ccCnt++;
    } else if (st === 192 && d.length >= 2) this.set(this.prId(), c1, true);
  }
  curSlot() { const pr = this.prId(); return pr ? Math.round(this.knobs.get(pr.i)?.v || 0) : 0; }
  async pullSlot(s, q = false) {
    await this.initMidi(); this.set(this.prId(), s, true); this.sendHw([192, s & 127]);
    const log = (t) => { if (!q) this.note.textContent = t; };
    const syx = await new Promise((res) => {
      this.syxWait = res; this.sendHw(this.makeSyx(32, new Uint8Array([s & 127])));
      setTimeout(() => { if (this.syxWait === res) { this.syxWait = null; res(null); } }, 180);
    });
    if (syx) { syx[0] = s & 127; syx[1] = 1; await this.wrSlot(syx); log(`Pulled ${s + 1} (SysEx)`); return true; }
    this.ccCnt = 0; this.sendHw([176, 119, 1]); await new Promise((r) => setTimeout(r, 200));
    if (this.ccCnt > 0) { window.vibe_midi?.(176, 119, 127); await this.reload(); log(`Pulled ${s + 1} (${this.ccCnt} CCs)`); return true; }
    log('No reply — check USB');
    return false;
  }
  async pullBank() {
    let ok = 0; for (let s = 64; s < 99; s++) { this.note.textContent = `Pull ${s + 1}/99...`; if (await this.pullSlot(s, true)) ok++; }
    this.note.textContent = ok ? `Pulled ${ok} User Presets!` : 'No reply.';
  }
  async pushSlot() {
    await this.initMidi(); const s = this.curSlot(), pkt = await this.rdSlot(s);
    this.sendHw([192, s & 127]);
    if (pkt) { pkt[0] = s & 127; pkt[1] = 1; this.sendHw(this.makeSyx(33, pkt)); }
    for (const [i, cc] of this.paramCC) { const p = this.params[i], k = this.knobs.get(i); if (p && k) this.sendHw([176, cc, this.toCC(p, k.v)]); }
    this.sendHw([176, 119, 127]); window.vibe_midi?.(176, 119, 127);
    this.note.textContent = `Saved ${s + 1} -> FM-1`;
  }
  async exportSyx(bank = false) {
    const ch = [], s0 = bank ? 64 : this.curSlot(), s1 = bank ? 99 : s0 + 1;
    for (let s = s0; s < s1; s++) { const p = await this.rdSlot(s); if (p) { p[0] = s & 127; p[1] = 1; ch.push(this.makeSyx(33, p)); } }
    if (!ch.length) return;
    const out = new Uint8Array(ch.reduce((a, b) => a + b.length, 0)); let o = 0; for (const c of ch) { out.set(c, o); o += c.length; }
    const a = h('a'); a.href = URL.createObjectURL(new Blob([out])); a.download = bank ? 'bank.syx' : `preset-${s0 + 1}.syx`; a.click();
  }
  importSyx() {
    const inp = h('input'); inp.type = 'file'; inp.accept = '.syx,.bin';
    inp.onchange = async () => {
      const f = inp.files?.[0]; if (!f) return;
      const b = new Uint8Array(await f.arrayBuffer()); let n = 0, i = 0, fc = 0;
      for (const x of b) if (x === 240) fc++;
      while (i < b.length) {
        let s = b.indexOf(240, i), e = s >= 0 ? b.indexOf(247, s + 1) : -1; if (s < 0 || e < 0) break;
        const u = this.unpack7(b.subarray(s, e + 1));
        if (u?.body.length >= 82) { if (!n && fc === 1) u.body[0] = this.curSlot(); await this.wrSlot(u.body); this.sendHw(this.makeSyx(33, u.body)); n++; }
        i = e + 1;
      }
      this.note.textContent = `Imported ${n} preset(s)`;
    };
    inp.click();
  }
  buildHw(side) {
    const hw = h('div', 'ed-hw', side), ev = (t, i, d, v) => window.vibe_ev?.(t, i, d, v, 0);
    const mkBtn = (row, l, id) => {
      const b = h('button', 'ed-hw-btn', row, l);
      drag(b, (_, d) => { if (d) { b.classList.add('on'); ev(3, id, 1, 0); } }, () => { b.classList.remove('on'); ev(3, id, 0, 0); });
    };
    const rEnc = h('div', 'ed-hw-row enc', hw);
    for (let i = 0; i < 4; i++) {
      const ek = h('div', 'ed-hw-enc', rEnc, 'K' + (i + 1)); let y0 = 0, acc = 0;
      drag(ek, (e, d) => { if (d) { y0 = e.clientY; acc = 0; } else { const st = ((y0 - e.clientY) / 8 | 0) - acc; if (st) { acc += st; ev(4, i, 0, st); } } });
      ek.onwheel = (e) => { e.preventDefault(); ev(4, i, 0, e.deltaY < 0 ? 1 : -1); };
    }
    mkBtn(rEnc, '◀', 18); mkBtn(rEnc, '▶', 20);
    const rMode = h('div', 'ed-hw-row', hw), rSys = h('div', 'ed-hw-row', hw), rSyx = h('div', 'ed-hw-row syx', hw);
    [['HOME', 17], ['EDIT', 15], ['ENV', 12], ['LFO', 13], ['FX', 10], ['ARP', 14]].forEach(([l, id]) => mkBtn(rMode, l, id));
    [['SEQ', 0], ['GLO', 1], ['SAVE', 2], ['SEL', 11], ['PLAY', 23], ['STOP', 22]].forEach(([l, id]) => mkBtn(rSys, l, id));
    [['← FM-1', () => this.pullSlot(this.curSlot())], ['← 65-99', () => this.pullBank()], ['→ FM-1', () => this.pushSlot()], ['↓ .SYX', (e) => this.exportSyx(e.shiftKey)], ['↑ .SYX', () => this.importSyx()]]
      .forEach(([l, fn]) => { h('button', 'ed-hw-btn', rSyx, l).onclick = fn; });
  }
  async show(on) {
    this.el.hidden = !on; clearInterval(this.timer); this.timer = 0;
    if (on) { await this.reload(); this.timer = setInterval(() => this.poll(), 120); }
  }
  async reload() {
    let list = []; try { list = JSON.parse(await window.vibe_params()); } catch (e) {}
    this.params = list; this.ed = null;
    if (window.vibe_editor) try { const d = JSON.parse(await window.vibe_editor()); if (d?.cards) this.ed = d; } catch (e) {}
    this.ccMap.clear(); this.paramCC.clear();
    for (const p of list) {
      const g = p.group || '', k = g.indexOf(' · '), m = /\[CC\s+(\d+)\]/i.exec(this.ed?.help?.[p.i] || '');
      p.t = k > 0 ? g.slice(0, k) : ''; p.c = (k > 0 ? g.slice(k + 3) : g) || 'PARAMETERS';
      if (m) { this.ccMap.set(+m[1], p); this.paramCC.set(p.i, +m[1]); }
    }
    const tabs = this.tabList = [...new Set(list.map((p) => p.t))];
    if (!tabs.includes(this.tab)) this.tab = tabs[0] ?? '';
    this.tabs.textContent = '';
    if (tabs.length > 1) for (const t of tabs) {
      const b = h('button', 'ed-tab' + (t === this.tab ? ' on' : ''), this.tabs, t || 'ALL');
      b.onclick = () => { this.tab = t; this.reloadView(); [...this.tabs.children].forEach((c) => c.classList.toggle('on', c === b)); };
    }
    this.note.textContent = `${list.length} parameters`;
    this.reloadView();
  }
  rank(card, first) {
    if (this.ed?.cards.length) { const k = this.ed.cards.findIndex((c) => c.card === card); return k >= 0 ? k : 100000 + first; }
    return 500 + first;
  }
  reloadView() {
    this.main.textContent = ''; this.knobs.clear(); this.main.scrollTop = 0;
    const ti = Math.max(0, this.tabList.indexOf(this.tab)), own = this.ed && (this.ed.tabs[this.tab] || this.ed.accent);
    this.el.style.setProperty('--ed-acc', own ? '#' + (own >>> 0).toString(16).padStart(6, '0') : ['#FF60BE', '#40E0D0', '#FFC94A', '#A98BFF'][ti % 4]);
    this.main.classList.toggle('ed-grid12', !!this.ed?.cards.length);
    this.curves.clear();
    if (this.tab) {
      const head = h('div', 'ed-head', this.main);
      h('h2', '', head, this.tab);
      h('span', '', head, this.params.filter((p) => p.t === this.tab).length + ' parameters');
    }
    const cards = new Map(), order = new Map();
    this.params.forEach((p, n) => { if (p.t === this.tab && !order.has(p.c)) order.set(p.c, n); });
    for (const c of [...order.keys()].sort((a, b) => this.rank(a, order.get(a)) - this.rank(b, order.get(b)))) {
      const card = h('div', 'ed-card', this.main); h('h3', '', card, (this.tab ? this.tab + ' · ' : '') + c);
      const d = this.ed?.cards.find((x) => x.card === c);
      card.style.gridColumn = `span ${d?.width >= 1 && d.width <= 12 ? d.width : 4}`;
      if (d?.note) h('p', 'ed-cnote', card, d.note);
      if (this.ed?.curves?.includes(c)) {
        const cv = h('canvas', 'ed-curve', card); cv.width = 240; cv.height = 44;
        if (c === 'OSC DRAW') {
          cv.style.cursor = 'crosshair';
          drag(cv, (e) => {
            const r = cv.getBoundingClientRect(), sw = this.byId('draw_sw');
            if (sw && !this.knobs.get(sw.i)?.v) this.set(sw, 1);
            this.set(this.byId('draw_step'), clp((((e.clientX - r.left) / r.width) * 16 | 0) + 1, 1, 16));
            this.set(this.byId('draw_value'), clp(Math.round((1 - (e.clientY - r.top) / r.height) * 200 - 100), -100, 100));
            this.pollCurves(true);
          });
        }
        this.curves.set(c, cv); this.curveLast.delete(c);
      }
      cards.set(c, h('div', 'ed-ctrls', card));
    }
    let skip = -1;
    for (const p of this.params) {
      if (p.t !== this.tab || p.i === skip) continue;
      const c = p.c, ctl = this.ed?.controls ? this.ed.controls[p.i] | 0 : 0;
      if (ctl === 5) {
        const q = this.params[p.i + 1];
        if (q && q.c === c && q.t === this.tab) { const [kx, ky] = this.xyPad(p, q, cards.get(c)); kx.update(p.v); ky.update(q.v); this.knobs.set(p.i, kx); this.knobs.set(q.i, ky); skip = q.i; continue; }
      }
      const k = this.control(p, cards.get(c), ctl); k.update(p.v); this.knobs.set(p.i, k);
    }
    this.pollCurves(true);
  }
  async pollCurves(force) {
    if (this.cBusy || !this.ed?.curves || !this.curves.size || !window.vibe_editor_curve) return;
    this.cBusy = 1; let more = 0;
    const acc = getComputedStyle(this.el).getPropertyValue('--ed-acc').trim() || '#FF60BE';
    for (const [c, cv] of this.curves) {
      const i = this.ed.curves.indexOf(c); if (i < 0) continue;
      const key = this.params.filter((p) => p.t === this.tab && p.c === c).map((p) => (this.knobs.get(p.i) || {}).v).join(',');
      const last = this.curveLast.get(c);
      if (!force && last?.key === key && !last.n && last.pts) continue;
      let pts; try { pts = JSON.parse(await window.vibe_editor_curve(i, this.tab)); } catch (e) { pts = []; }
      const n = !pts.length || last?.key !== key ? 2 : Math.max(0, (last?.n || 1) - 1);
      if (n) more = 1;
      if (!pts.length) { this.curveLast.set(c, { key, pts: null, n }); break; }
      const js = JSON.stringify(pts);
      this.curveLast.set(c, { key, pts: js, n });
      if (last?.pts !== js) {
        const g = cv.getContext('2d'), w = cv.width, hh = cv.height;
        g.clearRect(0, 0, w, hh); g.beginPath();
        pts.forEach((y, k) => { const x = k / (pts.length - 1) * (w - 8) + 4, yy = hh - 5 - y * (hh - 10); if (k) g.lineTo(x, yy); else g.moveTo(x, yy); });
        g.strokeStyle = acc; g.lineWidth = 2; g.lineJoin = 'round'; g.stroke();
        g.lineTo(w - 4, hh - 3); g.lineTo(4, hh - 3); g.closePath(); g.fillStyle = acc + '26'; g.fill();
      }
      if (n) break;
    }
    this.cBusy = 0;
    if (more && this.open) setTimeout(() => this.pollCurves(false), 16);
  }
  xyPad(px, py, parent) {
    const box = h('div', 'ed-ctl ed-ctl-xy', parent), pad = h('div', 'ed-xy', box), dot = h('div', 'ed-dot', pad);
    h('span', 'ed-lab', box, `${this.short(px)} × ${this.short(py)}`); const out = h('span', 'ed-val', box);
    const help = this.ed?.help && (this.ed.help[px.i] || this.ed.help[py.i]); if (help) box.title = help;
    const kx = { p: px, v: px.v }, ky = { p: py, v: py.v };
    const show = () => { dot.style.left = (nrm(px, kx.v) * 100) + '%'; dot.style.top = ((1 - nrm(py, ky.v)) * 100) + '%'; out.textContent = `${this.fmt(px, kx.v)} · ${this.fmt(py, ky.v)}`; };
    kx.update = (v) => { kx.v = v; show(); }; ky.update = (v) => { ky.v = v; show(); };
    drag(pad, (e) => {
      this.held = px.i; this.heldY = py.i; const r = pad.getBoundingClientRect();
      this.set(px, px.min + clp((e.clientX - r.left) / r.width, 0, 1) * (px.max - px.min));
      this.set(py, py.min + clp(1 - (e.clientY - r.top) / r.height, 0, 1) * (py.max - py.min));
    }, () => { this.held = this.heldY = -1; });
    pad.ondblclick = () => { this.set(px, px.def); this.set(py, py.def); };
    return [kx, ky];
  }
  short(p) {
    const s = this.tab && p.name.startsWith(this.tab + ' ') ? p.name.slice(this.tab.length + 1) : p.name, c = p.c + ' ';
    return s.startsWith(c) ? s.slice(c.length) : s;
  }
  control(p, parent, ctl = 0) {
    const box = h('div', 'ed-ctl', parent), short = this.short(p);
    if (this.ed?.help?.[p.i]) box.title = this.ed.help[p.i];
    if (ctl === 2 && (p.steps === 2 || p.choices?.length === 2)) {
      const b = h('button', 'ed-tog', box, short);
      b.onclick = () => this.set(p, this.step(p, this.knobs.get(p.i).v) ? p.min : p.max);
      return { p, v: p.v, update: (v) => { this.knobs.get(p.i) && (this.knobs.get(p.i).v = v); b.classList.toggle('on', this.step(p, v) > 0); b.textContent = short + ' · ' + this.fmt(p, v); } };
    }
    if (ctl === 1 && !p.choices?.length) {
      box.classList.add('ed-ctl-sl');
      const row = h('div', 'ed-slrow', box); h('span', 'ed-lab', row, short); const out = h('span', 'ed-val', row);
      const r = h('input', 'ed-slider', box); r.type = 'range'; r.min = 0; r.max = 1000; r.step = 1;
      r.oninput = () => this.set(p, p.min + (+r.value / 1000) * (p.max - p.min));
      r.ondblclick = () => this.set(p, p.def);
      const k = { p, v: p.v, update: (v) => { k.v = v; if (document.activeElement !== r) r.value = Math.round(nrm(p, v) * 1000); out.textContent = this.fmt(p, v); } };
      return k;
    }
    if (p.choices?.length || ctl === 3) {
      h('span', 'ed-lab', box, short);
      if (p.choices?.length <= 6 && ctl !== 3) {
        const row = h('div', 'ed-seg', box), btns = p.choices.map((c, k) => { const b = h('button', '', row, c); b.onclick = () => this.set(p, p.min + k * (p.max - p.min) / Math.max(1, p.choices.length - 1)); return b; });
        const kObj = { p, v: p.v, update: (v) => { kObj.v = v; const k = this.step(p, v); btns.forEach((b, n) => b.classList.toggle('on', n === k)); } };
        return kObj;
      }
      const nav = h('div', 'ed-nav-row', box), bPrev = h('button', 'ed-nav-btn', nav, '◀'), sel = h('select', 'ed-sel', nav), bNext = h('button', 'ed-nav-btn', nav, '▶');
      const total = Math.max(p.steps || 0, p.choices?.length || 0);
      for (let k = 0; k < total; k++) h('option', '', sel, p.choices?.[k] || `${String(k + 1).padStart(2, '0')}: [INIT]`).value = k;
      const chg = (idx) => { const c = clp(idx, 0, total - 1); sel.selectedIndex = c; this.set(p, p.min + (total > 1 ? c * (p.max - p.min) / (total - 1) : 0)); };
      bPrev.onclick = () => chg(sel.selectedIndex - 1);
      bNext.onclick = () => chg(sel.selectedIndex + 1);
      sel.onchange = () => chg(sel.selectedIndex);
      const kObj = { p, v: p.v, update: (v) => { kObj.v = v; const c = clp(this.step(p, v), 0, total - 1); if (p.choices?.[c] && sel.options[c]) sel.options[c].textContent = p.choices[c]; sel.selectedIndex = c; } };
      return kObj;
    }
    const svg = ns('svg', 'ed-knob', box); svg.setAttribute('viewBox', '0 0 64 64');
    const track = ns('path', 'trk', svg), val = ns('path', 'val', svg), dot = ns('line', 'ptr', svg);
    track.setAttribute('d', this.arcPath(0, 1));
    h('span', 'ed-lab', box, short); const out = h('span', 'ed-val', box);
    const k = { p, v: p.v, update: (v) => {
      k.v = v; const n = nrm(p, v), z = p.min < 0 && p.max > 0 ? nrm(p, 0) : ctl === 4 ? 0.5 : 0;
      val.setAttribute('d', this.arcPath(Math.min(z, n), Math.max(z, n)));
      const a = (-135 + 270 * n) * Math.PI / 180, sa = Math.sin(a), ca = Math.cos(a);
      dot.setAttribute('x1', 32 + 12 * sa); dot.setAttribute('y1', 34 - 12 * ca);
      dot.setAttribute('x2', 32 + 22 * sa); dot.setAttribute('y2', 34 - 22 * ca);
      out.textContent = this.fmt(p, v);
    } };
    let y0 = 0, v0 = 0;
    drag(svg, (e, d) => { if (d) { y0 = e.clientY; v0 = k.v; this.held = p.i; } else this.set(p, v0 + (y0 - e.clientY) / (e.shiftKey ? 900 : 180) * (p.max - p.min)); }, () => { this.held = -1; });
    svg.onwheel = (e) => { e.preventDefault(); const st = p.steps > 1 ? (p.max - p.min) / (p.steps - 1) : (p.max - p.min) / 100; this.set(p, k.v - Math.sign(e.deltaY) * st * (e.shiftKey ? 1 : (p.steps > 32 ? 2 : 1))); };
    svg.ondblclick = () => this.set(p, p.def);
    return k;
  }
  arcPath(a, b) {
    const pt = (n) => { const r = (-135 + 270 * n) * Math.PI / 180; return [32 + 24 * Math.sin(r), 34 - 24 * Math.cos(r)]; };
    if (b - a < 1e-4) b = a + 1e-4;
    const [x1, y1] = pt(a), [x2, y2] = pt(b);
    return `M ${x1.toFixed(2)} ${y1.toFixed(2)} A 24 24 0 ${(b - a) * 270 > 180 ? 1 : 0} 1 ${x2.toFixed(2)} ${y2.toFixed(2)}`;
  }
  step(p, v) { const n = Math.max(1, Math.max(p.steps || 0, p.choices?.length || 0) - 1); return Math.round((v - p.min) / ((p.max - p.min) / n || 1)); }
  fmt(p, v) {
    if (p.choices?.length) { const i = this.step(p, v); return p.choices[i] ?? `${String(i + 1).padStart(2, '0')}: [INIT]`; }
    const s = p.steps > 1 ? String(Math.round(v)) : Math.abs(v) >= 100 ? v.toFixed(0) : Math.abs(v) >= 10 ? v.toFixed(1) : v.toFixed(2);
    return (p.min < 0 && v > 0.0005 ? '+' : '') + s + (p.unit ? ' ' + p.unit : '');
  }
  set(p, v, fromHw = false) {
    if (!p) return;
    v = clp(v, p.min, p.max);
    if (p.steps > 1) { const st = (p.max - p.min) / (p.steps - 1); v = p.min + Math.round((v - p.min) / st) * st; }
    this.knobs.get(p.i)?.update(v);
    this.touched.set(p.i, performance.now()); this.pending.set(p.i, v);
    if (!fromHw && this.midiOut) {
      if (p.id.startsWith('preset')) this.sendHw([192, clp(Math.round(v), 0, 127)]);
      else if (this.paramCC.has(p.i)) this.sendHw([176, this.paramCC.get(p.i), this.toCC(p, v)]);
    }
    if (!this.raf) this.raf = requestAnimationFrame(() => { this.raf = 0; for (const [i, x] of this.pending) window.vibe_param_set(i, x); this.pending.clear(); this.pollCurves(false); });
  }
  async poll() {
    if (!this.open) return;
    let vals; try { vals = JSON.parse(await window.vibe_param_values()); } catch (e) { return; }
    if (vals.length !== this.params.length) { this.reload(); return; }
    const now = performance.now();
    for (const [i, k] of this.knobs) if (i !== this.held && i !== this.heldY && now - (this.touched.get(i) || 0) >= 400) k.update(vals[i]);
    this.pollCurves(false);
  }
  frame(fb) {
    if (!this.open) return;
    const d = this.img.data;
    for (let i = 0, j = 0; i < fb.length; i++, j += 4) { const v = fb[i]; d[j] = (v >> 8) & 248; d[j + 1] = (v >> 3) & 252; d[j + 2] = (v << 3) & 248; d[j + 3] = 255; }
    this.ctx.putImageData(this.img, 0, 0);
  }
}

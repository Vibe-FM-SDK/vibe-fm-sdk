// panel.js – the VIBE FM Mk1 front panel in the browser. Pure "hardware": it turns pointer/touch/keyboard input
// into SDK events (onEvent) and shows what the firmware tells it (display framebuffer, LEDs). No music logic here.
// Layout = the 3D model's numbers (1600 x 860 px, 4 px = 1 mm).

export const EV = { PAD: 1, KEY: 2, BUTTON: 3, ENCODER: 4, MIDI: 5, PEDAL: 6, CLOCK: 7, TRANSPORT: 8, TOUCH: 16 };
export const BTN = { F1: 0, F2: 1, F3: 2, F4: 3, F5: 4, F6: 5, SHIFT: 6, OCT_DOWN: 7, OCT_UP: 8, ENTER: 9, K1: 10, K2: 11, K3: 12, K4: 13, K5: 14, K6: 15,
  RETURN: 16, HOME: 17, LEFT: 18, MENU: 19, RIGHT: 20, REC: 21, STOP: 22, PLAY: 23, DATA_PUSH: 24, COUNT: 25 };
export const ENC = { E1: 0, E2: 1, E3: 2, E4: 3, A: 4, B: 5, DATA: 6, COUNT: 7 };

const L = {
  disp: { x: 108, y: 36, w: 360, h: 270 }, soft: { y: 314, w: 50, h: 28 }, knobs: { x0: 548, dx: 116, y: 100 }, jog: { x: 1070, y: 120 },
  vol: { x: 64, y: 96 }, word: { x: 506, y: 272 }, pads: { x: 1200, y: 40, size: 82, pitch: 92 }, btnY: 360,
  play: { x: 40, y: 420, w: 426, h: 380 }, fx: [{ x: 168, y: 490, label: 'A' }, { x: 338, y: 490, label: 'B' }],
  transport: { x: 59, w: 120, gap: 14, y1: 598, h1: 52, y: 668, h: 70 }, keys: { x: 490, y: 430, w: 1070, h: 430 },
};
// the box view (setBox): 1600 x 620, the display 1.8 x its full-view size
export const BOX_H = 620;
const BOX = {
  vol: [86, 96], ret: [61, 178, 50, 40], home: [61, 230, 50, 40],
  well: [150, 36, 660, 446], soft: (i) => [160 + i * 108, 494, 96, 34],
  knob: (i) => [900 + i * 116, 96], jog: [1440, 112],
  row1: (i) => [850 + i * 100, 232, 84, 44], row2: (i) => [850 + i * 116, 294, 84, 44],
  play: [840, 370, 720, 200], ab: (i) => [920 + i * 150, 478],
  nav: (i) => [1150 + i * 134, 390, 120, 52], tr: (i) => [1150 + i * 134, 456, 120, 70],
  word: [150, 556],
};
const softX = (i) => L.disp.x + 12 + i * 57.2;

// 31.10.2026: the display touched (the window only – the VIBE itself has no touch). Every native firmware draws the VIBE FM
// layout (firmware/obervave/o3_draw.h, docs UI standard): under the page the soft keys (y 282-320, six fields of 80 px) –
// a click presses F1-F6; above them four knob columns (x 10-470, 115 px each) – dragged up / down or scrolled, encoder 1-4
// turns, as the knob above the display does. el: what takes the pointer, canvasOf(): the 480 x 320 picture in it, on():
// only for a native firmware (a converted FM-1 one draws its own way) and not while the library is open
// the firmware's 480 x 320 picture: sharp pixels once it has at least that many real pixels (a Retina Mac, the Rack view,
// the big display) – soft when it is drawn smaller (nearest-neighbour would drop rows of a small font: ZVAMPLER's, 31.10.)
export function crisp(canvas) {
  const set = () => { const w = canvas.getBoundingClientRect().width * (window.devicePixelRatio || 1); canvas.style.imageRendering = w >= 479 ? 'pixelated' : 'auto'; };
  if (window.ResizeObserver) new ResizeObserver(set).observe(canvas);
  window.addEventListener('resize', set); set();
}
export function displayTouch(el, canvasOf, emit, on) {
  let drag = null;
  const at = (e) => {
    const r = canvasOf().getBoundingClientRect();
    return { x: (e.clientX - r.left) / r.width * 480, y: (e.clientY - r.top) / r.height * 320, inside: e.clientX >= r.left && e.clientX <= r.right && e.clientY >= r.top && e.clientY <= r.bottom };
  };
  const zone = (p) => (!p.inside ? null : p.y >= 282 ? { f: Math.min(5, Math.max(0, Math.floor(p.x / 80))) }
    : p.y >= 40 && p.x >= 10 && p.x < 470 ? { c: Math.min(3, Math.floor((p.x - 10) / 115)) } : null);
  // (SDK 18) a click anywhere else on the display – a firmware's own tabs at the top (ZVAMPLER) – goes to the firmware as
  // VIBE_EV_TOUCH: id 0 down / 1 move / 2 up, value = y * 512 + x (display pixels; VIBE_TOUCH_X / _Y in vibe.h); on a knob
  // column dragging turns the knob, a plain click (no turn) is a touch as well, down and up on release
  const touch = (p, ph) => emit(EV.TOUCH, ph, ph < 2 ? 1 : 0, Math.round(clamp(p.y, 0, 319)) * 512 + Math.round(clamp(p.x, 0, 479)));
  el.addEventListener('pointerdown', (e) => {
    const p = on() ? at(e) : null; if (!p || !p.inside) return;
    const z = zone(p);
    e.preventDefault(); e.stopPropagation(); try { el.setPointerCapture(e.pointerId); } catch (x) { /* a pointer that is gone */ }
    if (!z) { drag = { t: 1 }; touch(p, 0); }
    else if (z.f !== undefined) { drag = { f: z.f }; emit(EV.BUTTON, BTN.F1 + z.f, 1, 0.9); }
    else drag = { c: z.c, last: e.clientY, acc: 0, p, moved: false };
  });
  el.addEventListener('pointermove', (e) => {
    if (drag && drag.c !== undefined) {
      drag.acc += (drag.last - e.clientY) / 5; drag.last = e.clientY;
      if (Math.abs(drag.acc) >= 0.25) { emit(EV.ENCODER, drag.c, 0, drag.acc); drag.acc = 0; drag.moved = true; }
      return;
    }
    if (drag && drag.t) { touch(at(e), 1); return; }
    if (!drag) { const p = on() ? at(e) : null, z = p && p.inside ? zone(p) : null; el.style.cursor = !p || !p.inside ? '' : !z || z.f !== undefined ? 'pointer' : 'ns-resize'; }
  });
  const end = (e) => {
    if (!drag) return;
    if (drag.f !== undefined) emit(EV.BUTTON, BTN.F1 + drag.f, 0, 0);
    if (drag.t) touch(at(e), 2);
    if (drag.c !== undefined && !drag.moved) { touch(drag.p, 0); touch(drag.p, 2); }   // (a click on a knob column, not dragged: the firmware's – a track, a list entry)
    drag = null; e.stopPropagation();
  };
  el.addEventListener('pointerup', end); el.addEventListener('pointercancel', end);
  el.addEventListener('click', (e) => { if (on() && at(e).inside) e.stopPropagation(); });   // (the big display: a touch is not "close")
  el.addEventListener('wheel', (e) => {
    const z = on() ? zone(at(e)) : null; if (!z || z.c === undefined) return;
    e.preventDefault(); emit(EV.ENCODER, z.c, 0, -Math.sign(e.deltaY));
  }, { passive: false });
}
const btnX = (i) => (i < 4 ? 40 + i * 100 : 480 + (i - 4) * 120);
const trX = (i) => L.transport.x + i * (L.transport.w + L.transport.gap);
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const px = (n) => n + 'px';
const h = (tag, cls, parent, style) => { const e = document.createElement(tag); if (cls) e.className = cls; if (style) Object.assign(e.style, style); if (parent) parent.appendChild(e); return e; };
const cap = (el, e) => { try { el.setPointerCapture(e.pointerId); } catch (err) { /* synthetic or released pointer */ } };
const hex = (n) => '#' + (n >>> 0).toString(16).padStart(6, '0').slice(-6);

const ICON = {
  RETURN: '<svg viewBox="0 0 24 24"><path d="M18 5v6a3 3 0 0 1-3 3H7" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round"/><path d="M4 14l5-4.5v9z" fill="currentColor"/></svg>',
  HOME: '<svg viewBox="0 0 24 24"><path d="M12 3.5l8.5 8h-2.6v8.5H6.1v-8.5H3.5z" fill="currentColor"/><rect x="10.4" y="14.5" width="3.2" height="5.5" fill="#34363C"/></svg>',
  LEFT: '<svg viewBox="0 0 24 24"><path d="M16 5v14L5 12z" fill="currentColor"/></svg>',
  RIGHT: '<svg viewBox="0 0 24 24"><path d="M8 5v14l11-7z" fill="currentColor"/></svg>',
  REC: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="6.5" fill="currentColor"/></svg>',
  STOP: '<svg viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" fill="currentColor"/></svg>',
  PLAY: '<svg viewBox="0 0 24 24"><path d="M7 4.5v15L19.5 12z" fill="currentColor"/></svg>',
};

export class Panel {
  constructor(dev, onEvent, onVolume) {
    this.dev = dev; this.emit = onEvent; this.onVolume = onVolume; this.volume = 0.8;
    this.buttons = []; this.encoders = []; this.pads = []; this.keys = [];
    this.build();
  }

  build() {
    const dev = this.dev, plate = h('div', 'plate', dev);
    const print = (t, x, y, cls) => { const e = h('div', 'print ' + (cls || ''), plate, { left: px(x), top: px(y) }); e.textContent = t; };
    ['1', '2', '3', '4'].forEach((t, i) => print(t, L.knobs.x0 + i * L.knobs.dx, 168, 'knum'));
    print('DATA', L.jog.x, 210, 'cap'); print('VOLUME', L.vol.x, L.vol.y + 42, 'cap small');
    this.wordmark = h('div', 'wordmark', plate, { left: px(L.word.x), top: px(L.word.y - 44) });
    this.setBrand('Mk1');
    this.playpanel = h('div', 'playpanel', plate, { left: px(L.play.x), top: px(L.play.y), width: px(L.play.w), height: px(L.play.h) });
    this.playpanel.innerHTML = '<span>PLAY</span>';
    h('div', 'keybed', plate, { left: px(L.keys.x), top: px(L.keys.y), width: px(L.keys.w), height: px(L.keys.h) });
    print('ENCODER A/B · TRANSPORT', L.play.x, 812, 'cap small left');

    // display: the firmware's 480 x 320 framebuffer, plus a host menu layer (firmware library)
    const well = this.well = h('div', 'well', dev, { left: px(L.disp.x), top: px(L.disp.y), width: px(L.disp.w), height: px(L.disp.h) });
    this.canvas = h('canvas', 'screen', well); this.canvas.width = 480; this.canvas.height = 320; crisp(this.canvas);
    this.ctx = this.canvas.getContext('2d'); this.img = this.ctx.createImageData(480, 320);
    this.menu = h('div', 'dispmenu', well);
    h('div', 'glass', well);
    // the display takes clicks (displayTouch) for a firmware of the VIBE FM layout: one with sound parameters (host.js
    // touchOk) or one that names its soft keys (labelTouch, showLabels – DJ, ZVAMPLER: no parameters, the layout all the same)
    this.touchOk = false; this.labelTouch = false; this.fm1 = false;
    // (1.11., a bug report: a converted FM-1 firmware labels F1-F6 as well – SEQ, GLO, SAVE … –, so its picture took the
    //  VIBE layout's zones: a click low in it pressed SAVE, a drag turned KNOB 1-4. It draws its own way: no clicks)
    this.touchable = () => !this.fm1 && (this.touchOk || this.labelTouch);
    displayTouch(well, () => this.canvas, (...a) => this.emit(...a), () => this.touchable() && !this.menu.classList.contains('on'));

    // buttons
    const button = (id, x, y, w, hh, label, cls) => {
      const b = h('button', 'btn led ' + (cls || ''), dev, { left: px(x), top: px(y), width: px(w), height: px(hh) });
      b.type = 'button'; b.innerHTML = label ?? '';
      // hold keys (the FM-1 ports use them as layers: hold + turn): a double click keeps the key held, the next click lets go
      // (off for a firmware whose firmware.json says "latch": false: it holds its keys itself, OberVave with F5 HOLD)
      const latchable = (id >= BTN.K1 && id <= BTN.K6) || id === BTN.F1 || id === BTN.F2 || id === BTN.HOME || id === BTN.SHIFT;
      let lastTap = 0, latchNext = false, skipUp = false;
      const down = (e) => {
        e.preventDefault(); cap(b, e);
        if (b.classList.contains('latched')) { b.classList.remove('latched', 'down'); skipUp = true; this.emit(EV.BUTTON, id, 0, 0); return; }
        if (b.classList.contains('down')) return;
        const now = performance.now(); latchNext = latchable && this.latch !== false && now - lastTap < 350; lastTap = latchNext ? 0 : now;
        b.classList.add('down'); this.emit(EV.BUTTON, id, 1, 0);
      };
      const up = () => {
        if (skipUp) { skipUp = false; return; }
        if (latchNext) { latchNext = false; b.classList.add('latched'); return; }
        if (b.classList.contains('latched') || !b.classList.contains('down')) return;
        b.classList.remove('down'); this.emit(EV.BUTTON, id, 0, 0);
      };
      b.addEventListener('pointerdown', down); b.addEventListener('pointerup', up); b.addEventListener('pointercancel', up); b.addEventListener('lostpointercapture', up);
      b.addEventListener('keydown', (e) => { if ((e.key === 'Enter' || e.key === ' ') && !e.repeat) { e.preventDefault(); e.stopPropagation(); b.classList.add('down'); this.emit(EV.BUTTON, id, 1, 0); } });
      b.addEventListener('keyup', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); e.stopPropagation(); b.classList.remove('down'); this.emit(EV.BUTTON, id, 0, 0); } });
      this.buttons[id] = b; return b;
    };
    const lab = (b, name) => b.setAttribute('aria-label', name);
    lab(button(BTN.RETURN, 40, 178, 50, 40, ICON.RETURN, 'icon'), 'Back');
    lab(button(BTN.HOME, 40, 230, 50, 40, ICON.HOME, 'icon'), 'HOME');
    ['SHIFT', 'OCT −', 'OCT +', 'ENTER'].forEach((n, i) => lab(button(BTN.SHIFT + i, btnX(i), L.btnY, 84, 44, n), n));
    for (let i = 0; i < 6; i++) lab(button(BTN.K1 + i, btnX(i + 4), L.btnY, 84, 44, '', 'free'), 'Key ' + (i + 1));
    for (let i = 0; i < 6; i++) lab(button(BTN.F1 + i, softX(i), L.soft.y, L.soft.w, L.soft.h, 'F' + (i + 1), 'soft'), 'F' + (i + 1));
    [['LEFT', BTN.LEFT], ['MENU', BTN.MENU], ['RIGHT', BTN.RIGHT]].forEach(([n, id], i) => lab(button(id, trX(i), L.transport.y1, L.transport.w, L.transport.h1, ICON[n] || n, ICON[n] ? 'icon' : ''), n));
    [['REC', BTN.REC], ['STOP', BTN.STOP], ['PLAY', BTN.PLAY]].forEach(([n, id], i) => lab(button(id, trX(i), L.transport.y, L.transport.w, L.transport.h, ICON[n], 'icon ' + n.toLowerCase()), n));

    // encoders (endless: they send detents). No LED rings (dropped 15.10.2026): the firmware shows values on the display.
    const knob = (name, cx, cy, r, onDetents) => {
      const size = r * 2 + 12;   // a little grab margin around the cap
      const wrap = h('div', 'knobwrap', dev, { left: px(cx - size / 2), top: px(cy - size / 2), width: px(size), height: px(size) });
      const k = h('div', 'knob', wrap, { width: px(r * 2), height: px(r * 2) }); h('div', 'cap', k); h('div', 'ind', k);
      wrap.tabIndex = 0; wrap.setAttribute('role', 'slider'); wrap.setAttribute('aria-label', name);
      const o = { wrap, k, angle: -135 };
      let last = null, acc = 0;
      const turn = (det) => { o.angle += det * 7.5; k.style.setProperty('--a', o.angle + 'deg'); onDetents(det); };
      wrap.addEventListener('pointerdown', (e) => { e.preventDefault(); cap(wrap, e); last = e.clientY; acc = 0; wrap.classList.add('grab'); });
      wrap.addEventListener('pointermove', (e) => { if (last === null) return; acc += (last - e.clientY) / 5; last = e.clientY; if (Math.abs(acc) >= 0.25) { turn(acc); acc = 0; } });
      const end = () => { last = null; wrap.classList.remove('grab'); };
      wrap.addEventListener('pointerup', end); wrap.addEventListener('pointercancel', end);
      wrap.addEventListener('wheel', (e) => { e.preventDefault(); turn(-Math.sign(e.deltaY)); }, { passive: false });
      wrap.addEventListener('keydown', (e) => { const d = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 }[e.key]; if (d) { e.preventDefault(); e.stopPropagation(); turn(d); } });
      return o;
    };
    for (let i = 0; i < 4; i++) this.encoders[i] = knob('Encoder ' + (i + 1), L.knobs.x0 + i * L.knobs.dx, L.knobs.y, 34, (d) => this.emit(EV.ENCODER, i, 0, d));
    L.fx.forEach((f, i) => { print(f.label, f.x + 64, f.y - 8, 'knum'); this.encoders[ENC.A + i] = knob('Encoder ' + f.label, f.x, f.y, 34, (d) => this.emit(EV.ENCODER, ENC.A + i, 0, d)); });
    // VOLUME is a real potentiometer on the device: handled by the runtime (hardware), not the firmware
    this.vol = knob('Volume', L.vol.x, L.vol.y, 29, (d) => this.setVolume(this.volume + d / 60));
    this.vol.k.style.setProperty('--a', (-135 + this.volume * 270) + 'deg');

    // DATA wheel: turned by circling around it, 15 degrees per detent; a tap without turning = DATA push
    {
      const r = 68, wrap = this.jogWrap = h('div', 'knobwrap jogwrap', dev, { left: px(L.jog.x - r), top: px(L.jog.y - r), width: px(r * 2), height: px(r * 2) });
      const k = h('div', 'knob jog', wrap, { width: px(r * 2), height: px(r * 2) }); h('div', 'cap', k); h('div', 'dimple', k);
      wrap.tabIndex = 0; wrap.setAttribute('role', 'slider'); wrap.setAttribute('aria-label', 'DATA');
      let lastA = null, angle = 0, acc = 0, moved = 0, lastY = null;   // (mouse: dragged up / down like the knobs; touch: circled)
      const ang = (e) => { const b = wrap.getBoundingClientRect(); return Math.atan2(e.clientY - (b.top + b.height / 2), e.clientX - (b.left + b.width / 2)) * 180 / Math.PI; };
      const turn = (deg) => { angle += deg; acc += deg; moved += Math.abs(deg); k.style.transform = `rotate(${angle}deg)`; while (Math.abs(acc) >= 15) { const s = Math.sign(acc); acc -= s * 15; this.emit(EV.ENCODER, ENC.DATA, 0, s); } };
      wrap.addEventListener('pointerdown', (e) => { e.preventDefault(); cap(wrap, e); lastA = ang(e); lastY = e.pointerType === 'mouse' ? e.clientY : null; moved = 0; });
      wrap.addEventListener('pointermove', (e) => {
        if (lastA === null) return;
        if (lastY !== null) { const dy = lastY - e.clientY; lastY = e.clientY; turn(dy * 2.5); return; }   // (6 px a detent)
        const a = ang(e); let d = a - lastA; if (d > 180) d -= 360; if (d < -180) d += 360; lastA = a; turn(d);
      });
      wrap.addEventListener('pointerup', () => { if (lastA !== null && moved < 4) { this.emit(EV.BUTTON, BTN.DATA_PUSH, 1, 0); this.emit(EV.BUTTON, BTN.DATA_PUSH, 0, 0); } lastA = null; });
      wrap.addEventListener('pointercancel', () => (lastA = null));
      wrap.addEventListener('wheel', (e) => { e.preventDefault(); turn(-Math.sign(e.deltaY) * 15); }, { passive: false });
      wrap.addEventListener('keydown', (e) => { const d = { ArrowUp: 15, ArrowRight: 15, ArrowDown: -15, ArrowLeft: -15 }[e.key]; if (d) { e.preventDefault(); e.stopPropagation(); turn(d); } });
    }

    // pads: velocity from touch pressure, or from where you hit (centre = loud)
    for (let i = 0; i < 16; i++) {
      const p = h('div', 'pad', dev, { left: px(L.pads.x + (i % 4) * L.pads.pitch), top: px(L.pads.y + Math.floor(i / 4) * L.pads.pitch), width: px(L.pads.size), height: px(L.pads.size) });
      p.innerHTML = `<span class="pn">${i + 1}</span>`; p.setAttribute('role', 'button'); p.setAttribute('aria-label', 'Pad ' + (i + 1));
      p.addEventListener('pointerdown', (e) => {
        e.preventDefault(); cap(p, e);
        const b = p.getBoundingClientRect(), dx = (e.clientX - b.left) / b.width - 0.5, dy = (e.clientY - b.top) / b.height - 0.5;
        const vel = e.pressure && e.pressure !== 0.5 && e.pointerType !== 'mouse' ? clamp(e.pressure * 1.3, 0.15, 1) : clamp(1 - Math.hypot(dx, dy) * 1.25, 0.25, 1);
        p.classList.add('down'); this.emit(EV.PAD, i, 1, vel);
      });
      const up = () => { if (!p.classList.contains('down')) return; p.classList.remove('down'); this.emit(EV.PAD, i, 0, 0); };
      p.addEventListener('pointerup', up); p.addEventListener('pointercancel', up);
      this.pads.push(p);
    }

    // 25 keys, glissando with one finger
    const kInX = L.keys.x + 8, kP = (L.keys.w - 16) / 15, wTop = L.keys.y + 22, OFF = [0, 2, 4, 5, 7, 9, 11];
    for (let w = 0; w < 15; w++) { const k = h('div', 'key white', dev, { left: px(kInX + w * kP + 1.5), top: px(wTop), width: px(kP - 3), height: px(L.keys.y + L.keys.h - wTop - 6) }); k.dataset.id = Math.floor(w / 7) * 12 + OFF[w % 7]; this.keys.push(k); }
    for (let bk = 0; bk < 14; bk++) { if ([0, 1, 3, 4, 5].indexOf(bk % 7) < 0) continue; const k = h('div', 'key black', dev, { left: px(kInX + (bk + 1) * kP - 20), top: px(wTop), width: '40px', height: '255px' }); k.dataset.id = Math.floor(bk / 7) * 12 + OFF[bk % 7] + 1; this.keys.push(k); }
    const held = new Map();
    const keyAt = (e) => { const el = document.elementFromPoint(e.clientX, e.clientY); return el && el.classList && el.classList.contains('key') ? el : null; };
    const vel = (el, e) => { const b = el.getBoundingClientRect(); return clamp(0.35 + ((e.clientY - b.top) / b.height) * 0.75, 0.2, 1); };
    const on = (el, v) => { el.classList.add('down'); this.emit(EV.KEY, +el.dataset.id, 1, v); };
    const off = (el) => { el.classList.remove('down'); this.emit(EV.KEY, +el.dataset.id, 0, 0); };
    dev.addEventListener('pointerdown', (e) => { const el = e.target.closest && e.target.closest('.key'); if (!el) return; e.preventDefault(); cap(dev, e); held.set(e.pointerId, el); on(el, vel(el, e)); });
    dev.addEventListener('pointermove', (e) => { if (!held.has(e.pointerId)) return; const el = keyAt(e), cur = held.get(e.pointerId); if (el && el !== cur) { off(cur); held.set(e.pointerId, el); on(el, vel(el, e)); } });
    const end = (e) => { const cur = held.get(e.pointerId); if (cur) { off(cur); held.delete(e.pointerId); } };
    dev.addEventListener('pointerup', end); dev.addEventListener('pointercancel', end);
    this.keyEl = (id) => this.keys.find((k) => +k.dataset.id === id);
  }

  // the device's print: "VIBE FM Mk1" – in the plugin "VIBE FM VST"; the claim under it
  setBrand(model) { this.wordmark.innerHTML = `VIBE <b>FM</b> <span>${model}</span><i>MAKE FIRMWARE <em>NOT WAR</em></i>`; }

  // the box view (VIBE Box): the device without keys and pads – in a DAW the notes come from its keyboard – and with a
  // bigger display: the display left, the encoders, keys and the play section right. Every control stays where its
  // event is; only the places change (1600 x 620 instead of 1600 x 860).
  setBox(on) {
    const dev = this.dev, st = (el) => el && el.style;
    if (!this._full) {                          // the full view's places, to go back to
      this._full = new Map();
      const keep = (el) => { if (el) this._full.set(el, { left: el.style.left, top: el.style.top, width: el.style.width, height: el.style.height }); };
      [this.well, this.wordmark, this.playpanel, this.jogWrap, this.vol.wrap, ...this.encoders.map((o) => o && o.wrap), ...this.buttons].forEach(keep);
      this.boxPrints = h('div', 'boxprints', dev);
      const pr = (t, x, y, cls) => { const e = h('div', 'print ' + (cls || ''), this.boxPrints, { left: px(x), top: px(y) }); e.textContent = t; };
      ['1', '2', '3', '4'].forEach((t, i) => pr(t, BOX.knob(i)[0], 152, 'knum'));
      pr('DATA', BOX.jog[0], 196, 'cap'); pr('VOLUME', BOX.vol[0], BOX.vol[1] + 38, 'cap small');
      ['A', 'B'].forEach((t, i) => pr(t, BOX.ab(i)[0] + 56, BOX.ab(i)[1] - 46, 'knum'));
    }
    this.box = on; dev.classList.toggle('boxview', on);
    if (!on) { for (const [el, p] of this._full) Object.assign(el.style, p); return; }
    const place = (el, x, y, w, hh) => { if (!el) return; el.style.left = px(x); el.style.top = px(y); if (w != null) el.style.width = px(w); if (hh != null) el.style.height = px(hh); };
    const knobAt = (o, cx, cy) => { if (!o) return; const sz = parseFloat(o.wrap.style.width); place(o.wrap, cx - sz / 2, cy - sz / 2); };
    place(this.well, ...BOX.well);
    for (let i = 0; i < 6; i++) place(this.buttons[BTN.F1 + i], ...BOX.soft(i));
    place(this.buttons[BTN.RETURN], ...BOX.ret); place(this.buttons[BTN.HOME], ...BOX.home);
    for (let i = 0; i < 4; i++) place(this.buttons[BTN.SHIFT + i], ...BOX.row1(i));
    for (let i = 0; i < 6; i++) place(this.buttons[BTN.K1 + i], ...BOX.row2(i));
    [BTN.LEFT, BTN.MENU, BTN.RIGHT].forEach((id, i) => place(this.buttons[id], ...BOX.nav(i)));
    [BTN.REC, BTN.STOP, BTN.PLAY].forEach((id, i) => place(this.buttons[id], ...BOX.tr(i)));
    for (let i = 0; i < 4; i++) knobAt(this.encoders[i], ...BOX.knob(i));
    knobAt(this.encoders[ENC.A], ...BOX.ab(0)); knobAt(this.encoders[ENC.B], ...BOX.ab(1));
    knobAt(this.vol, ...BOX.vol); place(this.jogWrap, BOX.jog[0] - 68, BOX.jog[1] - 68);
    place(this.playpanel, ...BOX.play); place(this.wordmark, ...BOX.word);
  }

  // RGB565 framebuffer -> canvas
  showFrame(fb) {
    const d = this.img.data;
    for (let i = 0, j = 0; i < fb.length; i++, j += 4) { const v = fb[i]; d[j] = (v >> 8) & 0xF8; d[j + 1] = (v >> 3) & 0xFC; d[j + 2] = (v << 3) & 0xF8; d[j + 3] = 255; }
    this.ctx.putImageData(this.img, 0, 0);
    if (this.big) this.bigCtx.drawImage(this.canvas, 0, 0);   // the display-only view: the same frame, pixel for pixel
  }
  // "Display": only the screen, at the largest whole-number zoom that fits the window, without smoothing – sharp
  // (the device view scales the 480 x 320 twice by odd factors, which blurs it)
  showBig(on) {
    if (on && !this.bigEl) {
      this.bigEl = h('div', 'bigdisp', document.body); this.bigCanvas = h('canvas', '', this.bigEl); this.bigCanvas.width = 480; this.bigCanvas.height = 320;
      this.bigCtx = this.bigCanvas.getContext('2d'); this.bigCtx.imageSmoothingEnabled = false;
      this.bigLabels = h('div', 'biglabels', this.bigEl); this.labelKey = null;
      this.bigEl.addEventListener('click', () => this.onBig && this.onBig(false));
      displayTouch(this.bigCanvas, () => this.bigCanvas, (...a) => this.emit(...a), () => this.touchable());
    }
    this.big = on; if (this.bigEl) this.bigEl.classList.toggle('on', on);
    this.labelKey = null;                         /* (the captions line under the big display) */
    if (on) {
      this.fitBig();
      // the picture and the captions there now – not only with the next frame (27.10.: the plugin with the host's audio
      // stopped sends none while nothing changes: the display view stayed black)
      this.bigCtx.drawImage(this.canvas, 0, 0);
      if (this.lastLabels) this.showLabels(this.lastLabels);
    }
  }
  fitBig() {
    if (!this.big) return;
    const dpr = window.devicePixelRatio || 1, vw = innerWidth - 24, vh = innerHeight - 110;
    const z = Math.max(1, Math.floor(Math.min(vw * dpr / 480, vh * dpr / 320))) / dpr;   // whole device pixels per framebuffer pixel
    this.bigCanvas.style.width = (480 * z) + 'px'; this.bigCanvas.style.height = (320 * z) + 'px';
  }

  // LED table written by the firmware (layout = struct vibe_leds in sdk/src/vibe_sdk.c)
  // the firmware's captions for the buttons (vibe_label): on the soft keys and the six free keys, and under the big display
  showLabels(buf) {
    if (!buf) return;
    this.lastLabels = buf;
    const u = new Uint8Array(buf), txt = (id) => { let s = ''; for (let i = 0; i < 8 && u[id * 8 + i]; i++) s += String.fromCharCode(u[id * 8 + i]); return s; };
    let key = ''; const soft = [], keys = [];
    for (let i = 0; i < 6; i++) { soft.push(txt(BTN.F1 + i)); keys.push(txt(BTN.K1 + i)); }
    this.labelTouch = soft.some(Boolean);             // (it names its soft keys: the VIBE FM layout – the display takes clicks)
    key = soft.join('|') + '/' + keys.join('|'); if (key === this.labelKey) return; this.labelKey = key;
    // the keys of the device carry no print (the display explains them, as on the hardware): the captions go to
    // the tooltips and the line under the big display only
    soft.forEach((t, i) => { const b = this.buttons[BTN.F1 + i]; if (b) b.title = t || ''; });
    keys.forEach((t, i) => { const b = this.buttons[BTN.K1 + i]; if (b) b.title = t || ''; });
    // in the plugin (no hardware to stay true to) the captions are printed on the keys: the coloured six and F1–F6
    if (this.printLabels) {
      const put = (b, t, dflt) => { if (b) b.innerHTML = `<span class="lab">${(t || dflt).replace(/[&<>]/g, '')}</span>`; };
      soft.forEach((t, i) => put(this.buttons[BTN.F1 + i], t, 'F' + (i + 1)));
      keys.forEach((t, i) => put(this.buttons[BTN.K1 + i], t, ''));
    }
    if (this.bigLabels) this.bigLabels.innerHTML = soft.map((t, i) => `<span><b>F${i + 1}</b> ${t || '–'}</span>`).join('') + '<i></i>' + keys.map((t, i) => `<span><b>${i + 1}</b> ${t || '–'}</span>`).join('');
  }
  // a controller (USB-MIDI) moved something: show it on the panel as if the mouse had – the knob turns, the pad / button lights
  mirror(type, id, down, value) {
    if (type === EV.ENCODER) { const o = this.encoders[id]; if (o && o.k) { o.angle += value * 7.5; o.k.style.setProperty('--a', o.angle + 'deg'); } }
    else if (type === EV.PAD) { const p = this.pads[id]; if (p) p.classList.toggle('down', !!down); }
    else if (type === EV.BUTTON) { const b = this.buttons[id]; if (b) b.classList.toggle('down', !!down); }
    else if (type === EV.KEY) { const k = this.keyEl(id); if (k) k.classList.toggle('down', !!down); }
  }
  setVolume(v) { this.volume = clamp(v, 0, 1); this.onVolume(this.volume); this.vol.k.style.setProperty('--a', (-135 + this.volume * 270) + 'deg'); }
  setLatch(on) {   // the firmware decides (firmware.json "latch"); a key latched before is let go
    this.latch = on;
    for (const [id, b] of Object.entries(this.buttons)) if (b.classList.contains('latched')) { b.classList.remove('latched', 'down'); this.emit(EV.BUTTON, +id, 0, 0); }
  }
  showLeds(buf) {
    const u = new Uint32Array(buf), f = new Float32Array(buf);
    const P = 16, B = BTN.COUNT;
    for (let i = 0; i < P; i++) this.light(this.pads[i], u[i], f[P + i]);
    for (let b = 0; b < B; b++) if (this.buttons[b]) this.light(this.buttons[b], u[2 * P + b], f[2 * P + B + b], true);
  }
  light(el, rgb, lvl, isButton) {
    const key = rgb + ':' + Math.round(lvl * 40);
    if (el._led === key) return; el._led = key;
    el.style.setProperty(isButton ? '--led' : '--c', hex(rgb));
    el.style.setProperty(isButton ? '--lvl' : '--l', clamp(lvl, 0, 1).toFixed(3));
    if (isButton) el.classList.toggle('bright', lvl > 0.5);
  }
}

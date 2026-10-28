// rear.js – the VIBE FM Mk1 rear panel in the browser (SDK 2): IN 1/IN 2, PEDAL, CV OUT, GATE OUT, CV IN, CLK IN.
// Like panel.js it is pure "hardware": it shows what the firmware sets (input mode, CV/Gate out) and turns what the
// user does (pedal, CV IN knob, clock) into SDK events. The jacks sit in the order of the 3D model's rear side.
import { EV } from './panel.js';

const MODE = ['LINE', 'MIC', 'HI-Z'];
const h = (tag, cls, parent, html) => { const e = document.createElement(tag); if (cls) e.className = cls; if (html !== undefined) e.innerHTML = html; if (parent) parent.appendChild(e); return e; };
const jack = (parent, label, cls) => { const g = h('div', 'jack ' + (cls || ''), parent); h('span', 'hole', g); h('b', '', g, label); return g; };

export class Rear {
  // send(type, id, down, value) = SDK event, cvIn(volts) = CV IN, onInput() = switch the audio inputs on
  constructor(el, send, cvIn, onInput) {
    this.send = send; this.cvIn = cvIn; this.pedalType = 'switch'; this.clockSrc = 'off'; this.lastClock = 0; this.midiTicks = 0;
    this.sdk = 0;
    h('span', 'rlabel', el, 'REAR PANEL');

    // IN 1 / IN 2: balanced 6.3 mm TRS (no XLR), next to OUT L/R; mode set by the firmware (LINE, MIC, HI-Z), optionally linked as stereo
    const ins = h('div', 'rgroup', el);
    this.inJ = [0, 1].map((i) => { const j = jack(ins, 'IN ' + (i + 1)); const m = h('small', 'mode', j, '–'); const lv = h('i', 'lvl', j); return { j, m, lv }; });
    this.link = h('small', 'link', ins, 'STEREO');
    this.inBtn = h('button', 'rbtn', ins, 'Inputs on'); this.inBtn.type = 'button';
    this.inBtn.title = 'Microphone or audio interface: channel 1 = IN 1, channel 2 = IN 2';
    this.inBtn.addEventListener('click', () => onInput());

    // PEDAL: 6.3 mm TRS, a foot switch or an expression pedal (the device detects which)
    const ped = h('div', 'rgroup', el);
    jack(ped, 'PEDAL');
    this.pedSel = h('select', '', ped, '<option value="switch">Footswitch</option><option value="expr">Expression</option>');
    this.pedSel.setAttribute('aria-label', 'Pedal type');
    this.pedBtn = h('button', 'rbtn', ped, 'press'); this.pedBtn.type = 'button'; this.pedBtn.title = 'Footswitch (key P)';
    this.pedExpr = h('input', '', ped); Object.assign(this.pedExpr, { type: 'range', min: 0, max: 1, step: 0.01, value: 0 });
    this.pedExpr.setAttribute('aria-label', 'Expression pedal');
    const down = (e) => { e.preventDefault(); this.pedal(1); }, up = () => this.pedal(0);
    this.pedBtn.addEventListener('pointerdown', down); this.pedBtn.addEventListener('pointerup', up); this.pedBtn.addEventListener('pointerleave', up);
    this.pedExpr.addEventListener('input', () => this.expr(+this.pedExpr.value));
    this.pedSel.addEventListener('change', () => this.setPedal(this.pedSel.value));
    this.setPedal('switch');

    // CV/Gate: CV OUT, GATE OUT (shown), CV IN (a knob for the voltage), CLK IN (tap, MIDI clock or off)
    const cv = h('div', 'rgroup', el);
    const co = jack(cv, 'CV OUT'); this.cvOut = h('small', 'val', co, '0.00 V');
    const go = jack(cv, 'GATE OUT'); this.gateLed = h('i', 'led', go);
    const ci = jack(cv, 'CV IN'); this.cvVal = h('small', 'val', ci, '0.00 V');
    this.cvKnob = h('input', '', cv); Object.assign(this.cvKnob, { type: 'range', min: -5, max: 5, step: 0.01, value: 0 });
    this.cvKnob.setAttribute('aria-label', 'Voltage at CV IN');
    this.cvKnob.addEventListener('input', () => { const v = +this.cvKnob.value; this.cvVal.textContent = v.toFixed(2) + ' V'; this.cvIn(v); });
    this.cvKnob.addEventListener('dblclick', () => { this.cvKnob.value = 0; this.cvKnob.dispatchEvent(new Event('input')); });
    const ck = jack(cv, 'CLK IN'); this.clkLed = h('i', 'led', ck);
    this.clkSel = h('select', '', cv, '<option value="off">off</option><option value="tap">Tap</option><option value="midi">MIDI clock (16ths)</option>');
    this.clkSel.setAttribute('aria-label', 'Source for CLK IN');
    this.tap = h('button', 'rbtn', cv, 'Pulse'); this.tap.type = 'button';
    this.tap.addEventListener('pointerdown', (e) => { e.preventDefault(); this.clock(); });
    this.clkSel.addEventListener('change', () => { this.clockSrc = this.clkSel.value; this.tap.hidden = this.clockSrc !== 'tap'; this.lastClock = 0; });
    this.tap.hidden = true;
  }

  setPedal(type) { this.pedalType = type; this.pedSel.value = type; this.pedBtn.hidden = type !== 'switch'; this.pedExpr.hidden = type !== 'expr'; }
  pedal(down) {
    if (this.pedalType !== 'switch' || this.pedDown === down) return;
    this.pedDown = down; this.pedBtn.classList.toggle('on', !!down); this.send(EV.PEDAL, 0, down, down ? 1 : 0);
  }
  expr(v) { if (this.pedalType !== 'expr') return; this.pedExpr.value = v; this.send(EV.PEDAL, 1, 0, v); }
  // USB-MIDI stand-ins: CC64 = foot switch, CC11 = expression, MIDI clock = CLK IN (one pulse per 16th = 6 ticks)
  midi(d) {
    const st = d[0] & 0xF0;
    if (d[0] === 0xF8) { if (this.clockSrc === 'midi' && this.midiTicks++ % 6 === 0) this.clock(); return true; }
    if (d[0] === 0xFA) { this.midiTicks = 0; return false; }
    if (st === 0xB0 && d[1] === 64) { this.setPedal('switch'); this.pedal(d[2] >= 64 ? 1 : 0); return true; }
    if (st === 0xB0 && d[1] === 11) { this.setPedal('expr'); this.expr(d[2] / 127); return true; }
    return false;
  }
  clock() {
    const now = performance.now(), dt = this.lastClock ? (now - this.lastClock) / 1000 : 0; this.lastClock = now;
    this.send(EV.CLOCK, 0, 1, dt > 4 ? 0 : dt);
    this.clkLed.classList.add('on'); clearTimeout(this.clkT); this.clkT = setTimeout(() => this.clkLed.classList.remove('on'), 60);
  }
  // what the firmware set (frame message): [cv_out, gate_out, cv_in, in_mode1, in_mode2, in_link]; null = SDK 1 firmware
  show(r) {
    if (!r) { this.inJ.forEach((x) => (x.m.textContent = 'MIC/INST')); this.link.classList.remove('on'); this.cvOut.textContent = '–'; this.gateLed.classList.remove('on'); return; }
    this.cvOut.textContent = r[0].toFixed(2) + ' V'; this.gateLed.classList.toggle('on', r[1] > 2.5);
    this.inJ.forEach((x, i) => (x.m.textContent = MODE[r[3 + i]] || 'LINE'));
    this.link.classList.toggle('on', r[5] > 0.5);
  }
  inputsOn(on) { this.inBtn.classList.toggle('on', on); this.inBtn.textContent = on ? 'Inputs on ✓' : 'Inputs on'; }
  levels(l1, l2) { this.inJ[0].lv.style.setProperty('--v', l1); this.inJ[1].lv.style.setProperty('--v', l2); }
}

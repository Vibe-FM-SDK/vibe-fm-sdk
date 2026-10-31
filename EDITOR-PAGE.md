# Your own Editor page (SDK 16)

**Give your firmware the interface you always wanted: your design, your controls, in plain HTML, CSS and JavaScript.**
It ships inside your firmware's `.zip`, and VIBE FM VST shows it in its Editor view. No plugin framework, no build step
for the page, no new plugin: the official VIBE FM VST 0.6.0 or newer runs it.

```
my-synth/
  my-synth.c
  firmware.json
  icon.png             ← your firmware's face (README: "icon.png")
  editor/              ← your page: index.html + whatever it needs, 2 MB at most
    index.html
    app.js
    style.css
```

**Contents:** [Two rules](#two-rules) · [Your first page in 10 minutes](#your-first-page-in-10-minutes) ·
[How it works](#how-it-works) · [`window.vibe`](#windowvibe) · [Recipes](#recipes) ·
[Bytes between page and firmware](#bytes-between-page-and-firmware) · [Hardware and files](#hardware-and-files) ·
[The sandbox](#the-sandbox) · [When something does not work](#when-something-does-not-work) ·
[With an AI assistant](#with-an-ai-assistant) · [Before you share it](#before-you-share-it)

## Two rules

1. **Your firmware stays fully playable without the page.** The VIBE FM hardware never shows it: there every control
   is reached with DATA + ENTER (FOCUS). In the plugin a **CUSTOM · STANDARD** switch is always there and brings back
   the standard cards. Your page is a second face, never the only one.
2. **Your page talks only to its own firmware.** It runs in a sandbox: no network, no files, no MIDI, nothing of the
   plugin. Everything goes through `window.vibe`. What needs more (a hardware dump, a file) the firmware does.

## Your first page in 10 minutes

**1. Start from the example.** `examples/example-synth/editor/` is a complete page (filter pad, sliders, keyboard, the
display) in about 100 lines. Copy it next to your firmware:

```sh
cp -r examples/example-synth/editor my-synth/editor
```

Or start from nothing with this `editor/index.html` – it works as it is:

```html
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <style>
    body { margin: 0; padding: 24px; background: var(--vibe-bg); color: var(--vibe-fg); font-family: var(--vibe-font); }
    .row { display: flex; gap: 16px; flex-wrap: wrap; }
    label { display: grid; gap: 6px; width: 160px; font-size: 13px; }
    input { accent-color: var(--vibe-accent); }
  </style>
</head>
<body>
  <h1>MY SYNTH</h1>
  <div class="row" id="controls"></div>
  <script src="vibe-editor.js"></script>   <!-- served by the plugin: gives you window.vibe -->
  <script>
    (async () => {
      await vibe.ready;                                         // the firmware's parameters are there
      const box = document.getElementById('controls');
      const sliders = vibe.params.map((p) => {                  // one slider per parameter
        const l = document.createElement('label'), s = document.createElement('input'), v = document.createElement('span');
        s.type = 'range'; s.min = 0; s.max = 1000;
        s.oninput = () => vibe.set(p.i, p.min + s.value / 1000 * (p.max - p.min));
        l.append(p.name, s, v); box.append(l);
        return { p, s, v };
      });
      vibe.onValues((values) => {                               // the truth: DAW automation, the device, presets
        for (const { p, s, v } of sliders) {
          if (document.activeElement !== s) s.value = (values[p.i] - p.min) / (p.max - p.min) * 1000;
          v.textContent = values[p.i].toFixed(2) + (p.unit ? ' ' + p.unit : '');
        }
      });
    })();
  </script>
</body>
</html>
```

**2. Build.** `sh tools/build.sh my-synth` – it says `its Editor page: build/my-synth.editor/ (in the zip)`.

**3. Run it.** Drop `build/my-synth.zip` onto the VIBE FM VST window, choose **View ▾ → Editor**. Your page is there,
with **CUSTOM · STANDARD** above the little display on the left.

**4. Change, build, drop again.** That is the whole loop. Open the page's developer tools while you work (see
[When something does not work](#when-something-does-not-work)).

## How it works

```
 your page (sandboxed iframe)          VIBE FM VST                     your firmware (.wasm)
 ───────────────────────────           ───────────                     ─────────────────────
 vibe.set(i, v)            ──────────▶ the DAW records it ──────────▶  VIBE_EV_PARAM
 vibe.onValues(cb)         ◀────────── ~20 times a second ◀──────────  vibe_param_set, presets, automation
 vibe.send(bytes)          ──────────────────────────────────────────▶ VIBE_EV_EDITOR_DATA
 vibe.onData(cb)           ◀────────────────────────────────────────── vibe_editor_post(bytes, n)
 vibe.curve(card)          ◀────────────────────────────────────────── your vibe_editor_curve function
 vibe.note(n, vel)         ──────────────────────────────────────────▶ a note, as from the keys
 vibe.onFrame(cb)          ◀────────────────────────────────────────── vibe_on_draw (the display)
```

- **The firmware is the truth.** The page shows what `onValues` says and sends what the user does. Never keep your own
  copy of a value and trust it: the DAW's automation, the hardware's knobs and preset changes move parameters too.
- **Parameters are for knobs.** Everything a DAW should automate is a parameter (`vibe_params`), and the page uses
  `vibe.set`. Everything else – a wavetable, a pattern, a name, a list – goes as bytes.
- **The page can disappear any time** (STANDARD, another firmware, the window closed). Keep nothing important in it.

## `window.vibe`

Load it with `<script src="vibe-editor.js"></script>` before your own scripts. Everything works after `await vibe.ready`.

| | |
|---|---|
| `vibe.params` | `[{ i, id, name, min, max, def, unit, steps, choices, group }]` – your `vibe_params`, in order. `steps` > 0: n choices, `choices`: their names |
| `vibe.byId('cutoff')` | one parameter by its id (`vibe.byId('cutoff').i` is its index) |
| `vibe.values` | the current values, plain (`min..max`), indexed like `params` |
| `vibe.onValues(cb)` | `cb(values)` now and whenever a value changes |
| `vibe.set(i, value)` | set parameter `i` (plain value; snapped to its steps). The DAW records it |
| `vibe.curve(card, tab)` | `Promise<number[]>` – a card's curve (`vibe_editor_curve`), points 0..1 |
| `vibe.draw(card, tab, point, count, value)` | draw into a curve (`vibe_editor_curve_draw`) |
| `vibe.send(bytes)` | to the firmware: a `Uint8Array`, an array of numbers, or a string – up to 4096 bytes |
| `vibe.onData(cb)` | `cb(Uint8Array)` – what the firmware posted with `vibe_editor_post` |
| `vibe.note(n, vel)` | a note: MIDI number 0..127, velocity 0..1 (0 = note off) |
| `vibe.onFrame(cb)` | `cb(Uint16Array)` – the device display, 480 × 320, RGB565, about 30 times a second |
| `vibe.paint(canvas, fb)` | draws such a frame onto a 480 × 320 canvas |
| `vibe.theme` | `{ bg, fg, muted, line, accent, font }` – also CSS variables `--vibe-bg`, `--vibe-fg`, `--vibe-muted`, `--vibe-line`, `--vibe-accent`, `--vibe-font`. `accent` is your firmware's own (`vibe_editor_accent`) |

## Recipes

Each one is complete – paste it into `app.js` (inside `(async () => { await vibe.ready; … })()`), add the HTML element.

### A knob you drag

```js
// <div class="knob" data-id="cutoff"></div>   style: .knob { width: 64px; height: 64px; border-radius: 50%; background: conic-gradient(var(--vibe-accent) var(--a), #333 0); }
for (const el of document.querySelectorAll('.knob')) {
  const p = vibe.byId(el.dataset.id); let y0 = 0, v0 = 0;
  const norm = (v) => (v - p.min) / (p.max - p.min);
  el.onpointerdown = (e) => { el.setPointerCapture(e.pointerId); y0 = e.clientY; v0 = vibe.values[p.i]; };
  el.onpointermove = (e) => { if (!e.buttons) return;
    const fine = e.shiftKey ? 0.2 : 1;                          // Shift: fine
    vibe.set(p.i, v0 + (y0 - e.clientY) / 200 * fine * (p.max - p.min)); };
  el.ondblclick = () => vibe.set(p.i, p.def);                   // double-click: the default
  vibe.onValues((v) => el.style.setProperty('--a', norm(v[p.i]) * 360 + 'deg'));
}
```

### An XY pad for two parameters

```js
// <div id="pad" style="position:relative;height:200px;background:#1c1f24;touch-action:none"><i id="dot" style="position:absolute;width:16px;height:16px;margin:-8px;border-radius:50%;background:var(--vibe-accent)"></i></div>
const px = vibe.byId('cutoff'), py = vibe.byId('reso'), pad = document.getElementById('pad'), dot = document.getElementById('dot');
const at = (e) => { const r = pad.getBoundingClientRect();
  vibe.set(px.i, px.min + Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)) * (px.max - px.min));
  vibe.set(py.i, py.min + Math.min(1, Math.max(0, 1 - (e.clientY - r.top) / r.height)) * (py.max - py.min)); };
pad.onpointerdown = (e) => { pad.setPointerCapture(e.pointerId); at(e); };
pad.onpointermove = (e) => { if (e.buttons) at(e); };
vibe.onValues((v) => { dot.style.left = (v[px.i] - px.min) / (px.max - px.min) * 100 + '%';
                       dot.style.top = (1 - (v[py.i] - py.min) / (py.max - py.min)) * 100 + '%'; });
```

### A menu for a parameter with choices

```js
// <select id="wave"></select>
const p = vibe.byId('wave'), sel = document.getElementById('wave');
p.choices.forEach((name, k) => sel.add(new Option(name, k)));
const step = (v) => Math.round((v - p.min) / (p.max - p.min) * (p.choices.length - 1));
sel.onchange = () => vibe.set(p.i, p.min + sel.value / (p.choices.length - 1) * (p.max - p.min));
vibe.onValues((v) => { sel.value = step(v[p.i]); });
```

### Show a curve (an envelope, a filter)

Declare it in the firmware (`vibe_editor_curve("AMP ENV", fn)`), then draw it in the page – again whenever values change:

```js
// <canvas id="env" width="300" height="80"></canvas>
const cv = document.getElementById('env'), g = cv.getContext('2d');
async function drawEnv() {
  const pts = await vibe.curve('AMP ENV');                   // [] while the firmware has not made it yet
  if (!pts.length) return;
  g.clearRect(0, 0, cv.width, cv.height); g.beginPath();
  pts.forEach((y, k) => g.lineTo(k / (pts.length - 1) * cv.width, (1 - y) * cv.height));
  g.strokeStyle = vibe.theme.accent; g.lineWidth = 2; g.stroke();
}
let busy = false;
vibe.onValues(async () => { if (busy) return; busy = true; await drawEnv(); busy = false; });
```

### Draw a wavetable and send it

```js
// <canvas id="wt" width="512" height="128"></canvas> – 64 steps, 0..255 each
const wt = new Uint8Array(64).fill(128), cv = document.getElementById('wt'), g = cv.getContext('2d');
const paint = () => { g.clearRect(0, 0, 512, 128); g.fillStyle = vibe.theme.accent;
  wt.forEach((v, k) => g.fillRect(k * 8, 128 - v / 2, 7, v / 2)); };
cv.onpointermove = (e) => { if (!e.buttons) return; const r = cv.getBoundingClientRect();
  const k = Math.floor((e.clientX - r.left) / r.width * 64), v = Math.round((1 - (e.clientY - r.top) / r.height) * 255);
  if (k >= 0 && k < 64) { wt[k] = Math.max(0, Math.min(255, v)); paint(); } };
cv.onpointerup = () => vibe.send(new Uint8Array([1, ...wt]));   // command 1 = "here is the wavetable" (your format)
paint();
```

The firmware side is in [Bytes between page and firmware](#bytes-between-page-and-firmware).

### A keyboard

```js
// <div id="keys" style="display:flex;height:80px"></div>
const keys = document.getElementById('keys');
for (let n = 48; n < 72; n++) {
  const b = document.createElement('button'), black = [1, 3, 6, 8, 10].includes(n % 12);
  b.style.cssText = `flex:1;border:0;background:${black ? '#222' : '#eee'}`;
  b.onpointerdown = () => vibe.note(n, 0.8);
  b.onpointerup = b.onpointerleave = (e) => { if (e.type === 'pointerup' || e.buttons) vibe.note(n, 0); };
  keys.append(b);
}
```

### The device display

```js
// <canvas id="disp" width="480" height="320" style="width:240px;image-rendering:pixelated"></canvas>
const disp = document.getElementById('disp');
vibe.onFrame((fb) => vibe.paint(disp, fb));
```

### Fit the window

The Editor area changes size with the plugin window. Use CSS grid or flex with relative sizes, and test small (about
900 × 500) and large. A canvas: set its `width`/`height` from `getBoundingClientRect()` × `devicePixelRatio` in a
`ResizeObserver`, then draw again.

## Bytes between page and firmware

For everything that is not a parameter. Pick a tiny format of your own – usually a first byte that says what it is.

**Page:**

```js
vibe.send(new Uint8Array([1, ...wavetable]));             // 1 = the wavetable
vibe.send(new Uint8Array([2]));                           // 2 = "tell me the preset name"
vibe.onData((b) => {
  if (b[0] === 2) document.getElementById('name').textContent = new TextDecoder().decode(b.subarray(1));
});
```

**Firmware:**

```c
static float wave[64];
static char preset_name[16] = "INIT";

void vibe_on_event(const vibe_event *e)
{
  if (e->type == VIBE_EV_EDITOR_DATA) {             /* value = how many bytes, the bytes in vibe_editor_data() */
    const uint8_t *b = vibe_editor_data(); int n = (int)e->value;
    if (b[0] == 1 && n == 65) for (int i = 0; i < 64; i++) wave[i] = b[1 + i] / 127.5f - 1.0f;
    if (b[0] == 2) {
      uint8_t out[17] = { 2 }; int k = 0;
      while (k < 16 && preset_name[k]) { out[1 + k] = (uint8_t)preset_name[k]; k++; }
      vibe_editor_post(out, 1 + k);                 /* 0 = sent; -1 = the last one not taken yet */
    }
  }
}
```

- Up to **4096 bytes** a message, both ways.
- `vibe_editor_post` holds **one** message until the plugin has taken it (after the next audio block). If it returns -1,
  try again from your next `vibe_on_draw`. Never call it from `vibe_on_audio`.
- On the hardware and in an older plugin nothing arrives – your firmware must not wait for the page.

## Hardware and files

The page has no MIDI and no files – on purpose. The firmware has both (SDK 13–15):

| you want | page | firmware |
|---|---|---|
| pull a patch from a hardware synth | `vibe.send([10, slot])` | `vibe_midi_send_sysex(request, n)`; the answer comes as `VIBE_EV_SYSEX` (`vibe_sysex_in()`); then `vibe_editor_post` to tell the page |
| push a patch to it | `vibe.send([11, slot])` | build the SysEx, `vibe_midi_send_sysex` |
| export a file | `vibe.send([12])` | `vibe_media_create("my-patch.syx", path, n)`, `vibe_media_append`, `vibe_media_close` – it lands in `VIBE FM/<id>/` |
| import a file | – | the user drops it onto the window; the firmware takes it with `vibe_media_dropped` and reads it with `vibe_media_read` (SDK 13/14) |

## The sandbox

| works | does not work |
|---|---|
| your own JS, CSS and images (png, jpg, svg, webp, gif) from `editor/`, linked relatively (`img/logo.png`) | anything from the internet – `http(s)://` and `//` addresses are blocked |
| inline `<script>`, `<style>`, `style=""`, `onclick=""` | `fetch`, XHR, WebSocket – there is no network at all |
| canvas, SVG, WebGL, CSS animation, `requestAnimationFrame` | `eval`, `new Function` |
| fonts as `data:` URLs in your CSS (`@font-face { src: url(data:font/woff2;base64,…) }`), or `var(--vibe-font)` | font files by URL – the sandbox blocks them; embed them as `data:` |
| pointer, wheel and keyboard events | `localStorage`, `sessionStorage`, cookies – keep state in the firmware (`vibe.send`) |
| | other frames, forms, workers, `alert`, opening windows, WebAudio for sound (the firmware makes the sound) |

No framework is needed, but you may use one: put its file into `editor/` (no CDN). Keep the whole folder under 2 MB.

## When something does not work

**Open the page's developer tools:** in the plugin's Editor right-click your page → **Inspect** (Linux, Windows). On
macOS: Safari → Settings → Advanced → "Show features for web developers", then **Develop** → your computer → the plugin's
page. The console shows your errors and anything the sandbox blocked.

| what you see | why, and what to do |
|---|---|
| no CUSTOM · STANDARD switch | no page in the zip: `build.sh` must say "its Editor page" – the folder is `editor/` with `index.html`, next to `firmware.json`. Or the plugin is older than 0.6.0 |
| a blank page | an error before anything is drawn: look at the console. Most often `vibe` is used before `<script src="vibe-editor.js">` or before `await vibe.ready` |
| `vibe is not defined` | the `<script src="vibe-editor.js"></script>` line is missing, or comes after your script |
| "Refused to load …" / "violates the Content Security Policy" | something from outside, `eval`, or a font file by URL – see [The sandbox](#the-sandbox) |
| an image does not show | its path: relative to `index.html`, inside `editor/`, exact spelling (Linux and the hub care about upper/lower case) |
| `SecurityError` at `localStorage` | no storage in the sandbox – send it to the firmware |
| `vibe.set` does nothing | the wrong index – use `vibe.byId('id').i`; the value is plain (`min..max`), not 0..1 |
| a slider jumps back while dragging | update it in `onValues` only when it is not the element in your hand (`document.activeElement !== el`) |
| `vibe.curve` gives `[]` | the firmware has no curve for that card name, or has not made it yet – ask again on the next `onValues` |
| `onData` never fires | the firmware's `vibe_editor_post` returned -1 (retry from `vibe_on_draw`), or posts from `vibe_on_audio` |
| the old page after a new build | switch STANDARD and back to CUSTOM, or load the firmware again |
| `vibe-check` fails "Editor page" | it names the file and the address or the `eval` it found |

Run `node tools/vibe-check.mjs build/my-synth.wasm` – it checks the page (index.html there, 2 MB at most, no outside
addresses, no `eval`) along with everything else.

## With an AI assistant

`AGENTS.md` / `CLAUDE.md` tell an assistant the rules. A prompt that works:

> Build an Editor page for my firmware in `my-synth/editor/` following `EDITOR-PAGE.md`: plain HTML/CSS/JS, `window.vibe`
> only, no network, no external files. Controls for every parameter in `my-synth.c` grouped like its
> `vibe_param_groups`, a keyboard, and the display. Use the `--vibe-*` CSS variables. Then run `sh tools/build.sh my-synth`
> and `node tools/vibe-check.mjs build/my-synth.wasm` and fix what fails. Never change the plugin or the emulator.

## Before you share it

- [ ] `vibe-check` passes (with the page and the icon).
- [ ] The firmware plays fully on **STANDARD** and on the hardware layout (the emulator: `python3 tools/emulator.py`).
- [ ] Values follow the DAW: automate a parameter, watch your page move.
- [ ] Small window and large window both look right; text 11 px and up; enough contrast.
- [ ] No console errors.
- [ ] Your `.zip` contains `<id>.wasm`, `<id>.json`, `<id>.png` and `<id>.editor/`.

Your page is part of your firmware: same licence, same version (a new page is a new version). Share the zip wherever you
like; for **The Firmware Hub**, pages of curated firmwares are reviewed – playable without the page, no network, no
tracking, readable.

The browser emulator shows the standard Editor for now; test pages in VIBE FM VST. Protocol details: VIBE FM Protokoll 9.4.

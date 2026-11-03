# VIBE FM SDK – Early Access

**Build your own native VIBE FM synth with this – in plain C, as one file. It runs natively on the VIBE FM hardware,
and through VIBE FM VST as a plugin in your DAW.**

You write a synth (or a sampler, a sequencer, an effect…) in plain C against one small API. It compiles to a single
WebAssembly file. That one `.wasm` runs as a **VST3 / AU / CLAP plugin** in your DAW through VIBE FM VST, in the
**browser**, and on the coming **VIBE FM hardware**. No plugin framework, no GUI toolkit, no per-platform builds.

### What you build – and what you don't

- **You build the instrument:** its sound, its screen, its parameters and its Editor layout. All of it is in your C
  code and your `firmware.json`. The plugin's Editor view is drawn from what your firmware declares (`vibe_params`,
  `vibe_param_groups`, `vibe_editor`), so it is yours too, without a line of GUI code.
- **You don't build a plugin.** VIBE FM VST is the host that makes your `.wasm` a VST3 / AU / CLAP plugin. You don't
  build it, change it, patch it or ship it. It is closed software (all rights reserved). The emulator in `emulator/` is
  a host as well: free for developing, not to pass on or to ship changed (`emulator/LICENSE`). Its JavaScript is not an
  extension point; editing it only changes your own copy.
- **Missing something in the host?** A control type, a hardware bridge, a bug: open an **issue**. We build it into the
  host, and then every firmware has it.
- **Shipping:** your firmware is yours. Share your `.wasm` / `.zip` however you like, under the licence you choose:
  anyone drops it into the official VIBE FM VST and it plays, in the browser and on the hardware too, unchanged. Want
  it on The Firmware Hub as well? Send it to us.

> **Early Access – thank you for being one of the first.** This is a preview for a handful of developers. Please don't
> share the SDK or post it publicly yet. Everything here is still allowed to change. Tell us what is missing, unclear or
> broken: open an **issue** in this repository, or mail **zven@thefirmwarehub.com**

---

## What you need

| | |
|---|---|
| **Zig 0.13.0** | its clang compiles C to WebAssembly – nothing else to install. Download: <https://ziglang.org/download/#release-0.13.0>, unpack, put `zig` on your PATH (or set `ZIG=/path/to/zig`). |
| **VIBE FM VST 0.4** | the host you test in, free: <https://thefirmwarehub.com/convert> (macOS, Windows, Linux). 0.4 plays MIDI out, the Cue output and your Editor layout; an SDK-9 firmware also runs in 0.3, without them. |
| **The native firmwares** *(to look at)* | what this SDK makes: OberVave 3 (VOX coming soon) – <https://thefirmwarehub.com/native/>. OberVave 3 comes with VIBE FM VST, the Editor included. |
| a shell | macOS / Linux: the terminal. Windows: Git Bash or WSL (`tools/build.sh` is a plain `sh` script). |
| Node.js 18+ *(optional)* | for `tools/vibe-check.mjs` and `tools/wasmtest.mjs` (testing without a DAW). |
| Python 3 *(optional)* | for `tools/emulator.py` – the device in your browser (Chrome or Edge). |

## Five minutes to sound

```sh
sh tools/build.sh examples/example-synth
```

You get `build/example-synth.wasm` and `build/example-synth.zip`. Open VIBE FM VST in your DAW, **drop the `.zip`
onto the plugin window** – it installs and starts playing. Play notes from your MIDI keyboard or the DAW; the
synth's Cutoff, Reso, Attack, Release and Volume show up as automatable parameters – and in the plugin's
**View ▾ → Editor**, as cards FILTER, ENVELOPE and OUTPUT with real knobs.

Changed the code? Build again and drop the new zip – it replaces the old one.

## Your own firmware

> **Coming from the M-VAVE FM-1?** Your FM-1 firmware can become a native VIBE FM plugin: your engine stays, the FM-1
> layer goes. **[FROM-FM1.md](FROM-FM1.md)** walks you through it, step by step, the way OberVave 2 became OberVave 3.

```sh
cp -r examples/hello my-synth          # the folder's name is your firmware's id (letters, digits, - _ .)
mv my-synth/hello.c my-synth/my-synth.c
# edit my-synth/firmware.json: name, version, author, description, license
sh tools/build.sh my-synth
```

`examples/hello` (~60 lines) is the smallest useful firmware: a sine on the keys, pads that light up, an encoder that
bends. `examples/example-synth` (~150 lines) is a real instrument: 8 voices, saw through a resonant filter, envelope,
DAW parameters and the song position. `examples/example-seq` (~150 lines) is a 16-step sequencer that plays **out**
over MIDI, sample-exact, with its own clock or following the DAW, and lays out its own Editor card. Start from whichever
is closer to what you want.

**`firmware.json`** is your firmware's card – what the plugin shows in its library and the hub will show later:

| field | |
|---|---|
| `name` | shown everywhere – "MY SYNTH" (up to 40 characters) |
| `version` | yours, e.g. "1.0" – shown next to the name; change it when you ship a new build |
| `author` | your name or handle |
| `description` | one line: what it is |
| `license` | your firmware's licence – an SPDX id ("MIT", "GPL-3.0", "Apache-2.0") or "proprietary" |

The id is the folder's name; it must not change once people use your firmware (their saved files and DAW projects hang on it).

**`icon.png` – give your firmware a face.** Put a picture next to `firmware.json`: 16:9, e.g. 960 × 540, PNG, up to
1 MB. It becomes your firmware's tile in the VIBE FM main menu (a 3 × 2 grid of small tiles, 148 × 83 on the device)
and its card in The Firmware Hub. Make it read small: one strong motif, big shapes, little or no text. `tools/build.sh`
packs it into the zip; `vibe-check` reminds you when it is missing.

## How a firmware works

A firmware implements four functions. The host calls them one after the other from one thread – no locks needed.

```c
#include "vibe.h"
#include "vibe_dsp.h"   // oscillators, filters, envelopes, fast math
#include "vibe_gfx.h"   // drawing and text for the display

void vibe_on_init(float sample_rate)  { /* once: set up, declare parameters */ }
void vibe_on_event(const vibe_event *e) { /* a note, a knob, a button, the DAW … */ }
void vibe_on_audio(const float *in, float *out_l, float *out_r, int frames) { /* fill the block */ }
void vibe_on_draw(uint16_t *fb)       { /* paint the 480 × 320 display (RGB565) */ }
```

**Sample rate and block size are the host's, not yours.** `vibe_on_init` gets the DAW's rate (44.1, 48, 96 kHz …);
`frames` in `vibe_on_audio` varies from call to call (1 … 256 – the DAW's buffer, split at events). Derive everything
from `sample_rate`, never assume 128 or 48000. (`vibe-check` does not test other rates and block sizes yet; the plugin does.)

**The headers in `sdk/include/` are the reference** – every function is documented where it is declared:

| | |
|---|---|
| `vibe.h` | the contract: the controls (25 keys, 16 pads, encoders, buttons), events, LEDs and labels, notes from every source (`vibe_notes`), **DAW parameters** (`vibe_params`), the DAW's transport and song position (`vibe_transport`), audio inputs, CV/gate, storage (`vibe_store_*`), timing |
| `vibe_dsp.h` | `vd_sin`, `vd_mtof`, `vd_tanh`, band-limited `vd_saw` / `vd_square`, the state-variable filter `vd_svf_*`, ADSR `vd_env_*`, noise, random |
| `vibe_gfx.h` | `vg_clear`, `vg_rect`, `vg_round`, `vg_line`, `vg_circle`, `vg_text` (anti-aliased fonts built in), `vg_mix` |

**Notes.** Call `vibe_notes(VIBE_NOTES)` in `vibe_on_init` and every note – VIBE keys, pads, MIDI, the DAW – arrives as
one `VIBE_EV_NOTE` event. (See `example-synth`.)

**Parameters.** Declare a static `vibe_param` list with `vibe_params()` and the DAW gets them: automation, saved with
the project, moved back when you turn a knob (`vibe_param_set`). Named steps (`choices`) show in the DAW by name.
A parameter's `id` is for ever – the DAW's automation hangs on it. Rename the `name`, never the `id`.

**Storage.** `vibe_store_write` / `read` / `list` / `delete` are your files: sounds, patterns, samples. In VIBE FM VST
they live **inside the DAW project, per plugin instance** – two instances of your synth keep two sets of sounds, and they
travel with the song. In the browser they are in IndexedDB, on the hardware on the microSD. Same code everywhere.

**The Editor view.** VIBE FM VST 0.3 has a full-screen **Editor** (View ▾ → Editor): every parameter on one screen,
knobs, menus for named steps, all automatable. It is built from your groups – one card per group:

```c
static const char *const GROUPS[P_COUNT] = { "FILTER", "FILTER", "ENVELOPE", "ENVELOPE", "OUTPUT" };
vibe_params(PARAMS, P_COUNT);
vibe_param_groups(GROUPS);       // "A · FILTER" makes a tab "A" with a card "FILTER" – for multi-part instruments
```

Give your parameters groups and the Editor is yours (up to 256 parameters; OberVave 3 has 219). Without groups, a
firmware with only a handful of parameters gets the device view only.

**MIDI out** (SDK 9). `vibe_midi_send(status, d1, d2)` – or `vibe_midi_send_at(…, frame)` from inside
`vibe_on_audio` for sample-exact timing – goes to the plugin's MIDI output (route it to another track or instrument in
your DAW), to Web MIDI in the browser, and to USB-MIDI and MIDI OUT (DIN) on the hardware. `vibe_clock_out(1)` +
`vibe_tempo_set(bpm)` and the SDK sends 24-PPQ clock itself; while the DAW plays, it stays silent (one master).
Nothing is passed through by itself: forward incoming MIDI yourself if you want thru. See `example-seq`.

**Outputs** (SDK 9). `out_l` / `out_r` are yours – `vibe_outputs(VIBE_OUT_DUAL_MONO)` says they are two mono outs
(drums left, bass right). `vibe_outputs(VIBE_OUT_CUE)` gives you a **headphone signal of your own**: write into
`vibe_cue(0)` / `vibe_cue(1)` inside `vibe_on_audio` (a DJ's pre-listening, a click only for the player). The plugin
has a second output "Cue" for it, the hardware a headphone jack fed separately. `vibe_cue_host()` says whether this host
plays it – if not, fall back (e.g. master on L, cue on R).

**Your Editor** (SDK 9). Without it the Editor builds one card per group in its own order. With a few lines in
`vibe_on_init` it is yours: `vibe_editor_cards()` (order, width in a 12-column grid, a note), `vibe_editor_control()`
(slider, toggle, menu, bipolar knob, an XY pad from two parameters), `vibe_editor_accent()` / `vibe_editor_tab_color()`,
`vibe_editor_curve()` (a small graph per card from points you fill – an envelope, a filter, your pattern) and
`vibe_param_help()` (a tooltip). It is declarative on purpose: no pixels, no mouse – the same file has to look right
on the hardware, where the Editor does not exist.

## The rules (they keep the audio clean)

- **No heap, no libc.** The target is freestanding WebAssembly: use static memory and the SDK's helpers.
- **The audio callback is sacred:** no storing (`vibe_store_*`), no media (`vibe_media_*`), no logging, no long loops in
  `vibe_on_audio`. Do heavy setup (tables) in `vibe_on_init`.
- **Samples and songs are the host's (SDK 13):** read them with `vibe_media_*` from the sources the host offers (the
  VIBE's card and USB sticks, the emulator's Media folder, the plugin's media folder) into a buffer of your own, ahead of
  the audio – never bundle them into the firmware. -2 = not yet (wait for `VIBE_EV_MEDIA` READY), -1 = gone.
- **Don't run code at load time** other than through `vibe_on_init`.
- Leave `VIBE_PARALLEL_AUDIO` off for now (it has extra rules we have not published yet).

## New in SDK 10: FOCUS

Every control on the display reachable with **one knob and one button** (DATA + ENTER) – the standard for every
firmware, so the VIBE can be played from the smallest controller. About ten lines in your firmware: `AGENTS.md`
("SDK 10"), `sdk/include/vibe_gfx.h` (`vf_*`). Nothing new for a host: it runs in every VIBE FM VST.

## New in SDK 11: the mixer, and controller profiles

A firmware with tracks reports its mixer as parameters with fixed ids (`mix_1_level`, `mix_1_mute`, …, `mix_select`).
Then any mixer controller and the DAW work its tracks, with no controller code in the firmware. Controllers themselves
are described by small JSON **profiles** in `controllers/`, which the host applies. Make one in the emulator (Setup, then
Profile ↓) and send it to us. Both are explained in **`CONTROLLERS.md`**.

## New in SDK 12 and 13: a stream from the host, areas – and media

**SDK 12:** `vibe_stream` (a stream the host plays into the firmware – Spotify Connect on the VIBE), `vibe_areas` (a
controller's BANK − / + step through your own areas), SHIFT + DATA = the soft keys F1–F6 in every firmware.

**SDK 13 – media:** samples and songs no longer live inside a firmware. The host keeps **sources** – on the VIBE its
card and any USB stick (read only), in the emulator a folder you pick with the **Media** button (or drop onto the
device), in the plugin a folder from its settings – and your firmware reads them in pieces with `vibe_media_*`. It
writes only into `VIBE FM/<your id>/` (recordings, exports). Nothing waits: the host reads ahead in its own thread and
answers "not yet" (-2) until the bytes are there, then sends `VIBE_EV_MEDIA` READY. A pulled stick gives -1, never a
crash. Start from **`examples/example-media`**; the rules are in `AGENTS.md` ("SDK 13") and `sdk/include/vibe.h`.
`tools/wasmtest.mjs` takes `"media": {...}` in a script, `vibe-check` pulls a stick halfway through.
The emulator has it now; the VIBE FM VST gets it with its next version (0.4.2 answers -1: your firmware still loads).

## New in SDK 14, 15 and 16: drops, an Editor that talks back – and your own Editor page

**SDK 14:** files dropped onto the plugin's window reach your firmware (`vibe_media_dropped`) – a sample onto a pad.

**SDK 15:** a card's curve the mouse can draw (`vibe_editor_curve_draw`: a wavetable, a step shape), buttons on a card
(`vibe_editor_button` → `VIBE_EV_EDITOR`), and SysEx coming in (`VIBE_EV_SYSEX`, `vibe_sysex_in`) – with
`vibe_midi_send_sysex` a firmware swaps patches with a hardware synth. Menus in the Editor step with ◀ ▶.

**SDK 16 – your own Editor page:** an `editor/` folder (HTML, CSS, JS) next to your `.c` files replaces the Editor's
cards in the plugin: your design, your controls, still a firmware that plays everywhere. It runs in a sandbox and
talks to your firmware through `window.vibe` – parameters, curves, notes, the display, and bytes both ways
(`vibe.send` ↔ `vibe_editor_post`). **[EDITOR-PAGE.md](EDITOR-PAGE.md)** is the guide; `examples/example-synth/editor/`
a complete page. The emulator shows it too (`python3 tools/emulator.py`, View ▾ → Editor); in a DAW it needs VIBE FM VST 0.6.0.

## New in SDK 17: more cores – worker jobs

The VIBE FM has four cores; your audio uses one. Heavy, independent parts of a block (several synth tracks, a reverb)
can run on the others – `VIBE_JOBS(3)`, `vibe_on_job(k, frames)`, and in `vibe_on_audio` `vibe_jobs_begin(mask, frames)` …
`vibe_jobs_wait()` (`sdk/include/vibe.h`). Where a host has no workers – VIBE FM VST, the emulator, a one-core computer –
the jobs simply run one after the other, bit for bit the same: nothing to change, nothing to rebuild for anyone else.
A job calls nothing of the SDK and touches only its own buffers; `vibe-check` runs the jobs in two orders and must hear
the same sound. Jobs that only compute in a certain state (tracks set to a synth): `vibe-check --store DIR` (your
firmware's saved files) or `--script FILE` (a `wasmtest` script) – and `wasmtest` with `"jobs_reverse": true` must
print the same `out_hash` as without.

## New in SDK 18: clicks into the display

In VIBE FM VST and in the emulator, the mouse can work your display. With FOCUS (SDK 10), a single `vf_clicks(1)` in
`vibe_on_init` makes your `vf_button`s clickable and your `vf_value`s draggable. A firmware with its own zones, such as
tabs at the top, gets `VIBE_EV_TOUCH` with x and y. The VIBE has no touch screen, so a click is always an extra. Needs
VIBE FM VST 0.7.1.

## New in SDK 19: one firmware, more than one device

VIBE FM compatible devices may have more than the VIBE: up to 61 keys, 32 pads, eight more encoders, faders, more buttons,
a bigger display. `vibe_device()` tells your firmware what is there. Every firmware stays fully playable on the base,
"VIBE FM original", which is what the VIBE, the plugin and the emulator are today. Nothing changes for an existing
firmware, and a firmware built with SDK 19 still loads everywhere. **[DEVICES.md](DEVICES.md)** has the details, also for
anyone building such a device.

## New in SDK 20: a pad grid of any shape

A device may have more than 16 pads, up to 128, as a grid: the VIBE's 4 × 4, a Launchpad- or Push-style 8 × 8, a
20 × 4 matrix. `vibe_device()` tells your firmware the shape (`pad_cols` × `pad_rows`), `VIBE_EV_PAD` and `vibe_led_pad()`
reach every pad. Nothing changes on the VIBE, in the plugin or in the emulator: there the grid is 4 × 4, as always.
**[DEVICES.md](DEVICES.md)** has the details.

## New on 3 November: buttons, endless encoders, the `.vibe.aot` by hand

- **Buttons: pressed is only `e->down`.** For `VIBE_EV_BUTTON`, `e->value` is always 0 – in the plugin, the emulator and
  on the VIBE alike. Test `if (e->down)`, never `if (e->value > 0)`: such a firmware reacts to no button at all.
  `vibe-check` now presses 18 buttons and fails it, with the fix in its message.
- **Endless encoders in profiles:** a control can have its own `"knobs"` (`rel2c` …), see [CONTROLLERS.md](CONTROLLERS.md).
- **The `.vibe.aot` by hand** (your own build path, your own device – the VIBE FM host on a Raspberry Pi): WAMR **2.2.0**,
  ```
  wamrc --target=aarch64v8 --cpu=cortex-a53 --opt-level=3 --disable-aux-stack-check --enable-multi-thread \
        -o <id>.vibe.aot <id>.wasm
  ```
  Without it the host runs the `.wasm` in its interpreter, about ten times slower (and now says so loudly).
  `--enable-multi-thread` is needed for shared memory and worker jobs. Thanks to Lars / MandaLars, who worked it out
  himself for the SOUNDMASCHINE on a Pi 5 before it was written down here.

## Updates

This repository is where the SDK grows: `git pull` brings new versions, the **issues** are the place for questions,
bugs and wishes. Your own firmwares are best kept in a repository of your own (copy `examples/` as a start).

## Play it in the browser – the emulator

```sh
python3 tools/emulator.py
```

opens the **VIBE FM emulator** on your own computer (http://127.0.0.1:8766): the whole device – keys, pads, knobs, the
display, the rear panel – playing every firmware you built (`build/*.wasm`, choose it in the top bar), or any `.wasm` you
drop onto it. **OberVave 3.1** and **VIBE DJ** (with its two DECKS and three demo tracks) are always in the list –
to try the device, and OberVave's full source is in `firmwares/obervave-src/` to read (GPL-3.0). How to play all of it:
**`MANUAL.md`**. Your MIDI keyboard works (Web MIDI), the audio input too. **Media** (top bar) picks a folder of
samples or songs a firmware may read (SDK 13; Chrome / Edge – elsewhere drop a folder onto the device). Nothing leaves your computer. (The emulator in
`emulator/` is free to use for developing VIBE FM firmwares, not to pass on – see `emulator/LICENSE`.)

## Working with an AI assistant

Give it **`AGENTS.md`** (Claude Code, Cursor, Copilot and others read it on their own when you open this folder; else
paste it in): the shape of a firmware, the hard rules, the API in short, patterns that work, how to build and check,
the common mistakes. Then ask for the instrument you want – and have it run `vibe-check` before it says it is done.

## Test without a DAW

```sh
node tools/vibe-check.mjs build/my-synth.wasm
```

The same check every VIBE FM firmware has to pass: format, robustness under random input (no crash, no NaN), real-time
behaviour (no storing or logging in the audio path, no blocks over budget at start), storage, and media: it offers a
card and a USB stick and pulls the stick halfway through (your firmware must get -1, never crash). `tools/wasmtest.mjs` runs
a scripted performance and writes the audio to WAV and the screen to PNG (usage in its header); `"media"` in the script
gives it folders as sources, `"media_remove"` pulls one.

## Licences – what is yours

- The SDK (`sdk/`, `tools/`, `examples/`) is **Apache-2.0**: build anything with it, open or closed, free or
  commercial. Keep the notice from `sdk/NOTICE`.
- **Your firmware is yours.** Any licence you like.
- "VIBE FM" is a registered trademark. You may always say *"built with the VIBE FM SDK"*. "VIBE FM compatible" and the
  logo come later, for firmwares that pass `vibe-check`.

## Built for the hardware

Everything here exists for one reason: **your firmware is meant to run natively on the VIBE FM hardware** – the same
`.wasm`, unchanged. The VIBE is a desktop instrument in development, and the SDK's limits are its limits on purpose:

| | |
|---|---|
| **Brain** | a quad-core ARM Cortex-A53 (1.5 GHz, 1 GB RAM) running Linux. Your `.wasm` is compiled to native ARM code ahead of time (WAMR AOT): measured at 1.1–1.2× the cost of a native build, bit-exact with the plugin. |
| **Controls** | 25 velocity keys, 16 RGB pads, encoders 1–4, A, B and a DATA wheel, F1–F6 under the display, six keys right of ENTER, a play section, SHIFT, OCT −/+ – exactly the enums in `vibe.h`. |
| **Display** | 4.0" colour, 480 × 320 – your `vibe_on_draw` picture, 1:1. |
| **I/O** | OUT L/R, a headphone out with its own signal, IN 1 / IN 2 (line, mic, Hi-Z), PEDAL, CV OUT, GATE OUT, CV IN, CLK IN, MIDI IN/OUT (5-pin DIN), USB-C (MIDI and audio to a computer), a USB host port (controllers, sticks), microSD, Wi-Fi and Bluetooth MIDI. |

That is why there is no heap, no libc, no threads of your own and no free-form GUI: a firmware that keeps these rules
runs in the DAW today and on the box tomorrow – and the browser in between. And you get very far within them:
**OberVave 3** – four polyphonic tracks, filters, envelopes, LFOs, an arpeggiator, a sequencer, chorus, delay, reverb,
a mixer with drive – uses about **6–7 % of one core** on the prototype's ARM board (174 µs per 128-frame block). Measure
yours with `vibe-check`; a device budget per firmware follows with the hardware.

The hardware is in development; details may still change – the contract in `vibe.h` does not break (new things are
always added, never taken away).

## What's next – and what is not there yet

- **The VIBE FM hardware:** your `.wasm` runs there unchanged – 25 keys, 16 pads, encoders, a 4.0" colour display, audio
  and CV in/out.
- **A place on thefirmwarehub.com** for what you build.
- **The VIBE FM Protocol specification** in English (the rules behind `vibe-check`, the two-flows mode).
- A **device CPU budget** in `vibe-check`, measured on the hardware.

Not in SDK 13 yet – tell us if you need one of these first:

- **More than one stereo output to the DAW** (each track on its own channel; today: stereo out, dual mono and the Cue).
- **Reporting your own latency** to the DAW (a look-ahead limiter, for instance).
- **Audio input** works (`vibe_request(VIBE_REQ_AUDIO_INPUT)`, then `in` / `vibe_input`): in the plugin it is the
  instrument's sidechain input – how you route a track into an instrument differs per DAW. An effect variant that sits
  on a track's own signal is coming.

What is stable: the four functions, the events, the parameters and storage. The DSP and drawing helpers may still grow
(nothing in them will be taken away).

Questions, ideas, bugs: **zven@thefirmwarehub.com** – we read everything.

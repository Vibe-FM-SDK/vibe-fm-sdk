# VIBE FM SDK – Early Access

**Build your own instrument once: as a plugin in your DAW today, on the VIBE FM hardware tomorrow.**

You write a synth (or a sampler, a sequencer, an effect…) in plain C against one small API. It compiles to a single
WebAssembly file. That one `.wasm` runs as a **VST3 / AU / CLAP plugin** in your DAW through VIBE FM VST, in the
**browser**, and on the coming **VIBE FM hardware**. No plugin framework, no GUI toolkit, no per-platform builds.

> **Early Access – thank you for being one of the first.** This is a preview for a handful of developers. Please don't
> share the SDK or post it publicly yet. Everything here is still allowed to change. Tell us what is missing, unclear or
> broken: open an **issue** in this repository, or mail **zven@thefirmwarehub.com**

---

## What you need

| | |
|---|---|
| **Zig 0.13.0** | its clang compiles C to WebAssembly – nothing else to install. Download: <https://ziglang.org/download/#release-0.13.0>, unpack, put `zig` on your PATH (or set `ZIG=/path/to/zig`). |
| **VIBE FM VST 0.3** | the host you test in, free: <https://thefirmwarehub.com/convert> (macOS, Windows, Linux). |
| **The native firmwares** *(to look at)* | what this SDK makes: OberVave 3 (VOX coming soon) – <https://thefirmwarehub.com/native/>. OberVave 3 comes with VIBE FM VST, the Editor included. |
| a shell | macOS / Linux: the terminal. Windows: Git Bash or WSL (`tools/build.sh` is a plain `sh` script). |
| Node.js 18+ *(optional)* | for `tools/vibe-check.mjs` and `tools/wasmtest.mjs` (testing without a DAW). |

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

```sh
cp -r examples/hello my-synth          # the folder's name is your firmware's id (letters, digits, - _ .)
mv my-synth/hello.c my-synth/my-synth.c
# edit my-synth/firmware.json: name, version, author, description, license
sh tools/build.sh my-synth
```

`examples/hello` (~60 lines) is the smallest useful firmware: a sine on the keys, pads that light up, an encoder that
bends. `examples/example-synth` (~150 lines) is a real instrument: 8 voices, saw through a resonant filter, envelope,
DAW parameters and the song position. Start from whichever is closer to what you want.

**`firmware.json`** is your firmware's card – what the plugin shows in its library and the hub will show later:

| field | |
|---|---|
| `name` | shown everywhere – "MY SYNTH" (up to 40 characters) |
| `version` | yours, e.g. "1.0" – shown next to the name; change it when you ship a new build |
| `author` | your name or handle |
| `description` | one line: what it is |
| `license` | your firmware's licence – an SPDX id ("MIT", "GPL-3.0", "Apache-2.0") or "proprietary" |

The id is the folder's name; it must not change once people use your firmware (their saved files and DAW projects hang on it).

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

## The rules (they keep the audio clean)

- **No heap, no libc.** The target is freestanding WebAssembly: use static memory and the SDK's helpers.
- **The audio callback is sacred:** no storing (`vibe_store_*`), no logging, no long loops in `vibe_on_audio`. Do heavy
  setup (tables, samples) in `vibe_on_init`.
- **Don't run code at load time** other than through `vibe_on_init`.
- Leave `VIBE_PARALLEL_AUDIO` off for now (it has extra rules we have not published yet).

## Updates

This repository is where the SDK grows: `git pull` brings new versions, the **issues** are the place for questions,
bugs and wishes. Your own firmwares are best kept in a repository of your own (copy `examples/` as a start).

## Test without a DAW

```sh
node tools/vibe-check.mjs build/my-synth.wasm
```

The same check every VIBE FM firmware has to pass: format, robustness under random input (no crash, no NaN), real-time
behaviour (no storing or logging in the audio path, no blocks over budget at start), storage. `tools/wasmtest.mjs` runs
a scripted performance and writes the audio to WAV and the screen to PNG (usage in its header).

## Licences – what is yours

- The SDK (`sdk/`, `tools/`, `examples/`) is **Apache-2.0**: build anything with it, open or closed, free or
  commercial. Keep the notice from `sdk/NOTICE`.
- **Your firmware is yours.** Any licence you like.
- "VIBE FM" is a registered trademark. You may always say *"built with the VIBE FM SDK"*. "VIBE FM compatible" and the
  logo come later, for firmwares that pass `vibe-check`.

## What's next – and what is not there yet

- **The VIBE FM hardware:** your `.wasm` runs there unchanged – 25 keys, 16 pads, encoders, a 3.5" colour display, audio
  and CV in/out.
- **A place on thefirmwarehub.com** for what you build.
- **The VIBE FM Protocol specification** in English (the rules behind `vibe-check`, the two-flows mode).

Not in SDK 8 yet – tell us if you need one of these first:

- **MIDI out** (a sequencer driving other gear or a DAW track). MIDI in is complete.
- **More than one stereo output** (tracks on separate DAW channels).
- **Reporting your own latency** to the DAW (a look-ahead limiter, for instance).
- **Audio input** works (`vibe_request(VIBE_REQ_AUDIO_INPUT)`, then `in` / `vibe_input`): in the plugin it is the
  instrument's sidechain input – how you route a track into an instrument differs per DAW. An effect variant that sits
  on a track's own signal is coming.

What is stable: the four functions, the events, the parameters and storage. The DSP and drawing helpers may still grow
(nothing in them will be taken away).

Questions, ideas, bugs: **zven@thefirmwarehub.com** – we read everything.

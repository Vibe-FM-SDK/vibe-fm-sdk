# AGENTS.md – building VIBE FM firmwares (for AI coding assistants)

You are helping someone write a **firmware for VIBE FM**: an instrument or effect in plain C that compiles to one
WebAssembly file. The same `.wasm` runs as a VST3 / AU / CLAP plugin (VIBE FM VST), in the browser (the emulator) and
natively on the VIBE FM hardware. Read this file first, then the headers in `sdk/include/` – they are the reference, every
function is documented where it is declared. When this file and a header disagree, the header wins.

## The shape of a firmware

A firmware is a folder with one or more `.c` files and a `firmware.json`. It implements exactly four functions:

```c
#include "vibe.h"
#include "vibe_dsp.h"   // oscillators, filters, envelopes, fast math (vd_*)
#include "vibe_gfx.h"   // drawing and text on the 480 x 320 display (vg_*)

void vibe_on_init(float sample_rate)  { }   // once: tables, voices, parameters, vibe_notes(), the Editor
void vibe_on_event(const vibe_event *e) { }  // keys, pads, buttons, encoders, MIDI, the DAW (VIBE_EV_*)
void vibe_on_audio(const float *in, float *out_l, float *out_r, int frames) { }   // fill both outputs, `frames` long
void vibe_on_draw(uint16_t *fb)       { }   // paint the WHOLE display every time (vg_* draws into fb)
```

The host calls all four from **one thread, one after the other** – no locks, no atomics needed.

## Hard rules (the build or `vibe-check` fails, or the hardware breaks)

1. **No heap, no libc.** No `malloc`, `printf`, `<math.h>` (`sinf`, `expf`, `powf` …), `<stdio.h>`, threads, files.
   All memory is `static` (global arrays). Maths: `vd_sin` (phase in turns, 0..1), `vd_exp2`, `vd_log2`, `vd_pow`,
   `vd_tanh`, `vd_sqrt`, `vd_mtof`, `vd_coef`. Text and numbers: `vg_itoa`, `vg_ftoa`. Available from the SDK: `memset`,
   `memcpy`, `memmove`, `memcmp`, `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strchr`, `memchr`, `strtol`, `atoi`,
   `abs`. Anything else that needs an import makes the `.wasm` invalid ("forbidden imports" in `vibe-check`).
2. **The sample rate and the block size are the host's.** Use the `sample_rate` given to `vibe_on_init` (44100, 48000,
   96000 …). `frames` changes from call to call (1 … 256, `VIBE_MAX_BLOCK`). Never assume 48000 or 128.
3. **`vibe_on_audio` is real time.** No `vibe_store_*`, no `vibe_log`, no long loops, nothing that allocates or waits.
   Do heavy work (wavetables, sample loading, tables) in `vibe_on_init` or spread over `vibe_on_draw`.
4. **Write every frame of both outputs** in every `vibe_on_audio` call. Keep the level within -1 … +1. No NaN, no
   infinity (denormals: add a tiny DC or flush small values in feedback paths).
5. **`vibe_on_draw` paints the whole picture** and does nothing time-critical: the host may draw less often or not at
   all (the plugin with its window closed). Sequencers, envelopes, recording belong in `vibe_on_audio`.
6. **Nothing runs at load time** except through `vibe_on_init` (no constructors, no work in global initialisers beyond
   constant data).
7. Leave `VIBE_PARALLEL_AUDIO` off.

## The controls (fixed: they are the hardware's)

- 25 keys (`VIBE_EV_KEY`, id 0..24, velocity 0..1), 16 RGB pads (`VIBE_EV_PAD`, id 0..15), encoders `VE_1`..`VE_4`,
  `VE_A`, `VE_B`, `VE_DATA` (`VIBE_EV_ENCODER`, value = detents turned, may be fractional), buttons `enum vibe_button`
  (`VIBE_EV_BUTTON`: F1–F6 under the display, K1–K6 right of ENTER, SHIFT, OCT −/+, ENTER, RETURN, HOME, ◀ MENU ▶,
  REC STOP PLAY, DATA push).
- **HOME** belongs to the firmware library: on `VB_HOME` down call `vibe_request(VIBE_REQ_FIRMWARE_MENU)`.
- Lights: `vibe_led_pad`, `vibe_led_button`; captions for F1–F6 and K1–K6: `vibe_label` (≤ 7 characters).
- The house style for the six keys right of ENTER (K1–K6): **SOUND · ENV · LFO · ARP · FX · SEQ** where it fits; draw
  the page names on the display (the keys carry no print), full width, big type.

## Notes, parameters, the DAW

- Call `vibe_notes(VIBE_NOTES)` in `vibe_on_init`: keys, MIDI notes and the DAW's notes all arrive as `VIBE_EV_NOTE`
  (id = note 0..127, value = velocity). Other MIDI (bend, CCs, aftertouch) still comes as `VIBE_EV_MIDI` (`midi[0..2]`).
- **Parameters**: a `static const vibe_param LIST[]` with `vibe_params(LIST, n)` in `vibe_on_init`. The `id` string is
  permanent (DAW automation hangs on it) – rename `name`, never `id`. Read with `vibe_param_get(i)`; a change from the DAW
  arrives as `VIBE_EV_PARAM` (the value is already stored); when your own knob changes one, call `vibe_param_set(i, v)`.
  Groups (`vibe_param_groups`, "A · FILTER" = tab A, card FILTER) build the plugin's Editor.
- **The song position**: `vibe_transport()` (bpm, playing, beat, bar_start, loop) – fill sequencers from it inside
  `vibe_on_audio`; `has == 0` means no host clock: run on your own tempo.

## SDK 9

- **MIDI out**: `vibe_midi_send(st, d1, d2)`, sample-exact from audio with `vibe_midi_send_at(…, frame)`. Clock:
  `vibe_clock_out(1)` + `vibe_tempo_set(bpm)`, START/STOP with `vibe_transport_out()`. No automatic thru.
- **Outputs**: `vibe_outputs(VIBE_OUT_DUAL_MONO)` (two mono outs) and/or `VIBE_OUT_CUE` (a headphone signal of its own:
  write `vibe_cue(0)` / `vibe_cue(1)` inside `vibe_on_audio`; `vibe_cue_host()` says whether the host plays it).
- **The Editor**: `vibe_editor_cards`, `vibe_editor_control` (SLIDER, TOGGLE, MENU, BIPOLAR, XY_X + XY_Y as a pair on one
  card), `vibe_editor_accent`, `vibe_editor_tab_color`, `vibe_editor_curve` (2..64 points 0..1, under 1 ms),
  `vibe_param_help` (≤ 120 characters). Declarative only – no pixels, no mouse.
- **Storage**: `vibe_store_write/read/size/delete/list/read_at` – names `a-z 0-9 . _ -` (folders allowed). Never from
  `vibe_on_audio`. In the plugin the files live in the DAW project, per instance.

## Patterns that work

- **Voices**: a fixed `static voice V[N]`; note on → a free voice, else the oldest; per sample `vd_saw(phase, dt)` (band-
  limited) → `vd_svf_tick` (set coefficients once per block with `vd_svf_set` when the cutoff changes) → `vd_env_next`.
- **Smoothing** (no zipper noise): `float c = vd_coef(0.01f, sr);` once, then per sample `x = target + (x - target) * c;`
  (`vd_coef(seconds, sr)` is the one-pole factor: ~1/e of the way in that time).
- **Display**: `vg_clear`, then `vg_rect` / `vg_round` / `vg_line` / `vg_text(s, x, y, font, 0xRRGGBB, align)`; fonts
  `VG_SMALL`, `VG_BOLD`, `VG_MED`, `VG_BIG`, `VG_HUGE`. Colours are `0xRRGGBB`; the screen is 480 x 320.
- The examples are the best start: `examples/hello` (60 lines), `examples/example-synth` (a full synth with parameters and
  the song position), `examples/example-seq` (MIDI out, clock, its own Editor card and curve).

## Build, run, check – do this before saying it is done

```sh
sh tools/build.sh my-synth                       # -> build/my-synth.wasm + build/my-synth.zip (needs Zig 0.13.0)
node tools/vibe-check.mjs build/my-synth.wasm     # must end with PASSED: format, crashes, NaN, real time, storage, SDK 9
python3 tools/emulator.py                        # the device in the browser at http://127.0.0.1:8766 – play it
node tools/wasmtest.mjs build/my-synth.wasm script.json out/   # scripted: audio.wav, screenshots, midi.json
```

Then drop `build/my-synth.zip` onto the VIBE FM VST window in a DAW. The folder name is the firmware's id (`a-z 0-9 - _ .`);
`firmware.json` holds `name`, `version`, `author`, `description`, `license`.

## Common mistakes

- Using `sinf` / `powf` / `rand()` → use `vd_sin` (turns, not radians!) / `vd_pow` / `vd_rand`.
- A phase that is never wrapped (`if (phase >= 1) phase -= 1`) → precision loss after minutes.
- Computing filter coefficients per sample with `vd_svf_run` in a hot loop → set them once per block.
- Drawing only what changed → the host expects a full frame each call.
- Hard-coding 48000 / 128 → wrong pitch and tempo in a DAW at 44.1 kHz or with odd buffer sizes.
- Reading `vibe_param_get` per sample for many parameters → read once per block into locals.
- Forgetting `vibe_request(VIBE_REQ_FIRMWARE_MENU)` on HOME → the player is stuck in your firmware on the hardware.

Licence: the SDK is Apache-2.0; the user's firmware is theirs, under any licence they choose. "VIBE FM" is a trademark:
"built with the VIBE FM SDK" is fine, "VIBE FM compatible" or the logo need permission.

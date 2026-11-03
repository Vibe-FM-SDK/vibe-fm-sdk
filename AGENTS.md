# AGENTS.md – building VIBE FM firmwares (for AI coding assistants)

You are helping someone write a **firmware for VIBE FM**: an instrument or effect in plain C that compiles to one
WebAssembly file. The same `.wasm` runs as a VST3 / AU / CLAP plugin (VIBE FM VST), in the browser (the emulator) and
natively on the VIBE FM hardware. Read this file first, then the headers in `sdk/include/` – they are the reference, every
function is documented where it is declared. When this file and a header disagree, the header wins.

## House rules – every VIBE FM firmware (people and AIs alike)

1. **It plays at once.** Sound on the keys and pads right after start, no setup first.
2. **The layout is the house layout:** full width, big type. The six keys right of ENTER are **SOUND · ENV · LFO · ARP ·
   FX · SEQ** where it fits, encoders 1–4 are the four values shown above F1–F6, and F1–F6 have their captions on the display.
3. **No controller code in a firmware.** Controllers are mapped by the host (`CONTROLLERS.md`). A firmware with tracks
   reports its mixer (SDK 11).
4. **FOCUS (SDK 10):** every control on the display is reachable with DATA + ENTER (`vf_*`). If DATA already has a
   job, use `vf_mode(VF_ON_HOLD)`.
5. **HOME shows the controls:** HOME tapped = what every key, knob and pad on the open page does (`vg_controls()` draws
   the standard overview in one call); HOME again or RETURN closes it. HOME held 1 s or SHIFT+HOME opens the firmware
   library – the **host** does that. Never call `vibe_request(VIBE_REQ_FIRMWARE_MENU)` for HOME (vibe-check fails it).
6. **No clicks, ever:** nothing heavy in `vibe_on_audio`, and nothing long in `vibe_on_draw`. On the VIBE, drawing and
   sound share one flow. `wasmtest` reports `draw_max_ms`, which should stay at a few ms.
7. **`vibe-check` passes before you call it done.**
8. **You write the firmware, never the host.** All work happens in the firmware's folder (C, `firmware.json`). Never
   modify, patch, rebuild or redistribute VIBE FM VST (closed software, all rights reserved) or the emulator
   (`emulator/`, `tools/emulator.py`). That includes the binaries, their embedded HTML/JS/CSS and `emulator/js/*`. The
   Editor view is described by the firmware (`vibe_params`, `vibe_param_groups`, `vibe_editor` in `sdk/include/vibe.h`).
   If the host lacks something the firmware needs, stop and tell the user to open an issue in this repository.
   An own Editor page (SDK 16, `EDITOR-PAGE.md`) belongs to the firmware: `editor/` in its folder, talking only through
   `window.vibe` – the firmware must stay fully playable without it.
9. **Give it a face:** an `icon.png` next to `firmware.json` (16:9, e.g. 960 × 540, up to 1 MB) – its tile in the VIBE's
   main menu and its card in the hub. One strong motif that reads small; little or no text.

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
3. **`vibe_on_audio` is real time.** No `vibe_store_*`, no `vibe_media_*`, no `vibe_log`, no long loops, nothing that allocates or waits.
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
  REC STOP PLAY, DATA push). **A button is pressed when `e->down` is 1 – `e->value` is always 0 for buttons**, in every
  host: test `if (e->down)`, never the value (vibe-check fails a firmware that does).
- **HOME** is the same in every firmware: tapped = the controls of the open page (`vg_controls(title, rows, n)`, see
  `sdk/include/vibe_gfx.h`), HOME again / RETURN = close. The library (HOME held 1 s, SHIFT+HOME) is the host's.
  RETURN, not HOME, goes back to your start page.
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

## SDK 10: FOCUS – every control reachable with one knob and one button

The standard for every firmware: whatever is on the display can be reached with **DATA** (turn) and **ENTER** (or DATA
pressed) – for small controllers (an MPK mini: K5 + its wheel pressed). Declare where each control is drawn, every frame:

```c
void vibe_on_init(float sr) { /* … */ vf_mode(VF_ON_HOLD); }   // DATA has its own job here: ENTER held 0.6 s = FOCUS
                                                                // (VF_ALWAYS, the default: DATA always walks)
void vibe_on_draw(uint16_t *fb) {
  /* … draw as always, then where each control is shown: */
  vf_begin();
  vf_value(1, 20, 60, 100, 40, &cutoff, 0, 1, 0.01f);   // a float you own: DATA changes it while editing
  vf_button(2, 140, 60, 80, 30);                       // a button: ENTER -> VF_PRESS with id 2
  vf_encoder(VE_1, 240, 60, 100, 40);                  // stands for encoder 1: DATA turns arrive as VE_1 turns
  vf_key(VB_F1, 3, 286, 74, 30);                       // stands for F1: ENTER = F1 pressed and let go
  vf_end(0xFF8A1F);                                    // draws the frame
}
void vibe_on_event(const vibe_event *e) {
  int id, r = vf_event(e, &id);                        // FIRST
  if (r == VF_PRESS && id == 2) start_stop();
  if (r == VF_CHANGED && id == 1) filter_dirty = 1;    // the value is written already
  if (r != VF_NO) return;
  /* … your own controls as always */
}
```

- The order of the calls is the order DATA walks. At most 64 controls a page; only what is drawn this frame is reachable.
- `vf_encoder` / `vf_key` are the least work for an existing firmware: nothing else changes.
- `vf_enable(0)` while a page uses DATA / ENTER itself (a list). With `VF_ON_HOLD`, a short ENTER still reaches you
  (on release); a long one never does.
- Full reference: `sdk/include/vibe_gfx.h`. A complete example: `firmwares/obervave-src/main.c` (`focus_items`).

## SDK 11: the mixer (only if the firmware has tracks)

Report the tracks as parameters with the fixed ids `mix_<n>_level` (0..1), `mix_<n>_pan` (-1..1), `mix_<n>_mute` /
`_solo` / `_arm` (0/1, steps 2), and `mix_select` (1..N). Then any mixer controller and the DAW work them. Keep them current
with `vibe_param_set` (compare once per block). Never write controller-specific code: controllers are the host's
(profiles, see `CONTROLLERS.md`). Example: `firmwares/obervave-src/o3_daw.h`.

## SDK 13: media (samples and songs the host keeps)

- Samples and songs are not stored in the firmware: the host offers **sources** (the VIBE's card and USB sticks, a folder
  in the emulator's **Media** button, the plugin's media folder). `vibe_media_sources`, `vibe_media_list`,
  `vibe_media_stat`, `vibe_media_read` (in pieces); write only with `vibe_media_create/append/close` – it lands in
  `VIBE FM/<your id>/`, the host picks the source and makes the name unique.
- **Never from `vibe_on_audio`.** Fill your own buffer from `vibe_on_event` / `vibe_on_draw`; the audio plays from it.
- `VIBE_MEDIA_PENDING` (-2) = not yet: ask again when `VIBE_EV_MEDIA` / `VIBE_MEDIA_EV_READY` comes. Keep going after a
  success until you get -2 or are done – a host that does not draw (the plugin, its window closed) only sends events.
- -1 = gone (a stick pulled): show it, never crash. `VIBE_MEDIA_EV_SOURCES` says a source came or went.
- Example: `examples/example-media`. `vibe-check` pulls a stick halfway through its run.

## SDK 18: clicks into the display (plugin and emulator, never the VIBE)

- With FOCUS, add `vf_clicks(1)` in `vibe_on_init`. A click then presses a `vf_button`, sets a slider-shaped `vf_value`
  (at least three times as wide as high) where it is clicked or dragged, and edits a smaller value with a vertical drag.
  Nothing else to write.
- Your own zones: `VIBE_EV_TOUCH` (id `VIBE_TOUCH_DOWN` / `_MOVE` / `_UP`, `VIBE_TOUCH_X(e)`, `VIBE_TOUCH_Y(e)`). The soft
  keys (y 282–320) never send one. On the knob columns, a drag turns the knob and a plain click arrives as DOWN + UP.
- A click is always an extra: everything stays playable with the controls. Needs VIBE FM VST 0.7.1.

## SDK 19: other devices (`DEVICES.md`)

- Every firmware must be fully playable on the base, "VIBE FM original" (25 keys, 16 pads, the seven encoders, the
  buttons, 480 × 320). That is the VIBE, the plugin and the emulator today.
- `vibe_device(&info)` in `vibe_on_init` says whether the device has more: keys up to 61, pads up to 32, extra encoders
  `VE_X1`…, faders `VIBE_EV_FADER`, extra buttons `VB_X1`…, a bigger display. Use the extras as shortcuts, never as the only
  way to a function. Ignore ids you do not use. Always draw 480 × 320.
- Do not call `vibe_device()` from `vibe_on_audio`.
- SDK 20: the pads are a grid, `pad_cols` × `pad_rows` (up to 128 pads). Use more than pads 0–15 only as an extra
  (a 64-step view, a clip grid), always also reachable on the 4 × 4; show the grid's state on the display too.

## Patterns that work

- **Voices**: a fixed `static voice V[N]`; note on → a free voice, else the oldest; per sample `vd_saw(phase, dt)` (band-
  limited) → `vd_svf_tick` (set coefficients once per block with `vd_svf_set` when the cutoff changes) → `vd_env_next`.
- **Smoothing** (no zipper noise): `float c = vd_coef(0.01f, sr);` once, then per sample `x = target + (x - target) * c;`
  (`vd_coef(seconds, sr)` is the one-pole factor: ~1/e of the way in that time).
- **Display**: `vg_clear`, then `vg_rect` / `vg_round` / `vg_line` / `vg_text(s, x, y, font, 0xRRGGBB, align)`; fonts
  `VG_SMALL`, `VG_BOLD`, `VG_MED`, `VG_BIG`, `VG_HUGE`. Colours are `0xRRGGBB`; the screen is 480 x 320.
- A full firmware to read: `firmwares/obervave-src/` (OberVave 3.1, GPL-3.0) – pages, sequencer, mixer, Editor, MIDI out,
  FOCUS.
- The examples are the best start: `examples/hello` (60 lines), `examples/example-synth` (a full synth with parameters and
  the song position), `examples/example-seq` (MIDI out, clock, its own Editor card and curve), `examples/example-media`
  (SDK 13: sources, folders, reading ahead, writing a file).

## Build, run, check – do this before saying it is done

```sh
sh tools/build.sh my-synth                       # -> build/my-synth.wasm + build/my-synth.zip (needs Zig 0.13.0)
node tools/vibe-check.mjs build/my-synth.wasm     # must end with PASSED: format, crashes, NaN, real time, storage, SDK 9, media (a stick is pulled halfway)
python3 tools/emulator.py                        # the device in the browser at http://127.0.0.1:8766 – play it
node tools/wasmtest.mjs build/my-synth.wasm script.json out/   # scripted: audio.wav, screenshots, midi.json
```

Then drop `build/my-synth.zip` onto the VIBE FM VST window in a DAW. The folder name is the firmware's id (`a-z 0-9 - _ .`);
`firmware.json` holds `name`, `version`, `author`, `description`, `license`.

## Porting an FM-1 firmware

If the user brings an M-VAVE FM-1 firmware: follow **FROM-FM1.md** – keep the sound engine, replace the main loop, ISRs
and HAL with the four callbacks, compute rate tables from `sample_rate`, redraw on 480 x 320 in the house layout.

## Common mistakes

- Using `sinf` / `powf` / `rand()` → use `vd_sin` (turns, not radians!) / `vd_pow` / `vd_rand`.
- A phase that is never wrapped (`if (phase >= 1) phase -= 1`) → precision loss after minutes.
- Computing filter coefficients per sample with `vd_svf_run` in a hot loop → set them once per block.
- Drawing only what changed → the host expects a full frame each call.
- Hard-coding 48000 / 128 → wrong pitch and tempo in a DAW at 44.1 kHz or with odd buffer sizes.
- Reading `vibe_param_get` per sample for many parameters → read once per block into locals.
- Calling `vibe_request(VIBE_REQ_FIRMWARE_MENU)` on HOME → wrong since 3 November 2026: HOME tapped shows the controls
  (`vg_controls`); the host opens the library on HOME held 1 s or SHIFT+HOME.

Licence: the SDK is Apache-2.0; the user's firmware is theirs, under any licence they choose. "VIBE FM" is a trademark:
"built with the VIBE FM SDK" is fine, "VIBE FM compatible" or the logo need permission.

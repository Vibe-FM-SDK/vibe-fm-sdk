# How to create a native VIBE FM plugin, starting from your FM-1 firmware

**You wrote a firmware for the M-VAVE FM-1. This is how it becomes a native VIBE FM firmware: one `.wasm` that runs as a
VST3 / AU / CLAP plugin in every DAW, in the browser, and on the VIBE FM hardware.**

There are two ways. Most people start with the first and move to the second once they like what they hear.

| | **Converted** (no work) | **Native** (this guide) |
|---|---|---|
| How | thefirmwarehub.com/convert: paste your GitHub link | you port your code to the VIBE FM SDK |
| Your code | unchanged: it runs on an imitated FM-1 | your engine stays, the FM-1 layer goes |
| Screen | your 240 × 240 picture, in the middle of the VIBE's display | your own pages on the full 480 × 320 |
| Controls | VIBE keys pretend to be FM-1 keys | VIBE keys call your functions directly |
| Sound | your 44.1 kHz, resampled | the host's real rate, nothing resampled |
| DAW | notes and audio | plus automatable parameters, an Editor, the song position, MIDI out |
| Latency on the VIBE | 8 ms buffer (your main loop runs inside the audio) | 4 ms |

**OberVave went this way** (OberVave 2 was an FM-1 port, OberVave 3 is native). Its full source is in
`firmwares/obervave-src/` (GPL-3.0): read it next to this guide. Everything below comes from that port.

**Contents:** [What stays, what goes](#what-stays-what-goes) · [The FM-1 → VIBE FM map](#the-fm-1--vibe-fm-map) ·
[Step by step](#step-by-step) · [The traps](#the-traps) · [Before you share it](#before-you-share-it)

## What stays, what goes

**Stays (usually unchanged):** your sound engine, your effects, your patches and parameter tables, your sequencer's
data and how it plays, your file formats.

**Goes:** the main loop, the ISRs, the HAL (`fm1_*`), USB, the flash driver, OTA / recovery / bootloader code, the key
matrix scan, the 240 × 240 drawing code, and anything that waits on a timer.

A rule of thumb from OberVave: keep the files that make sound (`obv_engine.h`, `obv_dsp.h`, `obv_fx.h`,
`obv_params.h`, `obv_patches.h`, `obv_seq.h`) and write new ones for everything the player sees and touches
(`o3_*.h`).

## The FM-1 → VIBE FM map

A VIBE FM firmware is four functions. The host calls them one after the other on one thread, so you need no locks.

```c
void vibe_on_init(float sample_rate);                                   // once: tables, voices, parameters
void vibe_on_event(const vibe_event *e);                                // keys, pads, buttons, encoders, MIDI, the DAW
void vibe_on_audio(const float *in, float *out_l, float *out_r, int frames);   // fill both outputs
void vibe_on_draw(uint16_t *fb);                                        // paint the whole 480 x 320 display
```

| On the FM-1 | On VIBE FM |
|---|---|
| `main()` / `fm1_cstart()` with a loop that never returns | **gone.** Setup goes into `vibe_on_init`, everything else is called by the host |
| the audio ISR (ALNK half buffer, `fm1_audio_*`) | `vibe_on_audio`: fill `out_l` / `out_r`, `frames` long (1…256, it changes) |
| a fixed 44.1 kHz | the `sample_rate` given to `vibe_on_init` (44.1, 48, 96 kHz…): compute your rate tables there |
| the key / button / encoder scan, edges, debounce | `vibe_on_event`: `VIBE_EV_KEY`, `VIBE_EV_PAD`, `VIBE_EV_BUTTON`, `VIBE_EV_ENCODER` (detents, can be fractional) |
| your MIDI parser (USB, UART) | `vibe_notes(VIBE_NOTES)`: keys, MIDI and DAW notes all arrive as `VIBE_EV_NOTE`; the rest as `VIBE_EV_MIDI` |
| the timer tick, `fm1_ms()`, waiting on a clock | the audio clock: count frames in `vibe_on_audio` (`vibe_frames()`), never wait |
| drawing into the 240 × 240 LCD | `vibe_on_draw`: `vg_*` in `vibe_gfx.h`, the whole picture every time |
| flash sectors, erase / write | `vibe_store_write` / `_read` (files with names), never from `vibe_on_audio` |
| LEDs under the keys | `vibe_led_pad`, `vibe_led_button`, captions with `vibe_label` |
| `sinf`, `expf`, your own fast maths | `vd_sin` (turns, not radians!), `vd_exp2`, `vd_pow`, `vd_tanh`… in `vibe_dsp.h` |
| `malloc` | static arrays (no heap on the target) |
| the second core | `VIBE_JOBS` (SDK 17), later; one core first |
| samples compiled into flash | the host's media (`vibe_media_*`, SDK 13): samples and songs live outside the firmware |

The controls are the VIBE's, not the FM-1's: 25 keys, 16 RGB pads, encoders 1–4 above the soft keys, A, B, DATA,
F1–F6 under the display, six keys right of ENTER, SHIFT, OCT −/+, ENTER, RETURN, HOME, ◀ MENU ▶, REC STOP PLAY.
`AGENTS.md` lists them all.

## Step by step

Do it in steps and listen after each one. OberVave took five; each was built, checked and compared before the next.

**1. A skeleton that makes your sound.** Start from `examples/example-synth` (a folder whose name is your firmware's
id). Copy your engine files in. In `vibe_on_init`, compute your rate-dependent tables from `sample_rate` (pitch,
filter, envelope times, glide, LFO, delay and reverb lengths). In `vibe_on_audio`, call what your audio ISR called.
Turn notes into your engine's note on / off. One simple page on the display. Then compare: render the same notes in your
old firmware and the new one. At 44.1 kHz they should match bit for bit if nothing else changed.

**2. The controls.** Map VIBE events straight to your functions: no imitated FM-1 keys, no layers. SHIFT is SHIFT.
Follow the house layout so every VIBE FM firmware feels the same (`AGENTS.md`, house rules):
- the six keys right of ENTER: **SOUND · ENV · LFO · ARP · FX · SEQ**, where it fits;
- encoders 1–4 are the four values shown above F1–F6;
- F1–F6 have their captions on the display (`vibe_label` for the light);
- HOME tapped shows the controls (`vg_controls()`, the FM-1's HOME function goes to RETURN); the library opens on HOME
  held 1 s or SHIFT+HOME – the host does it, never call `vibe_request(VIBE_REQ_FIRMWARE_MENU)` for HOME.

**3. The pages.** Draw every page fresh on 480 × 320 with `vg_*`: full width, big type, the page's colour in the header.
Draw everything, every time (the host may skip frames; the plugin with its window closed draws none). Keep it fast:
`wasmtest` reports `draw_max_ms`.

**4. FOCUS (SDK 10).** Every control on the display must be reachable with DATA + ENTER, so the VIBE plays from the
smallest controller. Usually ten lines: `vf_begin` … `vf_value` / `vf_encoder` / `vf_key` … `vf_end` in
`vibe_on_draw`, `vf_event` first in `vibe_on_event` (`AGENTS.md`, "SDK 10"; OberVave: `focus_items` in `main.c`).

**5. The DAW.** Declare your parameters (`vibe_params`, permanent `id`s: automation hangs on them) and groups
(`vibe_param_groups`: they become the plugin's Editor tabs and cards). Run your sequencer from the song position
(`vibe_transport()`) when the DAW plays, from your own tempo when not. A firmware with tracks reports its mixer
(SDK 11: `mix_<n>_level` …; OberVave: `o3_daw.h`).

**6. Storage.** Write your patches, patterns and settings with `vibe_store_*`. Keep the same formats and names as on the
FM-1 if you can: OberVave 3 still reads what OberVave 2 saved. Save a moment after a change, from `vibe_on_event` or
`vibe_on_draw`, never from the audio. In the plugin your files live inside the DAW project.

**7. Optional: your own Editor page.** The plugin's Editor is built from your parameters. For a look of your own:
`editor/` with HTML, CSS and JS ([EDITOR-PAGE.md](EDITOR-PAGE.md)).

## The traps

- **Your main loop did timing.** Anything that waited for milliseconds (blinking, auto-save, note lengths, debounce) now
  counts audio frames in `vibe_on_audio`, or checks `vibe_frames()` in `vibe_on_draw` for things the eye sees.
- **Sequencers belong in the audio.** Steps, arpeggios, envelopes, recording: in `vibe_on_audio`, sample-exact. Never
  in `vibe_on_draw`; it may not run at all.
- **44.1 kHz baked in.** Search for 44100, 22050, 1/44100, `0.0000226` and tables generated by your tools. OberVave's
  tables are now computed in `vibe_on_init` with the formulas of its old generator.
- **The block size changes.** `frames` is 1…256 and differs from call to call. No `assert(frames == 128)`.
- **Phases in radians.** `vd_sin` takes turns (0…1).
- **No libc.** `memset`, `memcpy`, `strlen`, `strcmp`, `strtol` and friends are there; `printf`, `malloc`, `<math.h>`
  are not. `vibe-check` names any forbidden import.
- **Heavy setup in the audio.** Wavetables and long tables: `vibe_on_init`.
- **Values with no home.** Encoders give detents (± and fractional), not absolute positions: your knob code adds them up.

## Before you share it

- `node tools/vibe-check.mjs build/<id>.wasm` ends with **PASSED**.
- Play it in the emulator (`python3 tools/emulator.py`) and in VIBE FM VST (drop the `.zip` on its window).
- An `icon.png` (16:9) next to `firmware.json`.
- **Licence:** your firmware stays yours, under your licence. If it is GPL-3.0, share the source with the `.wasm`.
- Want it on The Firmware Hub as a native firmware? Mail **zven@thefirmwarehub.com**.

Stuck on something the SDK cannot do? Open an **issue** in this repository: we build it into the host, and then every
firmware has it.

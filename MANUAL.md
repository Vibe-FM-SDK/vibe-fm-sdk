# VIBE FM – the manual

How to **play** the VIBE FM: in the emulator on your computer (`python3 tools/emulator.py`), in VIBE FM VST, and on
the hardware to come. For **building** firmwares, see `README.md` and `AGENTS.md`.

Status 11.11.2026 (SDK 26). Everything here runs in the emulator today. Things that only the hardware will have are
marked *(hardware)*.

---

## 1. The instrument in one minute

| | |
|---|---|
| **Display** | 4.0", 480 × 320, in colour. It always shows what the controls do right now. |
| **F1 – F6** | the soft keys under the display. Their captions are drawn on the display. |
| **K1 – K6** | six colour keys right of ENTER: the firmware's areas. The house style is **SOUND · ENV · LFO · ARP · FX · SEQ**. |
| **Encoders 1 – 4** | the four values shown on the display |
| **Encoders A · B** | the track and the sound, or whatever the firmware shows for them |
| **DATA** | the big wheel: turn it, press it (= ENTER) |
| **SHIFT · OCT − / + · ENTER · ↩ RETURN · HOME** | as named. SHIFT gives many controls a second job. |
| **◀ MENU ▶ · REC STOP PLAY** | MENU shows what every control does on the open page. |
| **16 pads · 25 keys** | velocity-sensitive. The pads light up in RGB. |
| **Rear** | IN 1 / IN 2 (mic, line, guitar), OUT L/R, PHONES with its own signal, PEDAL, CV/Gate in and out, CLK IN, MIDI IN/OUT (DIN), USB *(hardware)* |

A **firmware** is a whole instrument, for example OberVave, VIBE DJ, or what you build. The VIBE runs one at a time,
and you switch between them in the firmware library.

## 2. The emulator

```sh
python3 tools/emulator.py          # -> http://127.0.0.1:8766 (Chrome or Edge)
```

1. Choose a firmware in the top bar. **OBERVAVE 3.1** and **VIBE DJ** are always there, together with everything you built.
2. Click **Power on**. The browser only allows sound after a click.
3. Play with the mouse (drag or scroll the knobs), with your computer keyboard (table below), or with a MIDI controller
   (Web MIDI).

**Cheat sheet** (top bar) shows how to play the running firmware. **Setup** and **Learn** map any MIDI controller (see 8).
**Rear panel** opens the jacks: the audio input, the pedal and CV. **Media** picks a folder of samples and songs that a
firmware may read (SDK 13) – the first one you allow to be written to also gets the firmware's own files in
`VIBE FM/<firmware>/`. In Firefox or Safari drop a folder onto the device instead (read only). Shift + click forgets them.
On the VIBE the same comes from its card and from any USB stick you plug in; in the plugin from its media folder.

| Key | VIBE | Key | VIBE |
|---|---|---|---|
| Shift + Tab | firmware library | ↑ ↓ | DATA |
| Enter | ENTER | Esc · Tab | RETURN · HOME |
| F1 – F6 · F7 | soft keys · MENU | Page ↑ / ↓ | encoder B |
| Space · Backspace · O | PLAY · STOP · REC | ← → | OCT − / + |
| 1 – 8, Q – I | pads 1 – 16 | Y X C V B N M · S D G H J | keys: white · black |

Click into the device first, so that it gets the keys.

## 3. The firmware library

| | on the VIBE | MPK mini (DAW preset) | computer keyboard |
|---|---|---|---|
| open it (from any firmware) | **hold HOME 1 s**, or **SHIFT + HOME** | **SHIFT + Undo** | Shift + Tab |
| choose | turn DATA | turn K5 | ↑ ↓ |
| start | ENTER | press the wheel | Enter |
| back | RETURN | Undo | Esc |

Each firmware keeps its own saved sounds, patterns and settings.

## 4. FOCUS – everything with one knob and one button (SDK 10)

Not every controller has 6 encoders and 12 keys. With FOCUS, an **orange frame** walks over every control on the
display. That makes one wheel and one button enough to reach everything, for example an MPK mini's K5 and its wheel
press.

- **Turn DATA**: the frame moves to the next control.
- **ENTER** (or press DATA):
  - on a button, it presses it;
  - on a value, it starts editing it (a small ◀ arrow shows). DATA then changes the value, and ENTER or ↩ ends editing.

Firmwares use FOCUS in one of two ways:

| | how FOCUS starts | used by |
|---|---|---|
| **always on** | DATA has no other job here, so turning it moves the frame straight away | VIBE DJ's DECKS |
| **on hold** | **hold ENTER (or the DATA wheel) for 0.6 s**. Hold it again, or press ↩, to end. Until then, DATA and ENTER do the firmware's own jobs (choose a sound, a pattern …), and a short ENTER is still the firmware's. | OberVave |

The keys, pads and encoders keep doing what they always did. FOCUS is an extra way in, not a replacement.
For developers: `vf_*` in `sdk/include/vibe_gfx.h` takes about ten lines in your firmware (see `AGENTS.md`).

## 5. OberVave 3.1

A 4-track analog-style poly synth: arp, sequencer, mixer with drive and master. It is also the reference firmware: the
full source is in `firmwares/obervave-src/` (GPL-3.0). `sh tools/build.sh firmwares/obervave-src` builds it.

**Colour keys K1 – K6 = the areas.** Press a key again to step through its pages:

| | |
|---|---|
| **SOUND** | OSC 1 · OSC 2 · OSC MOD · FILTER · FILTER + |
| **ENV** | filter envelope · amp envelope |
| **LFO** | shape, rate · destinations |
| **ARP** | mode · division · octaves · gate |
| **FX** | chorus · delay · reverb · drive. MIX is the return level. |
| **SEQ** | TRACKS → STEPS → TRACK → PATTERNS |

**Encoders 1 – 4** = the four values on the page. **A** = track A – D. **B** = the track's sound. **DATA** = the sound
(MIXER / TRACKS: the next pattern, SHIFT: tempo).

**F keys:**

- **F1 MIXER:** press again for LEVEL → PAN → sends → DRIVE → MASTER.
- **F2 TRACKS:** all four tracks.
- **F3 / F4:** previous / next sound.
- **F5 SAVE:** SHIFT gives HOLD.
- **F6 GLOBAL:** tempo, click, MIDI channel, tune. Here, ENTER switches sync and DATA switches MIDI OUT. SHIFT gives UNDO.

**Pads, in columns = tracks A – D:**

- **Row 1:** start / stop. It waits for the bar and blinks while it waits. With REC lit, row 1 records instead.
- **Row 2:** select the track.
- **Rows 3 – 4:** notes.

**Transport:**

- **PLAY:** start / stop.
- **REC:** record. Hold it for 1 s, or press SHIFT + REC, to clear the track.
- **MENU:** an overview of every control.
- **HOME:** what every control does on this page (MENU too) · **↩** the performance view. *Hold* HOME 1 s (or SHIFT+HOME) to open the firmware library.

**FOCUS:** hold ENTER for 0.6 s. The frame then walks over the area name, the track, the sound, the four values and F1 – F6.

Programs 65 – 70 are drum and keys sounds: KICK, SNARE, HIHAT, CLAP, E-PIANO, WURLI. Eight patterns. Edits made while
playing are kept and saved at the next stop. The full cheat sheet is under **Cheat sheet** in the emulator.

## 6. VIBE DJ

An MP3 / WAV player with two DJ decks (called PLAYER until 1.1). In the emulator it comes with three demo tracks (118, 124 and 128 BPM, made
with our own groovebox). You can also drop your own files or folders onto the device. Everything stays in your browser.

**The list:**

- **DATA** chooses, **ENTER** plays.
- **F1 LOAD A** and **F2 LOAD B** put the selected track on a deck.
- **F6 DECKS** (or **K6**) opens the decks.
- The number on the right of a track is its BPM, once it has been on a deck.

**DECKS:** the two waveforms run upward side by side in the middle, with the decks outside. Tempo and beats are measured
when a track loads, and kept: loaded again, the track has its BPM, beats and waveform at once.

| | |
|---|---|
| F1 / F6 | PLAY A / B (SHIFT: back to the start) |
| F2 / F5 | PHONES A / B: the deck in your headphones |
| F3 / F4 | SYNC A / B: this deck follows the other one's tempo and beat |
| Encoders 1 / 4 | volume A / B (SHIFT: the DJ filter – left low-pass, right high-pass) |
| Encoders 2 / 3 | pitch A / B, ±25 % (SHIFT: HIGH) |
| A / B | crossfader / cue ⇄ master in the headphones (SHIFT: MID) |
| Pads 1 – 4 / 5 – 8 | A / B: PLAY, PHONES, SYNC, to the start |
| Pads 9 – 12 / 13 – 16 | A / B: CUE 1, CUE 2 (empty: set on the beat; set: jump; SHIFT: clear), LOOP 4, LOOP 8 beats |
| OCT − / + · ◀ ▶ (held) | bend A · B by ±4 % |
| K3 | AUTO VOL: every track equally loud |
| FOCUS (always on) | turn DATA, then press ENTER. Framing a deck's title and pressing ENTER opens the list; ENTER there loads the track into that deck. |

**Headphones:** VIBE FM VST plays the cue on its second output, "Cue (headphones)". With an interface that has only two
outputs, the DJ switches to **split**: OUT L carries the master and OUT R the cue, both in mono.

## 7. In your DAW – VIBE FM VST

The same `.wasm` files run as a VST3 / AU / CLAP plugin (free: <https://thefirmwarehub.com/convert>). To load one,
drop `build/<id>.zip` onto the plugin window. The plugin window shows the device. Its **Editor** view lays out the
firmware's parameters for the mouse, and DAW automation works on them. VIBE FM VST 0.4 also plays the firmware's
MIDI out and its Cue output.

## 8. Controllers

**MPK mini IV / MK3:** recognised by name. Choose its **DAW preset**, because only that preset sends the buttons.

| MPK | VIBE | MPK | VIBE |
|---|---|---|---|
| keys | notes with velocity | pads (bank A / B) | pads 1 – 8 / 9 – 16 |
| K1 – K4 | encoders 1 – 4 | K5 · turn the wheel | DATA |
| K6 | master volume | K7 / K8 | encoders A / B |
| press the wheel · Overdub | ENTER (hold it for FOCUS) | Undo | HOME (SHIFT + Undo: the firmware library) |
| Stop/Play · Loop · Record | PLAY · STOP · REC | SHIFT | SHIFT |
| − / + | through the areas | TAP TEMPO | the same area again |

**Controller profiles:** what a controller does on the VIBE is a small JSON file in `controllers/` (`CONTROLLERS.md`).
**Setup** walks you through yours, and **Profile ↓** saves it as such a file. It then works the same on the VIBE.

**Mixer controllers** (Mackie Control): faders, MUTE / SOLO / SELECT and the LEDs work the tracks of OberVave and of
every firmware that reports its mixer (SDK 11).

**Any other MIDI controller:**

- Keys play notes, and channel 10 plays the pads.
- To map knobs and buttons, use **Setup** (a guided walk: "turn the knob for encoder 1 …") or **Learn** (click a control
  on the device, then move yours). The mapping is kept in the browser.
- On the hardware, the same setup will live under SETTINGS → CONTROLLER *(hardware)*.

With FOCUS, a controller that has **one encoder with a push** already reaches every control in OberVave and
VIBE DJ. Map it to DATA and ENTER.

## 9. When something is wrong

| | |
|---|---|
| no sound | Click **Power on**. Check the **Audio** volume on the left of the device (VOLUME). Use Chrome or Edge. |
| the controller does nothing | Use Chrome or Edge, allow MIDI, and make sure no other program holds the port (a DAW). MPK: use the **DAW preset**. |
| a firmware hangs or is silent | Open the library (Shift + Tab) and start it again. Then run `node tools/vibe-check.mjs build/<id>.wasm`, which shows what a firmware does wrong. |
| clicks while loading or switching | That firmware does too much work in `vibe_on_draw`. `tools/wasmtest.mjs` reports `draw_max_ms`, which should stay low (a few ms). |

Questions, bugs, wishes: open an **issue** here, or mail **zven@thefirmwarehub.com**.

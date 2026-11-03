# Controllers and mixers

Two short standards. With them, every VIBE FM firmware works with every controller, and nobody writes controller code
into a firmware.


**Built in (no profile needed):** the Novation **Launchpad Mini MK3**, in the emulator and on the VIBE FM (since SDK 20's
host, 2.11.2026): it switches to programmer mode by itself and is a whole VIBE – upper left 4 × 4 the pads, upper right
PLAY STOP REC HOME, K1–K6, F1 F2, OCT − / +, ◀ ▶, the lower four rows two octaves of keys, the round buttons F1–F6, MENU,
RETURN, SHIFT, ENTER and DATA – and shows the firmware's lights. Other grids (a Launchpad Pro, a Push) use a profile with
`"pads": { "notes": [...] }`.

## 1. Controller profiles (for the host, not your firmware)

A **profile** is a small JSON file that says what a MIDI controller's knobs, buttons and pads are on the VIBE. The host
applies it (the VIBE, the browser emulator), before your firmware sees anything, so your firmware just gets encoders,
buttons and pads as always. The same file works everywhere. Profiles live in `controllers/`.

```json
{
  "format": 1,
  "name": "My Controller",
  "match": ["My Controller"],
  "knobs": "auto",
  "pads": { "channel": 10, "note": 36, "rows": "top-down" },
  "map": [
    { "cc": 21, "to": "encoder", "id": "1" },
    { "cc": 28, "to": "encoder", "id": "DATA" },
    { "cc": 29, "to": "volume" },
    { "note": 48, "channel": 1, "to": "button", "id": "ENTER" },
    { "note": 49, "channel": 1, "to": "library" }
  ]
}
```

| Field | Meaning |
|---|---|
| `match` | part of the MIDI port's name (any case) |
| `knobs` | `auto` (the host finds out), `abs`, `rel64`, `rel2c`, `relsm` (Mackie V-pots) |
| `map[].knobs` | (VIBE FM VST 0.7.3, the emulator, vibe-host) the same, for this one control – before the profile's; what it says is never guessed. For endless encoders the guessing gets wrong: the MPK mini's DATA wheel (CC 14: `1` / `127`, fast `2`–`3` / `125`–`126`) is `rel2c` |
| `pads` | the drum pads as a block of notes: `channel`, first `note`, `rows` `top-down` or `bottom-up` – or (SDK 20) `notes`: each pad's note in turn, for a grid that does not count straight (a Launchpad in programmer mode: `[81, 82, … 88, 71, … 18]`). How many pads there are, the device says (`device.json`, else 16) |
| `map[]` | source: `cc` or `note` (optionally `channel` 1–16 and `port`, part of a port name). `to`: `encoder` (`1`–`4`, `A`, `B`, `DATA`), `button` (`F1`…`F6`, `K1`…`K6`, `SHIFT`, `OCT_DOWN`, `OCT_UP`, `ENTER`, `RETURN`, `HOME`, `LEFT`, `MENU`, `RIGHT`, `REC`, `STOP`, `PLAY`, `DATA_PUSH`), `pad` (1–16), `volume`, `area` (`dir` −1 / +1 / 0), `library` (the firmware library), `mix` (below) |

**Make one without typing:** in the emulator, plug your controller in, press **Setup** (it walks you through it), then
**Profile ↓** saves the file. Put it into `controllers/`. Send it to us (an issue or a pull request), and the controller
works for everyone.

**Tip:** with FOCUS (SDK 10), one knob with a push (`DATA` + `ENTER`) and a `HOME` button already reach every control in
every firmware.

## 2. The mixer convention (SDK 11, for your firmware)

If your firmware has tracks, report its mixer as **parameters with these ids**. Any mixer controller then works your
tracks (Mackie Control devices over USB, and later Bluetooth), and in the plugin the DAW does. You don't write any
controller code.

| id | range |
|---|---|
| `mix_<n>_level` | 0 … 1 (n = 1 … 16) |
| `mix_<n>_pan` | −1 … +1 |
| `mix_<n>_mute`, `mix_<n>_solo`, `mix_<n>_arm` | 0 / 1 (`steps` 2) |
| `mix_select` | 1 … N (`steps` N): the selected track |

- Use `vibe_params` as for any parameter (`sdk/include/vibe.h`). A change from outside arrives as `VIBE_EV_PARAM`.
- When your own panel changes one, call `vibe_param_set`. The simplest way is to compare once per audio block and report
  what differs. Then the controller's LEDs and faders follow.
- The host does the rest: faders pick the value up (no jumps), buttons toggle mute / solo / arm or select, the LEDs show
  your values, and the controller's first four knobs are encoders 1–4.
- A complete example: `firmwares/obervave-src/o3_daw.h` (`mix_params`, `mix_of`, `mix_from_host`).

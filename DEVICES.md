# DEVICES.md – one firmware, more than one device (SDK 19)

A VIBE FM firmware runs on the VIBE FM, in VIBE FM VST and in the browser. From SDK 19 it can also run on other
**VIBE FM compatible devices**, which may have more keys, more knobs, faders or a bigger display. This page covers two
things: what a firmware may ask for, and what a device has to answer.

## The base: "VIBE FM original"

Every firmware must be **fully playable** with the base. Every VIBE FM compatible device offers it, either directly or
through a mapping that its host does (like a controller profile, `CONTROLLERS.md`):

| | |
|---|---|
| keys | 25 (`VIBE_EV_KEY`, id 0..24) |
| pads | 16 (`VIBE_EV_PAD`, id 0..15) |
| encoders | `VE_1`..`VE_4`, `VE_A`, `VE_B`, `VE_DATA` (with push) |
| buttons | F1–F6, K1–K6, SHIFT, OCT −/+, ENTER, RETURN, HOME, ◀ MENU ▶, REC STOP PLAY |
| display | 480 × 320 |

Today the VIBE, the plugin and the emulator are all the base. They do not answer the question below, and the SDK then
reports the base, so nothing changes for them.

## For firmware authors: `vibe_device()`

Ask once, in `vibe_on_init`. The answer does not change while the firmware runs.

```c
static vibe_device_info dev;

void vibe_on_init(float sr) {
  vibe_device(&dev);              // 1 = the host described its device, 0 = the base (dev is filled in either way)
  /* … */
}
```

| field | range | what it means |
|---|---|---|
| `keys` | 25–61 | `VIBE_EV_KEY` ids 0 … keys − 1 (0 = the lowest C) |
| `pads` | 16–32 | `VIBE_EV_PAD` ids 0 … pads − 1 |
| `encoders_extra` | 0–8 | more encoders: `VIBE_EV_ENCODER` with ids `VE_X1` (16) … `VE_X1 + 7` |
| `faders` | 0–16 | `VIBE_EV_FADER` (17), id 0 …, value = the position 0..1 (absolute) |
| `buttons_extra` | 0–16 | more buttons: `VIBE_EV_BUTTON` with ids `VB_X1` (64) … `VB_X1 + 15` |
| `touch` | 0 / 1 | 1 = the display takes clicks (`VIBE_EV_TOUCH`, SDK 18) |
| `display_w`, `display_h` | ≥ 480 × 320 | the device's display |
| `name` | | e.g. "VIBE FM" |

The rules:

1. **Extras are shortcuts, never the only way.** Eight extra encoders may put a whole page under your fingers, but every
   function stays reachable with the base controls. `vibe-check` and the VIBE run your firmware with the base.
2. **Ignore what you do not know.** An event with an id you do not use (an extra button, a 26th key) does nothing.
3. **The picture stays 480 × 320.** A bigger display shows it scaled; your `vibe_on_draw` does not change.
4. **No new import.** `vibe_device()` uses `vibe_host_store` (op 14), which every host already has. A firmware built with
   SDK 19 therefore loads in every VIBE FM VST and on every VIBE, old ones included.

The examples do not use it yet, because there is no second device yet. A firmware that does not call `vibe_device()`
works on every VIBE FM compatible device unchanged.

## For device makers: answering the question

`vibe_device()` calls `vibe_host_store(14, "", 0, data, 52)`. A host that describes its device fills the 52 bytes and
returns **1**. A host that does not returns **−1**, as for any op it does not know, and the base applies.
The firmware asks only from its control flow (`vibe_on_init`), never from its audio.

| byte | field | range |
|---|---|---|
| 0 | `keys` | 25–61 |
| 1 | `pads` | 16–32 |
| 2 | `encoders_extra` | 0–8 |
| 3 | `faders` | 0–16 |
| 4 | `buttons_extra` | 0–16 |
| 5 | `touch` | 0 / 1. Send clicks only to native firmwares, never to converted FM-1 firmwares. |
| 6–15 | reserved | 0 |
| 16–17 | `display_w` (u16, little-endian) | ≥ 480 |
| 18–19 | `display_h` (u16, little-endian) | ≥ 320 |
| 20–51 | `name` | UTF-8, ending with 0 |

The SDK throws away an answer that falls below the base (fewer than 25 keys or 16 pads, a display under 480 × 320, an
extra count above its range) and reports the base instead. A device that has less than the base must map what is missing
in its host, for example a shift layer for the pads. The firmware never learns about that mapping.

A device that wants the name "VIBE FM compatible" runs the VIBE FM host, offers the whole base (directly or mapped), shows
480 × 320 (scaled is fine) and plays the reference firmwares (OberVave) as the VIBE does. If you are building one, write
to **zven@thefirmwarehub.com**. The layout file for a device (what it has, what it maps, the face the plugin shows) is
not in this repository yet. We will define it with the first device maker.

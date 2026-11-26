# UI-GUIDE.md – how a Vibe plays

Every Vibe is your instrument, and you decide what it does. Players, though, pick up a new Vibe in seconds only if the
basics work the same everywhere: where the track and the sound are chosen, where the sequencer lives, how to get back.
This guide is that common ground. It is a set of **recommendations, not rules**: none of it is required to be on the hub,
and your instrument may need something else. Follow it where it fits.

A good example of all of this is [KRACHT](https://sponde.de/kracht-vibe/#why): one start page, the sequencer on K6, a
way through every step without pads, and a 60-second video.

## The common ground – what we'd love every Vibe to do

1. **The start page is the playing page.** It shows the sound's name, big, and the four values of encoders 1–4.
   **RETURN** always brings you back to it.
2. **A chooses the track, B its sound, DATA the pattern** – as in OberVave, on every page. A steps through the tracks
   (or parts), B through the sounds of that track (presets, kits, programs), DATA through the patterns (the sequences).
   Pressing DATA chooses or confirms. In a list (files, songs), DATA moves through it and pressing opens.
3. **HOME, tapped, shows what every control does on this page** (`vg_controls`). Held for 1 s it opens the library,
   which belongs to the host.
4. **F1–F6 always carry a caption** on the display (`vibe_label`, at most 7 characters), right above the keys.
5. **REC · STOP · PLAY are the transport, and − / + are the octave** (`VB_OCT_DOWN` / `VB_OCT_UP`).
   **SHIFT + K1–K6 play notes**, like a small keyboard: the host's PLAY KEYS, the VIBE's key and scale. Your Vibe gets
   them as notes, so leave them free (or turn them off with `vibe_play_keys(0)` when K1–K6 must play something else).
6. **If your Vibe has a sequencer, it lives on K6 "SEQ"**, and it works without pads:
   - **A** chooses the step.
   - **B** sets its value (off / hit / accent, or the note).
   - **DATA pressed** switches the step on or off.
   - Encoders 1–4 edit that step's values (note, velocity, length …).
7. **Everything works without pads and keys.** The [VIBE Box](HARDWARE.md) has neither. Notes come in over MIDI, and
   every page, step and mute is reachable with the encoders and buttons. The pads are a faster way, never the only way.

## The house style

- **K1–K6: SOUND · ENV · LFO · ARP · FX · SEQ**, where your instrument has these areas. Draw the page name on the
  display (the keys carry no print).
- **Encoders 1–4 in the same order as the values on the display**, left to right, on every page.
- **SHIFT gives a second job.** Keep it the same across pages (SHIFT + − / + for the program, for example).
  SHIFT + K1–K6 and SHIFT + HOME belong to the host.

## Touch for free

The VIBE Box's display is a touch screen, and the web player and VIBE FM VST plugin take clicks and drags on it too.
They can only do the right thing if they know where things are:

- **Draw the house layout and it works by itself.** Soft keys go in the strip at the bottom (y 282–320, six fields of
  80 px). The four values go in four columns side by side above them (x 10–470, 115 px each). A click on a soft key
  presses it, and a drag in a column turns that encoder.
- **With FOCUS, add `vf_clicks(1)`** in `vibe_on_init`. Your `vf_button`s become clickable and your `vf_value`s
  draggable. There is nothing else to write.
- **Your own zones (a step grid, circles, a mixer)** come as `VIBE_EV_TOUCH` with x and y. A tap on a step can switch
  it on or off, which is the most natural thing on a screen.

A Vibe that draws its values one under another, or a grid, and does neither of these, gets clicks in the wrong place.
People then think touch is broken.

## Your Vibe in 60 seconds

A short video does more than any manual: **start · the first sound · the sequencer · one performance trick · without pads
(the Box)**. One minute, the screen and the sound, no talking needed. Put its link in your
[fmhub.json](https://thefirmwarehub.com/kb/#fmhub) as `"video"` (an MP4 in your repository, or YouTube). The hub shows
it on your Vibe's page.

## A quick check before you submit (optional)

- [ ] The start page shows the sound and the four values, and RETURN leads there.
- [ ] A the track, B the sound, DATA the pattern. HOME shows the controls. F1–F6 have captions. SHIFT + K1–K6 play notes.
- [ ] The sequencer (if any) is on K6 and works without pads.
- [ ] Every page is reachable without pads and keys.
- [ ] Clicks land where things are drawn (house layout, `vf_clicks(1)` or `VIBE_EV_TOUCH`).
- [ ] `vibe-check` passes.
- [ ] A 60-second video, linked in fmhub.json (recommended).

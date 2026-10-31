/* SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE. */
/*
 * vibe.h – the VIBE FM SDK: the one contract between a firmware and the hardware it runs on.
 *
 * A firmware is plain C. It implements the four vibe_on_* functions below and may call the vibe_* functions
 * the SDK provides. It never talks to hardware directly. The same .wasm therefore runs in VIBE FM VST (your DAW),
 * in the browser and on the VIBE FM hardware.
 *
 * Threading: the host calls every vibe_on_* function from ONE thread, one after the other
 * (events, then an audio block, now and then a draw). A firmware needs no locks.
 * Sample rate and block size are the host's: vibe_on_init gets the rate (the DAW's – 44.1, 48, 96 kHz …), and
 * `frames` in vibe_on_audio varies from call to call (1..VIBE_MAX_BLOCK). Never assume 128 or 48000.
 * SDK 5: a firmware may allow vibe_on_audio to run IN PARALLEL with events and draw (VIBE_PARALLEL_AUDIO below) –
 * then the display never holds up the sound. Its rules are in the VIBE FM Protocol specification (coming to this
 * repository in English); leave it off until then.
 */
#ifndef VIBE_H
#define VIBE_H

#include <stdint.h>
#include <stddef.h>

#define VIBE_SDK_VERSION 16       /* 2 (16.10.2026): IN 1/IN 2, PEDAL, CV/Gate, CLK IN; no LED rings.
                                      3 (20.10.2026): storage (vibe_store_*).
                                      4 (21.10.2026): vibe_store_list, vibe_store_read_at (big files, e.g. music).
                                      5 (21.10.2026): time (vibe_frames, vibe_event_heard, latency), two flows
                                      (VIBE_PARALLEL_AUDIO).
                                      6 (22.10.2026): one file for every host – the memory is shared (built with
                                      atomics), so the VIBE and the plugin can run the audio in its own flow.
                                      7 (22.10.2026): VIBE_EV_TRANSPORT – a DAW's play / stop and tempo.
                                      8 (26.10.2026): notes from every source (VIBE_EV_NOTE), parameters (vibe_params).
                                      9 (28.10.2026): MIDI out and clock out (vibe_midi_send, vibe_clock_out), the Editor
                                      described by the firmware (vibe_editor_*). Optional exports again: nothing new
                                      from a host, firmwares for 1-8 still run, SDK-9 firmwares run on SDK-8 hosts.
                                      10 (28.10.2026): FOCUS (vibe_gfx.h, vf_*) – every control on the display reachable
                                      with one knob and one button (DATA + ENTER), the standard for every firmware. Inside
                                      the firmware only: nothing new for a host.
                                      11 (28.10.2026): the mixer convention – mix_<n>_level / pan / mute / solo / arm and
                                      mix_select as parameters (Protokoll 7.2a): any mixer controller works the tracks.
                                      12 (28.10.2026): a stream from the host (vibe_stream: Spotify Connect on the VIBE),
                                      vibe_areas (a controller's BANK − / +), SHIFT + DATA = the soft keys F1-F6 in every
                                      firmware (the SDK takes SHIFT + DATA; a firmware no longer gets it: when the frame
                                      appears it gets SHIFT let go with value -1 – not a tap, if SHIFT tapped means something).
                                      13 (29.10.2026): media from outside (vibe_media_*): samples and songs on the VIBE's
                                      card, a USB stick, a folder the browser or the plugin was given – read in pieces,
                                      written only into "VIBE FM/<firmware>/". New ops of vibe_host_store, no new import:
                                      an SDK-13 firmware still runs on every host (an older one answers -1).
                                      14 (30.10.2026): files dropped onto the plugin's window (vibe_media_dropped, op 13):
                                      a sample from the desktop or the DAW onto a pad – nothing new for older hosts (-1).
                                      15 (31.10.2026): the Editor talks back – a curve you draw (vibe_editor_curve_draw),
                                      buttons on a card (vibe_editor_button, VIBE_EV_EDITOR) – and SysEx in
                                      (VIBE_EV_SYSEX, vibe_sysex_in). Optional exports again: an older host shows no
                                      buttons and sends no SysEx; the firmware still runs.
                                      16 (31.10.2026): the firmware's own Editor page (editor/, EDITOR-PAGE.md) and bytes
                                      between it and the firmware (vibe_editor_post, VIBE_EV_EDITOR_DATA). */
#define VIBE_HOST_PROTOCOL 7       /* what a host must speak to run a firmware of this SDK – reported by vibe_sdk_version().
                                      SDK 8's additions (notes, parameters) need nothing new from a host: the notes are made
                                      inside the firmware, the parameters are optional exports a host finds by name. So an
                                      SDK-8 firmware runs on every SDK-7 host (the VIBE FM VST 0.1.0 out there), which just
                                      does not offer its parameters. (26.10.2026) */

/* ---- the device ---------------------------------------------------------------------------------------------- */
#define VIBE_SCREEN_W   480        /* 4.0" colour display, RGB565 */
#define VIBE_SCREEN_H   320
#define VIBE_MAX_BLOCK  256        /* the host asks for at most this many audio frames per call – usually fewer, and
                                      not always the same number (the browser: 128, a DAW: its buffer, split at events) */
#define VIBE_PADS       16
#define VIBE_KEYS       25         /* key 0 = lowest C; octave shifting is the firmware's business */

/* buttons (event id for VIBE_EV_BUTTON, and LED id for vibe_led_button) */
enum vibe_button {
  VB_F1, VB_F2, VB_F3, VB_F4, VB_F5, VB_F6,                 /* soft keys under the display */
  VB_SHIFT, VB_OCT_DOWN, VB_OCT_UP, VB_ENTER,
  VB_K1, VB_K2, VB_K3, VB_K4, VB_K5, VB_K6,                 /* six free keys right of ENTER */
  VB_RETURN, VB_HOME,
  VB_LEFT, VB_MENU, VB_RIGHT,
  VB_REC, VB_STOP, VB_PLAY,
  VB_DATA_PUSH,
  VB_COUNT
};

/* endless encoders (event id for VIBE_EV_ENCODER). No LED rings: values belong on the display. */
enum vibe_encoder { VE_1, VE_2, VE_3, VE_4, VE_A, VE_B, VE_DATA, VE_COUNT };

typedef enum {
  VIBE_EV_PAD = 1,      /* id 0..15,  down 1/0, value = velocity 0..1 (on release: 0) */
  VIBE_EV_KEY = 2,      /* id 0..24,  down 1/0, value = velocity 0..1 */
  VIBE_EV_BUTTON = 3,   /* id enum vibe_button, down 1/0 */
  VIBE_EV_ENCODER = 4,  /* id enum vibe_encoder, value = turn in detents (+ clockwise), may be fractional */
  VIBE_EV_MIDI = 5,     /* midi[0..2] = raw MIDI message from USB */
  VIBE_EV_PEDAL = 6,    /* PEDAL jack (6.3 mm TRS, detected automatically): id VIBE_PEDAL_SWITCH: down 1/0,
                           id VIBE_PEDAL_EXPR: value 0..1 (heel .. toe) */
  VIBE_EV_CLOCK = 7,    /* CLK IN: one pulse (rising edge), value = seconds since the previous pulse (0 = first) */
  VIBE_EV_NOTE = 9,     /* SDK 8, after vibe_notes(): a note from any source – id = note 0..127, down 1/0,
                           value = velocity 0..1, midi[0] = channel 0..15, midi[1] = source (enum vibe_note_source) */
  VIBE_EV_PARAM = 10,   /* SDK 8: a parameter set from outside (the DAW's automation, a mapped controller) –
                           id = index into the vibe_params() list, value = its new value (min..max) */
  VIBE_EV_AREA = 11,    /* SDK 12, after vibe_areas(n): a controller's "area" buttons (an MPK's BANK − / +, TAP):
                           value = -1 previous area, +1 next, 0 the same once more. Without vibe_areas the host steps
                           through K1-K6, F1, F5, F6, MENU as before (OberVave, GROOVE). */
  VIBE_EV_MEDIA = 12,   /* SDK 13, once the firmware asked vibe_media_sources: id VIBE_MEDIA_EV_SOURCES – a source came
                           (down 1) or went (down 0: a stick pulled; value = how many there are now); id VIBE_MEDIA_EV_WRITTEN –
                           a file written with vibe_media_create is complete (down 1) or failed (down 0), value = its handle;
                           id VIBE_MEDIA_EV_READY – something the host said VIBE_MEDIA_PENDING for has come: ask again
                           (sent once for all that came; a host that stops drawing – the plugin with its window closed –
                           still sends it, so a firmware that reads on READY never stalls); SDK 14: id VIBE_MEDIA_EV_DROPPED –
                           a file was dropped onto the host's window: take it with vibe_media_dropped (value = how many wait) */
  VIBE_EV_EDITOR = 13,  /* SDK 15: a button of yours in the Editor was clicked (vibe_editor_button): id = your id, down 1;
                           vibe_editor_event_tab() says on which tab */
  VIBE_EV_SYSEX = 14,   /* SDK 15: a SysEx message came in (F0 … F7, a hardware's dump): value = its length in bytes,
                           the bytes in vibe_sysex_in() – valid during this event only */
  VIBE_EV_EDITOR_DATA = 15,   /* SDK 16: bytes from your own Editor page (vibe.send in its JavaScript): value = their
                           length, the bytes in vibe_editor_data() – valid during this event only */
  VIBE_EV_TRANSPORT = 8 /* SDK 7: the host's transport (the DAW in the plugin; a MIDI clock on the device and in the browser): id VIBE_TRANSPORT_PLAY: down 1 = it plays
                           (from the top, at this frame), 0 = it stopped; id VIBE_TRANSPORT_TEMPO: value = BPM (0 = the host
                           gives none any more: the firmware's own tempo again). Sent when it changes, tempo before play. */
} vibe_event_type;

enum vibe_transport { VIBE_TRANSPORT_PLAY, VIBE_TRANSPORT_TEMPO };   /* ids of VIBE_EV_TRANSPORT (SDK 7) */
enum vibe_pedal { VIBE_PEDAL_SWITCH, VIBE_PEDAL_EXPR };
enum vibe_note_source { VIBE_NOTE_KEYS, VIBE_NOTE_PADS, VIBE_NOTE_MIDI, VIBE_NOTE_HOST };   /* SDK 8 */

typedef struct {
  uint8_t type;         /* vibe_event_type */
  uint8_t id;
  uint8_t down;
  uint8_t midi[3];
  float value;
} vibe_event;

/* ---- what a firmware implements -------------------------------------------------------------------------------- */
void vibe_on_init(float sample_rate);                                   /* once, before anything else */
void vibe_on_event(const vibe_event *e);                                /* a control moved */
void vibe_on_audio(const float *in, float *out_l, float *out_r, int frames); /* in = IN 1, NULL while off
                                                                           (IN 2: vibe_input(1)) */
void vibe_on_draw(uint16_t *fb);                                        /* paint the whole display, RGB565 */

/* ---- what the SDK gives a firmware ------------------------------------------------------------------------------ */
/* lights: colours are 0xRRGGBB, level 0..1 (0 = off) */
void vibe_led_pad(int pad, uint32_t rgb, float level);
void vibe_led_button(int button, uint32_t rgb, float level);
/* a caption for a button (the emulator writes it on the key; later the panel's small displays): up to 7 characters,
   "" = none. Meant for the soft keys F1-F6 and the six free keys right of ENTER. (SDK 2, 18.10.2026) */
void vibe_label(int button, const char *text);
void vibe_led_ring(int encoder, float value, int bipolar);              /* obsolete: the device has no LED rings
                                                                           (15.10.2026). Does nothing, kept so
                                                                           SDK-1 firmwares still build. */

/* ---- notes (SDK 8): one way for the keys, the pads, MIDI and the host's notes -------------------------------------
   vibe_notes(VIBE_NOTES) in vibe_on_init: from then on the firmware gets VIBE_EV_NOTE instead of key events and MIDI
   note on/off (other MIDI – bend, aftertouch, CCs – still comes as VIBE_EV_MIDI). The 25 keys become notes where the
   keyboard sits: key 0 = C of octave vibe_keys_octave() (36 + 12 x octave: default 1 = note 48, C3); OCT-/OCT+ move it
   (then they reach the firmware as buttons too). VIBE_NOTES_OWN_OCTAVE: the firmware moves the keyboard itself.
   VIBE_NOTES_PADS: the pads become notes too (36 + pad: GM drums), else they stay pads. A note keeps its number until
   it is released, even if the octave moved while it was held. */
#define VIBE_NOTES            1
#define VIBE_NOTES_OWN_OCTAVE 2
#define VIBE_NOTES_PADS       4
void vibe_notes(int flags);                                            /* 0 = off (SDK 7 behaviour) */
void vibe_keys_octave(int octave);                                     /* 0..7 */
int  vibe_keys_octave_get(void);

/* ---- parameters (SDK 8): what the DAW automates and a controller maps --------------------------------------------
   Declared once in vibe_on_init. The id is the parameter's name for ever (the DAW's automation hangs on it: never
   rename one); name and unit are shown. Values are in the parameter's own range (min..max). A change from outside comes
   as VIBE_EV_PARAM; when the firmware changes one itself (its knob), vibe_param_set tells the host (the DAW records it). */
#define VIBE_MAX_PARAMS 256        /* (27.10.: 256 – OberVave 3 lists every sound parameter of its four tracks) */
typedef struct {
  const char *id;                     /* stable, a-z 0-9 _: "cutoff" */
  const char *name;                   /* shown: "Cutoff" */
  float min, max, def;
  const char *unit;                   /* "Hz", "%", "ms", "" */
  int steps;                          /* 0 = continuous, n = n choices (min..max in n steps) */
  const char *const *choices;         /* names of the steps, or 0 */
} vibe_param;
int   vibe_params(const vibe_param *list, int count);                  /* the list stays the firmware's (static); returns count */
void  vibe_param_groups(const char *const *groups);                   /* optional: a group per parameter ("A · Filter") – the
                                                                          plugin's Editor view shows each group together */
void  vibe_param_set(int index, float value);                          /* the firmware changed it */
float vibe_param_get(int index);                                       /* its current value */

/* ---- MIDI out (SDK 9) -------------------------------------------------------------------------------------------
   To the device's USB-MIDI and its MIDI OUT (DIN), the plugin's note/MIDI output (a DAW track), the browser's Web MIDI.
   1-3 bytes a message, no running status. Safe from vibe_on_audio – it is the main case: a message is stamped with a
   frame of the block being computed (vibe_midi_send: its first frame; _at: the frame you are at – sample-exact).
   From vibe_on_event / vibe_on_draw: the next block's first frame. Returns 0 = queued, -1 = full (256 messages a block
   is the limit: the firmware sends too much, the message is dropped and counted) or bad bytes (status < 0x80, data >= 0x80).
   No thru in the host: what comes in goes out only if the firmware sends it on (VIBE_EV_MIDI → vibe_midi_send).
   With VIBE_PARALLEL_AUDIO: send from one flow only (the queue has one writer). */
int  vibe_midi_send(uint8_t status, uint8_t d1, uint8_t d2);
int  vibe_midi_send_at(uint8_t status, uint8_t d1, uint8_t d2, int frame);   /* frame 0..frames-1 of this block */
int  vibe_midi_send_sysex(const uint8_t *bytes, int n);                     /* F0 … F7, up to 256 bytes; not from vibe_on_audio */

/* the firmware as the clock: 24 PPQ MIDI clock (F8) at its own tempo, sample-exact, made by the SDK inside each audio
   block – the firmware only switches it on and keeps vibe_tempo_set current. START (FA) / STOP (FC) with
   vibe_transport_out; START also lines the clock up (the first F8 on that frame). While a host leads – a DAW that plays,
   a MIDI clock coming in (VIBE_EV_TRANSPORT tempo > 0) – the firmware's clock stays silent: one master. */
void vibe_clock_out(int on);
void vibe_tempo_set(float bpm);          /* the firmware's own tempo, 20..400 (default 120) */
float vibe_tempo_get(void);
void vibe_transport_out(int playing);    /* 1 = START, 0 = STOP – out as MIDI; the clock (if on) starts from here */

/* ---- the host's position (SDK 8): where in the song – the DAW in the plugin; elsewhere what a MIDI clock tells ------
   The values of the current audio block (inside vibe_on_audio: this block's first frame; in vibe_on_event / vibe_on_draw:
   the last block's – fine for the display). A host that gives none (an older one, the device without a clock): has = 0,
   the rest as the firmware's own. With it, a sequencer starts in the right place and follows loops. */
typedef struct {
  int32_t has;                /* 1 = the host fills this (else: only VIBE_EV_TRANSPORT as before) */
  int32_t playing;            /* 1 = the host plays */
  float   bpm;                /* 0 = none given */
  int32_t sig_num, sig_den;   /* time signature (4/4: 4, 4) */
  double  beat;               /* position in quarter notes since the song's start, at this block's first frame */
  double  bar_start;          /* where the current bar began, in quarter notes */
  int32_t loop;               /* 1 = the host loops */
  double  loop_start, loop_end;   /* in quarter notes */
} vibe_transport_info;
const vibe_transport_info *vibe_transport(void);

/* ---- outputs (SDK 9): two mono outs, and the headphones' own signal ------------------------------------------------
   OUT L / OUT R are two buffers – a firmware may always put different things on them. vibe_outputs() says so to the host:
   VIBE_OUT_DUAL_MONO – OUT L and OUT R are two separate mono outputs (drums on L, bass on R; a click on R): the host
     names them (vibe_output_names) and gives the headphones both, in the middle, instead of L on the left ear only.
   VIBE_OUT_CUE – the firmware fills a signal of its own for the HEADPHONES (pre-listening: a DJ's next track, a click
     only for the player): inside vibe_on_audio, vibe_cue(0) / vibe_cue(1) are this block's left / right headphone
     buffers (zeroed before each block, `frames` long). The device: the PHONES jack (its own pair from the codec); the
     plugin: its second output "Cue"; the browser: a second audio device when one is chosen.
   vibe_cue_host() says whether this host plays the cue on its own outputs: 0 = it does not (the prototype's interface,
   the browser without a second device) – then the headphones hear OUT L/R, and a firmware that needs cueing may fall
   back on its own (a DJ's split cue: master mono on OUT L, cue mono on OUT R). Without vibe_outputs: as before –
   the headphones hear OUT L/R. */
#define VIBE_OUT_DUAL_MONO 1
#define VIBE_OUT_CUE       2
void  vibe_outputs(int flags);
void  vibe_output_names(const char *left, const char *right);   /* with DUAL_MONO: "DRUMS", "BASS" (up to 15 characters) */
float *vibe_cue(int channel);                                    /* inside vibe_on_audio, with VIBE_OUT_CUE: 0 left, 1 right */
int   vibe_cue_host(void);                                       /* 1 = the host plays the cue on its own outputs */

/* ---- the Editor (SDK 9): how the plugin's and the browser's full-screen view lays out your parameters -------------
   All optional and declarative – strings and numbers given once in vibe_on_init; the host draws them in its style, the
   device ignores them. Without any of it: one card per group in the Editor's own order (SDK 8). A host that does not
   know these (an SDK-8 host) shows the Editor as before. */

/* your cards in YOUR order (else the Editor sorts them by name); a card not listed goes last. Names as in your groups:
   the card part of "A · FILTER" is "FILTER" – the same card name on every tab is one entry. width: columns of a
   12-column row (3 = quarter, 4 = third, 6 = half, 12 = full; 0 = the Editor decides). note: one line under the title, or 0. */
typedef struct { const char *card; int width; const char *note; } vibe_card;
void vibe_editor_cards(const vibe_card *cards, int count);           /* the list stays the firmware's (static); up to 64 */

/* how a parameter is shown (a hint – the host may fall back to a knob). BIPOLAR: the arc from the middle (pan, detune,
   a modulation depth – min < 0 < max). XY: two parameters as one pad, X then Y, both on the same card. */
enum vibe_control { VIBE_CTL_KNOB, VIBE_CTL_SLIDER, VIBE_CTL_TOGGLE, VIBE_CTL_MENU, VIBE_CTL_BIPOLAR, VIBE_CTL_XY_X, VIBE_CTL_XY_Y };
void vibe_editor_control(int param, int control);

/* colours, 0xRRGGBB (the host fits them to light and dark): the instrument's accent (filled arcs, buttons) and one per
   tab (the tab part of "A · FILTER" is "A"; up to 8 tabs) */
void vibe_editor_accent(uint32_t rgb);
void vibe_editor_tab_color(const char *tab, uint32_t rgb);

/* a small graph on a card, drawn by the host from values the firmware fills: an envelope's shape, a filter's curve, a
   wavetable's frame – 2..64 points, each 0..1, left to right. Asked for again when a parameter of that card changed;
   called from the host's drawing thread (like vibe_on_draw), never from vibe_on_audio; keep it under a millisecond.
   Up to 16 cards; the same card name on every tab: `tab` says which ("" when the card has no tab). */
typedef int (*vibe_curve_fn)(const char *tab, const char *card, float *points, int max);   /* returns the points' count */
void vibe_editor_curve(const char *card, vibe_curve_fn fn);

/* SDK 15: a curve you draw – the mouse changes the card's curve (a wavetable, a step shape, your own LFO). fn gets the
   point under the mouse (0..count-1, count = the points the curve has now) and its value 0..1, many times while one
   drags; change what the curve shows (a table, or vibe_param_set on the parameters behind it) – the Editor asks the
   curve again. Called in the firmware's flow like vibe_on_event, never from vibe_on_audio. The card needs a curve
   (vibe_editor_curve) first. Plugin only; the device and the browser ignore it. */
typedef void (*vibe_curve_draw_fn)(const char *tab, const char *card, int point, int count, float value);
void vibe_editor_curve_draw(const char *card, vibe_curve_draw_fn fn);

/* SDK 15: a button on a card ("SEND", "GET DUMP", "EXPORT"): clicked, it sends VIBE_EV_EDITOR with your id. The same
   card name on every tab: one button on each, vibe_editor_event_tab() tells them apart. Up to 16 buttons, label up to
   15 characters. With vibe_midi_send_sysex, VIBE_EV_SYSEX, vibe_media_create (SDK 13) and a dropped file (SDK 14) a
   firmware moves its patches between itself, a hardware synth and files – no extra channel needed. Plugin only. */
void vibe_editor_button(const char *card, const char *label, int id);   /* id 0..255 */
const char *vibe_editor_event_tab(void);          /* the tab of the last VIBE_EV_EDITOR ("" without tabs) */

/* SDK 15: the bytes of the SysEx that came in (VIBE_EV_SYSEX), up to 4096 – a longer one is dropped */
const uint8_t *vibe_sysex_in(void);

/* SDK 16: your own Editor page – a folder editor/ next to your .c files (index.html, its JS, CSS, images; 2 MB at
   most) replaces the Editor's cards in the plugin (EDITOR-PAGE.md). The page talks to the firmware through the host:
   parameters as always (vibe_params), and bytes both ways for everything else (a wavetable, a pattern, a name):
   vibe_editor_post sends to the page (its vibe.onData), VIBE_EV_EDITOR_DATA brings what the page sent (vibe.send).
   Up to 4096 bytes a message. post: 0 = sent, -1 = too long, or the page has not taken the last one yet (try again
   from the next vibe_on_draw). Not from vibe_on_audio. Without a page (the device, an older host) nothing arrives. */
int vibe_editor_post(const uint8_t *bytes, int n);
const uint8_t *vibe_editor_data(void);           /* the bytes of VIBE_EV_EDITOR_DATA */

/* a short help for a parameter (a tooltip in the plugin; the device shows nothing) – up to 120 characters */
void vibe_param_help(int param, const char *text);

/* audio inputs IN 1 / IN 2 (rear, balanced 6.3 mm TRS jacks). Switched on with vibe_request(VIBE_REQ_AUDIO_INPUT). */
#define VIBE_INPUTS 2
enum vibe_input_mode { VIBE_IN_LINE, VIBE_IN_MIC, VIBE_IN_HIZ };
void vibe_input_mode(int input, int mode);                             /* input 0/1, enum vibe_input_mode */
void vibe_input_link(int stereo);                                      /* 1 = IN 1 + IN 2 as one stereo input */
const float *vibe_input(int input);                                    /* inside vibe_on_audio: this block of IN 1 (0)
                                                                           or IN 2 (1), NULL while off */

/* CV/Gate (rear, 3.5 mm): CV OUT, GATE OUT, CV IN, CLK IN (CLK IN arrives as VIBE_EV_CLOCK) */
void  vibe_cv_out(float volts);                                        /* 0..+5 V, 1 V/oct */
void  vibe_gate_out(int on);                                           /* 0 V / +5 V */
float vibe_cv_in(void);                                                /* -5..+5 V */

/* storage (SDK 3): named files that stay when the device is switched off – projects, sounds, samples. Every firmware
   has its own space. Browser: the browser's storage (IndexedDB, per site), device: the microSD.
   Where they live: in VIBE FM VST inside the DAW project, per plugin instance (two instances of your synth keep two
   sets of sounds, and they travel with the song); in the browser in IndexedDB (per site); on the device on the microSD.
   name: 1-31 characters a-z 0-9 . _ -  (SDK 4: may hold folders, e.g. "grace/06-hallelujah.mp3" – up to 4 folders,
   parts of 1-63 characters, 127 in all; vibe_store_list gives the full paths). Reading and writing are synchronous. On the device a big write takes a
   few milliseconds: write from vibe_on_event (a SAVE button), never from vibe_on_audio. */
int vibe_store_write(const char *name, const void *data, int bytes);  /* 0 = ok, -1 = error (bad name, full) */
int vibe_store_read(const char *name, void *data, int max_bytes);     /* bytes read, -1 = no such file */
int vibe_store_size(const char *name);                                /* bytes, -1 = no such file */
int vibe_store_delete(const char *name);                              /* 0 = ok, -1 = no such file */
/* SDK 4: the firmware's file names, one per line ('\n'), as many whole names as fit; returns the bytes written
   (0 = no files), -1 = the host cannot list (SDK 3 host). Not from vibe_on_audio. */
int vibe_store_list(char *names, int max_bytes);
/* SDK 4: up to `bytes` bytes of a file from byte `offset` on – for files too big to read at once (music, samples).
   Returns the bytes read (0 = past the end), -1 = no such file. Not from vibe_on_audio: read ahead from
   vibe_on_event / vibe_on_draw and keep a buffer. */
int vibe_store_read_at(const char *name, int offset, void *data, int bytes);

/* ---- media (SDK 13): samples and songs that are not the firmware's own ---------------------------------------------
   The host keeps the sources: on the VIBE its card (/root/vibe-media) and USB sticks, in the browser a folder the user
   picked or dropped, in the plugin a folder from its settings. A firmware reads them, it never writes into them – except
   into "VIBE FM/<its id>/" on the source the host picks for writing (recordings, exports).
   A path is "<source>/<folders>/<file>": the source's key (from vibe_media_sources, stable – the same stick has the same
   key tomorrow), then real UTF-8 names (up to 255 bytes a part, 1023 in all). No "..", no "." parts, no '\'.
   Nothing here waits: the host reads in its own thread and answers from what it has – VIBE_MEDIA_PENDING (-2) means
   "not yet, ask again" – when VIBE_EV_MEDIA / VIBE_MEDIA_EV_READY comes (or from the next vibe_on_draw; the host is
   fetching it now and reads ahead). -1 = there is
   no such thing (any more): the source was pulled, the file is gone – never a crash. A host older than SDK 13 answers
   -1 to everything. Call these from vibe_on_event / vibe_on_draw / vibe_on_init – NEVER from vibe_on_audio: keep a
   buffer that the audio plays from and fill it ahead. */
#define VIBE_MEDIA_PENDING (-2)
#define VIBE_MEDIA_PATH    1024       /* a path's buffer, with its 0 */
enum { VIBE_MEDIA_EV_SOURCES, VIBE_MEDIA_EV_WRITTEN, VIBE_MEDIA_EV_READY, VIBE_MEDIA_EV_DROPPED };   /* ids of VIBE_EV_MEDIA */
/* the sources, one per line: "<key>\t<flags>\t<label>\n" – flags: 'w' = writes go here, 'u' = removable (USB), 'n' = network.
   Returns the bytes written (whole lines; 0 = no sources), -1 = this host has no media. The first call also asks for
   VIBE_EV_MEDIA. */
int vibe_media_sources(char *out, int max_bytes);
/* a folder ("<key>" or "<key>/<folders>"): one entry per line, folders first, then files, each sorted by name –
   "d\t\t<name>\n" a folder, "f\t<bytes>\t<name>\n" a file. Hidden names (".x") and links are left out. skip = entries to
   leave out at the start (the next page: skip + the lines you got). Returns the bytes written (0 = no more), -1 = no
   such folder, VIBE_MEDIA_PENDING. */
int vibe_media_list(const char *folder, int skip, char *out, int max_bytes);
/* 1 = a file, 2 = a folder, -1 = nothing there, VIBE_MEDIA_PENDING; *bytes and *mtime (seconds since 1970) may be 0 */
int vibe_media_stat(const char *path, uint64_t *bytes, int64_t *mtime);
/* up to `bytes` bytes from `offset` on. Returns the bytes read – may be fewer than asked, also before the end (read
   on from where it stopped); 0 = past the end; -1 = gone; VIBE_MEDIA_PENDING = not here yet. */
int vibe_media_read(const char *path, uint64_t offset, void *data, int bytes);
/* a new file in "VIBE FM/<firmware id>/" on the host's writing source: `name` is a wish ("take 1.wav", one part); the host
   makes it unique ("take 1 (2).wav") and puts the whole path into path_out (VIBE_MEDIA_PATH bytes are enough).
   Returns a handle (> 0) or -1 (no writable source, a bad name, too many open: 8). */
int vibe_media_create(const char *name, char *path_out, int max_bytes);
/* add bytes to it. 0 = taken; -1 = failed (the source is gone or full: close it); VIBE_MEDIA_PENDING = the host is
   still writing what it got before (a slow stick) – nothing was taken, offer it again */
int vibe_media_append(int handle, const void *data, int bytes);
/* finished: keep = 1 the file stays, 0 it is thrown away. Returns 0 (-1 = no such handle). The file appears under its
   name only when it is complete; VIBE_EV_MEDIA / VIBE_MEDIA_EV_WRITTEN says when. */
int vibe_media_close(int handle, int keep);
/* SDK 14: drag & drop – a file let go on the host's window (the plugin: a sample from the desktop or the DAW). The host
   copies it into "VIBE FM/<firmware id>/dropped/" on its writing source first, so it stays where the firmware's kit finds
   it. The oldest one waiting: its path into `path` (VIBE_MEDIA_PATH bytes are enough), *pad = the pad it was let go on
   (0..15, else -1), *x / *y = where on the display (0..479 / 0..319, else -1) – any may be 0. Returns 1 = one taken (ask
   again: more may wait), 0 = none, -1 = this host takes no drops. Call it once in vibe_on_init: the host lets files be
   dropped only for a firmware that asked, and sends VIBE_EV_MEDIA / VIBE_MEDIA_EV_DROPPED when one comes. */
int vibe_media_dropped(char *path, int max_bytes, int *pad, int *x, int *y);

/* ---- time (SDK 5) ---------------------------------------------------------------------------------------------- */
/* The timeline: frames computed since the first vibe_on_audio (frame 0). vibe_frames() = the first frame of the next
   block (inside vibe_on_audio: of this block). */
uint64_t vibe_frames(void);
/* Inside vibe_on_event: the frame of the timeline that was AUDIBLE when the control was touched (the player reacted to
   it). Returns 1 and sets *frame, or 0 if the host does not know (SDK 4 hosts, the browser today). For recording:
   put a note where it was heard, no guessing. Live notes: play them at once as before. */
int vibe_event_heard(uint64_t *frame);
int vibe_latency_out(void);     /* frames from computing a frame to hearing it (buffer, converter, USB); 0 = unknown */
int vibe_latency_in(void);      /* frames from IN 1/IN 2 to vibe_on_audio; 0 = unknown */

/* ---- two flows (SDK 5) ---------------------------------------------------------------------------------------- */
/* Written once at file scope: vibe_on_audio may run at the same time as vibe_on_event / vibe_on_draw (in another
   thread). Only if the firmware keeps the Protocol's rules for two flows (6.2): between the two only single-writer values of <= 32 bit
   and single-reader/single-writer queues; data first, then VIBE_RELEASE(), then the flag / counter; the reader:
   flag / counter, VIBE_ACQUIRE(), then the data. No log, store, request, lights, labels or drawing from vibe_on_audio. */
#define VIBE_PARALLEL_AUDIO int vibe_on_flows(void) { return 2; }
#define VIBE_RELEASE() __atomic_thread_fence(__ATOMIC_RELEASE)
#define VIBE_ACQUIRE() __atomic_thread_fence(__ATOMIC_ACQUIRE)

/* host services */
void vibe_log(const char *text);                                       /* to the browser console / device log */
void vibe_request(int what);                                           /* ask the runtime for something: */
#define VIBE_REQ_FIRMWARE_MENU 1                                       /*   show the firmware library */
#define VIBE_REQ_AUDIO_INPUT   2                                       /*   switch on IN 1/IN 2 (browser: ask permission) */
#define VIBE_REQ_RESTART       3                                       /*   start the firmware afresh (after its own deletes) */
#define VIBE_REQ_SPOTIFY       4                                       /*   SDK 12: Spotify Connect for this firmware (VIBE only) */
#define VIBE_REQ_STREAM_OFF    5                                       /*   SDK 12: end the stream (and Spotify Connect) */

/* ---- SDK 12: a stream from the host – Spotify Connect on the VIBE (28.10.2026) -------------------------------------
   vibe_request(VIBE_REQ_SPOTIFY): the VIBE shows up as a speaker in the Spotify app (Premium); what plays there arrives
   here, with its title. It runs only for the firmware that asked, until VIBE_REQ_STREAM_OFF or the firmware ends. Other
   hosts (the browser, the plugin) leave state at 0. Read it in vibe_on_audio: vibe_stream_read gives up to max frames at
   info->rate (44100) – resample to your rate yourself (as for an MP3). */
typedef struct {
  int32_t state;                      /* 0 none, 1 waiting (open the Spotify app: pick the VIBE), 2 playing, 3 paused */
  int32_t rate;                       /* the stream's sample rate (Hz) */
  int32_t seq;                        /* +1 whenever the fields below change (a new track, play, pause) */
  int32_t dur_ms, pos_ms;             /* the track's length, the position at the last change */
  char name[32], title[96], artist[96], album[96];   /* name: the speaker's name in the app ("VIBE PLAYER") */
} vibe_stream_info;
const vibe_stream_info *vibe_stream(void);
void vibe_areas(int count);                            /* SDK 12: the firmware has its own n areas: VIBE_EV_AREA instead of the keys */
int vibe_stream_read(float *l, float *r, int max);     /* frames read (0 = none waiting: play silence) */

/* RGB565 helpers */
#define VIBE_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define VIBE_HEX(x) VIBE_RGB(((x) >> 16) & 255, ((x) >> 8) & 255, (x) & 255)

#endif

/* SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE. */
/*
 * vibe.h – the VIBE FM SDK: the one contract between a firmware and the hardware it runs on.
 *
 * A firmware is plain C. It implements the four vibe_on_* functions below and may call the vibe_* functions
 * the SDK provides. It never talks to hardware directly. The same firmware source therefore runs
 *   - in the browser (compiled to WebAssembly, runtime/web), and
 *   - on the device (compiled for ARM Linux, runtime/linux – planned).
 *
 * Threading: the runtime calls every vibe_on_* function from ONE thread, one after the other
 * (events, then an audio block, now and then a draw). A firmware needs no locks.
 * SDK 5: a firmware may allow vibe_on_audio to run IN PARALLEL with events and draw (VIBE_PARALLEL_AUDIO below) –
 * then the display never holds up the sound. Its rules: docs/protokoll.md 6.2.
 */
#ifndef VIBE_H
#define VIBE_H

#include <stdint.h>
#include <stddef.h>

#define VIBE_SDK_VERSION 8         /* 2 (16.10.2026): IN 1/IN 2, PEDAL, CV/Gate, CLK IN; no LED rings.
                                      3 (20.10.2026): storage (vibe_store_*).
                                      4 (21.10.2026): vibe_store_list, vibe_store_read_at (big files, e.g. music).
                                      5 (21.10.2026): time (vibe_frames, vibe_event_heard, latency), two flows
                                      (VIBE_PARALLEL_AUDIO).
                                      6 (22.10.2026): one file for every host – the memory is shared (built with
                                      atomics), so the VIBE and the plugin can run the audio in its own flow.
                                      7 (22.10.2026): VIBE_EV_TRANSPORT – a DAW's play / stop and tempo.
                                      8 (26.10.2026): notes from every source (VIBE_EV_NOTE), parameters (vibe_params).
                                      Firmwares for 1-6 still run. */
#define VIBE_HOST_PROTOCOL 7       /* what a host must speak to run a firmware of this SDK – reported by vibe_sdk_version().
                                      SDK 8's additions (notes, parameters) need nothing new from a host: the notes are made
                                      inside the firmware, the parameters are optional exports a host finds by name. So an
                                      SDK-8 firmware runs on every SDK-7 host (the VIBE FM VST 0.1.0 out there), which just
                                      does not offer its parameters. (26.10.2026) */

/* ---- the device ---------------------------------------------------------------------------------------------- */
#define VIBE_SCREEN_W   480        /* 3.5" colour display, RGB565 */
#define VIBE_SCREEN_H   320
#define VIBE_MAX_BLOCK  256        /* the runtime asks for at most this many audio frames per call (web: 128) */
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

/* ---- the host's position (SDK 8): where in the song – the DAW in the plugin; elsewhere what a MIDI clock tells ------
   Valid inside vibe_on_audio, for this block's first frame. A host that gives none (an older one, the device without a
   clock): has = 0, the rest as the firmware's own. With it, a sequencer starts in the right place and follows loops. */
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
   thread). Only if the firmware keeps docs/protokoll.md 6.2: between the two only single-writer values of <= 32 bit
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

/* RGB565 helpers */
#define VIBE_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define VIBE_HEX(x) VIBE_RGB(((x) >> 16) & 255, ((x) >> 8) & 255, (x) & 255)

#endif

/* SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE. */
/*
 * MEDIA TEST – the media of SDK 13 (vibe_media_*), shown and measured; the example to copy from and the firmware the
 * hosts are tested with (tools/wasmtest.mjs, tools/vibe-check.mjs, the VIBE, the browser, the plugin).
 *   on its own: the sources, then the first source's first folder, its first file read to the end (a checksum, how often
 *   the host said "not yet", MB/s), then a file written into "VIBE FM/mediatest/" (1 s of a tone as a WAV)
 *   DATA      choose in the list        ENTER / DATA push   open the folder, read the file
 *   RETURN    a folder up / the sources K1   write the tone again
 * Everything is called from vibe_on_draw (about 30 times a second) and from VIBE_EV_MEDIA – never from vibe_on_audio.
 * While the host answers VIBE_MEDIA_PENDING the same call comes again with READY (or the next frame).
 * Logs "media: ..." lines (the tests read them).
 */
#include "vibe.h"
#include "vibe_gfx.h"
#include "vibe_dsp.h"

static float sr;
static char srcs[1024], cur[VIBE_MEDIA_PATH], lines[24][128], status[96], wrote[VIBE_MEDIA_PATH];
static int nsrc, nlines, sel, top, auto_mode = 1, auto_step, list_want = 1, src_want = 1, gone;
enum { R_IDLE, R_READ, R_DONE } rstate;
static char rpath[VIBE_MEDIA_PATH]; static uint64_t roff, rsize; static uint32_t rhash; static int rpend, rcalls, rframes;
static int wh, wstep, wwant, wpend, ready_n; static uint8_t wbuf[16384];
static void work(void);

static void logf2(const char *a, const char *b) { char m[1200]; int n = 0; for (const char *s = a; *s && n < 600; s++) m[n++] = *s; for (const char *s = b; s && *s && n < 1190; s++) m[n++] = *s; m[n] = 0; vibe_log(m); }
static int scpy(char *d, const char *s, int max) { int n = 0; while (s[n] && n < max - 1) { d[n] = s[n]; n++; } d[n] = 0; return n; }
static int scat(char *d, const char *s, int max) { int n = 0; while (d[n]) n++; return n + scpy(d + n, s, max - n); }
static const char *field(const char *l, int k)   /* the k-th tab-separated field of a line */
{ while (k-- > 0) { while (*l && *l != '\t') l++; if (*l) l++; } return l; }
static int is_dir(int i) { return lines[i][0] == 'd'; }
static const char *name_of(int i) { return field(lines[i], 2); }

void vibe_on_init(float sample_rate) { sr = sample_rate; scpy(status, "asking the host for its sources", sizeof status); work(); }   /* (READY carries it on, drawn or not) */

static void read_start(const char *p) { scpy(rpath, p, sizeof rpath); roff = 0; rsize = 0; rhash = 2166136261u; rpend = rcalls = rframes = 0; rstate = R_READ; logf2("media: read ", p); }
static void open_sel(void)
{
  if (!cur[0]) {                                   /* a source */
    if (sel < nsrc) { const char *l = lines[sel]; int n = 0; while (l[n] && l[n] != '\t' && n < 47) { cur[n] = l[n]; n++; } cur[n] = 0; list_want = 1; sel = top = 0; }
    return;
  }
  if (sel >= nlines) return;
  char p[VIBE_MEDIA_PATH]; scpy(p, cur, sizeof p); scat(p, "/", sizeof p); scat(p, name_of(sel), sizeof p);
  if (is_dir(sel)) { scpy(cur, p, sizeof cur); list_want = 1; sel = top = 0; } else read_start(p);
}
static void up(void)
{
  int n = 0; while (cur[n]) n++;
  while (n > 0 && cur[n - 1] != '/') n--;
  if (n > 0) cur[n - 1] = 0; else { cur[0] = 0; src_want = 1; }
  list_want = 1; sel = top = 0;
}

void vibe_on_event(const vibe_event *e)
{
  if (e->type == VIBE_EV_MEDIA) {
    if (e->id == VIBE_MEDIA_EV_SOURCES) { src_want = 1; logf2("media: sources changed", e->down ? " (came)" : " (went)"); }
    if (e->id == VIBE_MEDIA_EV_WRITTEN) logf2("media: written ", e->down ? wrote : "FAILED");
    if (e->id == VIBE_MEDIA_EV_READY) { ready_n++; work(); }   /* (also when the host does not draw: the plugin, its window closed) */
    return;
  }
  if (e->type == VIBE_EV_ENCODER && e->id == VE_DATA) { auto_mode = 0; sel += e->value > 0 ? 1 : -1; int n = cur[0] ? nlines : nsrc; if (sel < 0) sel = 0; if (sel >= n) sel = n - 1; if (sel < 0) sel = 0; }
  if (e->type == VIBE_EV_BUTTON && e->down) {
    if (e->id == VB_ENTER || e->id == VB_DATA_PUSH) { auto_mode = 0; open_sel(); }
    if (e->id == VB_RETURN) { auto_mode = 0; up(); }
    if (e->id == VB_K1) wwant = 1;
    if (e->id == VB_HOME) vibe_request(VIBE_REQ_FIRMWARE_MENU);
  }
}

void vibe_on_audio(const float *in, float *l, float *r, int n) { (void)in; for (int i = 0; i < n; i++) l[i] = r[i] = 0; }

static void put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static void step(void)                              /* one step of the media work */
{
  if (src_want) {
    int r = vibe_media_sources(srcs, sizeof srcs - 1);
    if (r < 0) { scpy(status, "this host has no media (SDK 13)", sizeof status); src_want = 0; nsrc = 0; return; }
    srcs[r] = 0; src_want = 0; nsrc = 0;
    for (char *p = srcs; *p && nsrc < 24; ) { char *nl = p; while (*nl && *nl != '\n') nl++; int len = (int)(nl - p); if (len > 127) len = 127; for (int i = 0; i < len; i++) lines[nsrc][i] = p[i]; lines[nsrc][len] = 0; nsrc++; p = *nl ? nl + 1 : nl; }
    logf2("media: sources ", nsrc ? srcs : "(none)");
    if (cur[0]) { int ok = 0; for (int i = 0; i < nsrc; i++) { int k = 0; while (lines[i][k] && lines[i][k] != '\t' && lines[i][k] == cur[k]) k++; if (lines[i][k] == '\t' && (cur[k] == '/' || !cur[k])) ok = 1; } if (!ok) { cur[0] = 0; gone++; if (rstate == R_READ) { rstate = R_IDLE; logf2("media: source gone while reading ", rpath); } } }
    if (auto_mode && !auto_step && nsrc) { int pick = 0; for (int i = 0; i < nsrc; i++) if (field(lines[i], 1)[0] == 'u') pick = i; sel = pick; open_sel(); auto_step = 1; }
    list_want = cur[0] != 0;
  }
  if (list_want && cur[0]) {
    static char buf[4096]; int r = vibe_media_list(cur, 0, buf, sizeof buf - 1);
    if (r == VIBE_MEDIA_PENDING) return;
    list_want = 0; nlines = 0;
    if (r < 0) { scpy(status, "the folder is gone", sizeof status); logf2("media: list failed ", cur); up(); return; }
    buf[r] = 0;
    for (char *p = buf; *p && nlines < 24; ) { char *nl = p; while (*nl && *nl != '\n') nl++; int len = (int)(nl - p); if (len > 127) len = 127; for (int i = 0; i < len; i++) lines[nlines][i] = p[i]; lines[nlines][len] = 0; nlines++; p = *nl ? nl + 1 : nl; }
    { char m[32]; logf2("media: list ", cur); logf2("media: entries ", vg_itoa(nlines, m)); }
    if (auto_mode && auto_step == 1) {             /* the first file, else into the first folder (two deep at most) */
      int f = -1; for (int i = 0; i < nlines && f < 0; i++) if (!is_dir(i)) f = i;
      if (f >= 0) { sel = f; open_sel(); auto_step = 2; }
      else if (nlines && is_dir(0)) { int d = 0; for (const char *c = cur; *c; c++) d += *c == '/'; if (d < 2) { sel = 0; open_sel(); } else { auto_step = 2; wwant = 1; } }
      else { auto_step = 2; wwant = 1; }
    }
  }
  if (rstate == R_READ) {                           /* up to 8 reads of 32 KB a frame: 7.5 MB/s at 30 frames */
    static uint8_t b[32768]; rframes++;
    for (int k = 0; k < 8 && rstate == R_READ; k++) {
      int r = vibe_media_read(rpath, roff, b, sizeof b); rcalls++;
      if (r == VIBE_MEDIA_PENDING) { rpend++; break; }
      if (r < 0) { rstate = R_IDLE; scpy(status, "the file is gone", sizeof status); logf2("media: read failed ", rpath); break; }
      if (r == 0) {
        rstate = R_DONE; rsize = roff;
        char m[160], t[24]; scpy(m, "media: done ", sizeof m); scat(m, vg_itoa((int)roff, t), sizeof m); scat(m, " bytes fnv ", sizeof m);
        for (int i = 7; i >= 0; i--) { t[7 - i] = "0123456789abcdef"[(rhash >> (4 * i)) & 15]; } t[8] = 0; scat(m, t, sizeof m);
        scat(m, " pending ", sizeof m); scat(m, vg_itoa(rpend, t), sizeof m); scat(m, " of ", sizeof m); scat(m, vg_itoa(rcalls, t), sizeof m); vibe_log(m);
        if (auto_mode && auto_step == 2) { wwant = 1; auto_step = 3; }
        break;
      }
      for (int i = 0; i < r; i++) { rhash ^= b[i]; rhash *= 16777619u; }
      roff += (uint64_t)r;
    }
  }
  if (wwant && !wh) {                               /* a file of our own: 1 s of 440 Hz, 16 bit mono WAV */
    wh = vibe_media_create("tone 440.wav", wrote, sizeof wrote);
    if (wh == VIBE_MEDIA_PENDING) { wh = 0; wpend++; return; }
    wwant = 0;
    if (wh < 0) { wh = 0; scpy(status, "cannot write here (no writable source)", sizeof status); vibe_log("media: create failed"); return; }
    logf2("media: create ", wrote); wstep = 0;
  }
  if (wh) {
    int frames = (int)sr, per = (int)(sizeof wbuf / 2), at = wstep * per;
    if (wstep == 0) {
      uint8_t hd[44]; const char *ri = "RIFF", *wa = "WAVEfmt ", *da = "data"; for (int i = 0; i < 4; i++) { hd[i] = (uint8_t)ri[i]; hd[36 + i] = (uint8_t)da[i]; } for (int i = 0; i < 8; i++) hd[8 + i] = (uint8_t)wa[i];
      put32(hd + 4, 36 + frames * 2); put32(hd + 16, 16); hd[20] = 1; hd[21] = 0; hd[22] = 1; hd[23] = 0; put32(hd + 24, (uint32_t)sr); put32(hd + 28, (uint32_t)sr * 2); hd[32] = 2; hd[33] = 0; hd[34] = 16; hd[35] = 0; put32(hd + 40, frames * 2);
      int r = vibe_media_append(wh, hd, 44); if (r == VIBE_MEDIA_PENDING) return; if (r < 0) { vibe_media_close(wh, 0); wh = 0; vibe_log("media: append failed"); return; }
      wstep = 1; return;
    }
    at = (wstep - 1) * per;
    if (at >= frames) { vibe_media_close(wh, 1); wh = 0; scpy(status, "written – waiting for the host", sizeof status); return; }
    int n = frames - at < per ? frames - at : per;
    for (int i = 0; i < n; i++) { int v = (int)(vd_sin((float)(at + i) * 440.0f / sr) * 12000.0f); wbuf[2 * i] = (uint8_t)v; wbuf[2 * i + 1] = (uint8_t)(v >> 8); }
    int r = vibe_media_append(wh, wbuf, n * 2);
    if (r == VIBE_MEDIA_PENDING) return;
    if (r < 0) { vibe_media_close(wh, 0); wh = 0; scpy(status, "writing failed (source gone?)", sizeof status); vibe_log("media: append failed"); return; }
    wstep++;
  }
}

/* steps until the host says "not yet" (READY wakes us then) or there is nothing left – never one step per frame only:
   a host that does not draw (the plugin, its window closed) calls only on events */
static void work(void)
{
  for (int i = 0; i < 6; i++) { int a = auto_step, l = list_want, s = src_want, rs = rstate, w = wh, ws = wstep, ww = wwant; uint64_t ro = roff; char c0 = cur[0]; int cl = 0; while (cur[cl]) cl++;
    step();
    int cl2 = 0; while (cur[cl2]) cl2++;
    if (a == auto_step && l == list_want && s == src_want && rs == (int)rstate && w == wh && ws == wstep && ww == wwant && ro == roff && c0 == cur[0] && cl == cl2) break; }
}

void vibe_on_draw(uint16_t *fb)
{
  (void)fb; work();
  char b[200], t[24];
  vg_clear(0x0B1220);
  vg_text("MEDIA TEST", 16, 10, VG_BIG, 0xFF6A1A, VG_LEFT);
  vg_text(cur[0] ? cur : "SOURCES", 16, 48, VG_MED, 0xE6EAF2, VG_LEFT);
  int n = cur[0] ? nlines : nsrc;
  if (sel < top) top = sel; if (sel >= top + 8) top = sel - 7;
  for (int i = top; i < n && i < top + 8; i++) {
    int y = 78 + (i - top) * 22; const char *l = lines[i];
    if (i == sel) vg_round(10, y - 2, 460, 21, 4, 0x24314A);
    if (!cur[0]) { scpy(b, l, 48); for (char *c = b; *c; c++) if (*c == '\t') *c = ' '; vg_text(b, 18, y, VG_SMALL, 0xE6EAF2, VG_LEFT); }
    else { vg_text(is_dir(i) ? "▸" : " ", 18, y, VG_SMALL, 0x3FA9F5, VG_LEFT); vg_text(name_of(i), 34, y, VG_SMALL, 0xE6EAF2, VG_LEFT); if (!is_dir(i)) vg_text(field(l, 1), 462, y, VG_SMALL, 0x8A93A6, VG_RIGHT); }
  }
  if (rstate != R_IDLE) {
    scpy(b, rstate == R_READ ? "reading " : "read ", sizeof b); scat(b, vg_itoa((int)(roff / 1024), t), sizeof b); scat(b, " KB · waited ", sizeof b);
    scat(b, vg_itoa(rpend, t), sizeof b); scat(b, " of ", sizeof b); scat(b, vg_itoa(rcalls, t), sizeof b);
    if (rstate == R_DONE && rframes) { scat(b, " · ", sizeof b); scat(b, vg_ftoa((float)rsize / 1048576.0f / ((float)rframes / 30.0f), 1, t), sizeof b); scat(b, " MB/s", sizeof b); }
    vg_text(b, 16, 262, VG_SMALL, 0x6EE7A8, VG_LEFT);
  }
  vg_text(wrote[0] ? wrote : status, 16, 284, VG_SMALL, 0x8A93A6, VG_LEFT);
  vg_text("DATA choose · ENTER open/read · RETURN up · K1 write", 16, 302, VG_SMALL, 0x5B6577, VG_LEFT);
}

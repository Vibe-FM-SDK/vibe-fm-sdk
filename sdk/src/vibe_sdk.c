/* SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE. */
/*
 * vibe_sdk.c – the part of the SDK that is compiled into every firmware:
 *   1. lights (LED table the runtime reads), host services
 *   2. drawing (vibe_gfx.h)
 *   3. sound maths (vibe_dsp.h)
 *   4. the host glue: the functions a runtime calls (browser: exported from the .wasm; device: linked in)
 *   5. memset/memcpy for the freestanding build
 */
#include "vibe.h"
#include "vibe_gfx.h"
#include "vibe_dsp.h"
#include "vibe_font.h"

/* ================================================================ 1. lights + host services ===================== */
typedef struct {
  uint32_t pad_rgb[VIBE_PADS]; float pad_lvl[VIBE_PADS];
  uint32_t btn_rgb[VB_COUNT];  float btn_lvl[VB_COUNT];
  float ring[VE_COUNT];        float ring_bipolar[VE_COUNT];
} vibe_leds;
static vibe_leds leds;

/* SDK 20: every pad of a device's grid (up to 128) also here – the first 16 stay in `leds` as before, for every host */
static struct { uint32_t n; uint32_t rgb[VIBE_PADS_MAX]; float lvl[VIBE_PADS_MAX]; } padx;
void vibe_led_pad(int p, uint32_t rgb, float level)
{
  if (p < 0 || p >= VIBE_PADS_MAX) return;
  if (p < VIBE_PADS) { leds.pad_rgb[p] = rgb; leds.pad_lvl[p] = level; }
  padx.rgb[p] = rgb; padx.lvl[p] = level; if ((uint32_t)p >= padx.n) padx.n = (uint32_t)p + 1;
}
void vibe_led_button(int b, uint32_t rgb, float level) { if (b >= 0 && b < VB_COUNT) { leds.btn_rgb[b] = rgb; leds.btn_lvl[b] = level; } }
void vibe_led_ring(int e, float v, int bipolar) { (void)e; (void)v; (void)bipolar; }   /* no rings on the device */
static char labels[VB_COUNT][8];
void vibe_label(int b, const char *t) { if (b < 0 || b >= VB_COUNT) return; int i = 0; for (; t && t[i] && i < 7; i++) labels[b][i] = t[i]; labels[b][i] = 0; }

/* ---- SDK 24 (7.11.2026, docs/sdk-24-link.md, Protokoll 15c): Link – what the page and the encoders are NOW, for a controller
   with a screen (an FM-1 under VIBE FM Link). The firmware says it while drawing (vibe_on_draw), every frame, as with FOCUS;
   the SDK keeps the last frame's and counts a change. A firmware that says nothing still has a page: the caption (vibe_label)
   of the last of K1-K6 pressed, else the standard name (SOUND ENV LFO ARP FX SEQ). ASCII only reaches the controller. */
typedef struct { uint32_t serial; char page[20]; char name[VE_COUNT][12]; char value[VE_COUNT][12]; } link_state;
static link_state link_now, link_out;
static int link_said, link_k = -1;
static void link_copy(char *d, int n, const char *t) { int i = 0; for (; t && t[i] && i < n - 1; i++) d[i] = (t[i] >= 32 && t[i] < 127) ? t[i] : '?'; d[i] = 0; }
void vibe_link_page(const char *name) { link_copy(link_now.page, sizeof link_now.page, name); link_said = 1; }
void vibe_link_encoder(int encoder, const char *name, const char *value)
{
  if (encoder < 0 || encoder >= VE_COUNT) return;
  link_copy(link_now.name[encoder], sizeof link_now.name[0], name); link_copy(link_now.value[encoder], sizeof link_now.value[0], value);
  link_said = 1;
}
static struct { uint32_t serial; uint32_t px; } link_scr;   /* SDK 25: a 240 x 240 picture (its address in the firmware's memory) */
void vibe_link_screen(const uint16_t *px) { link_scr.px = (uint32_t)(uintptr_t)px; link_scr.serial++; }
__attribute__((unused)) static int link_same(const link_state *a, const link_state *b)   /* (byte by byte: no libc here, the same on wasm and natively) */
{ const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b; for (unsigned i = 0; i < sizeof *a; i++) if (x[i] != y[i]) return 0; return 1; }
__attribute__((unused)) static void link_begin(void) { uint32_t s = link_now.serial; link_state z = {0}; link_now = z; link_now.serial = s; link_said = 0; }
__attribute__((unused)) static void link_end(void)
{
  if (!link_now.page[0] && link_k >= 0) {        /* (nothing said: the soft key last pressed) */
    static const char *const std_name[6] = { "SOUND", "ENV", "LFO", "ARP", "FX", "SEQ" };
    link_copy(link_now.page, sizeof link_now.page, link_k == VB_HOME ? "CONTROLS" : labels[link_k][0] ? labels[link_k] : std_name[link_k - VB_K1]);
  }
  link_now.serial = link_out.serial;
  if (!link_same(&link_now, &link_out)) { link_now.serial = link_out.serial + 1u; link_out = link_now; }
}
__attribute__((unused)) static void link_event(int type, int id, int down)  /* (the soft key last pressed: the page when the firmware says none) */
{
  if (type == VIBE_EV_BUTTON && down && ((id >= VB_K1 && id <= VB_K6) || id == VB_HOME)) link_k = id;
}

/* rear panel: the runtime reads what the firmware sets (CV/Gate out, input setup) and writes what comes in */
typedef struct {
  float cv_out, gate_out;      /* firmware → jack */
  float cv_in;                 /* jack → firmware */
  float in_mode[VIBE_INPUTS];  /* enum vibe_input_mode */
  float in_link;
} vibe_rear;
static vibe_rear rear;
static const float *inputs[VIBE_INPUTS];

void  vibe_cv_out(float v) { rear.cv_out = v < 0 ? 0 : v > 5 ? 5 : v; }
void  vibe_gate_out(int on) { rear.gate_out = on ? 5.0f : 0.0f; }
float vibe_cv_in(void) { return rear.cv_in; }
void  vibe_input_mode(int i, int mode) { if (i >= 0 && i < VIBE_INPUTS && mode >= VIBE_IN_LINE && mode <= VIBE_IN_HIZ) rear.in_mode[i] = (float)mode; }
void  vibe_input_link(int stereo) { rear.in_link = stereo ? 1.0f : 0.0f; }
/* SDK 26: more inputs and outputs (Protokoll 8.2). The host fills the counts and names (any time, before the first
   block at the latest) and IN 3.. before each block (has_input bit n = IN n+1); it plays OUT 3.. after it.
   Layout (wasm32): magic @0 · host_in @4 · host_out @8 (host writes; 0 = never: 2) · out_used @12 (bit n = OUT n+1 written
   this block) · in_name @16, out_name @272 (16 x 16 bytes each, host writes) · in @528: 14 x 256 f32 (IN 3..16) ·
   out @14864: 14 x 256 f32 (OUT 3..16) – 29200 bytes */
#define PORTS_MAGIC 0x53545250u                  /* "PRTS" */
static struct { uint32_t magic; int32_t host_in, host_out; volatile uint32_t out_used; char in_name[VIBE_IO_MAX][16], out_name[VIBE_IO_MAX][16];
                float in[VIBE_IO_MAX - 2][VIBE_MAX_BLOCK], out[VIBE_IO_MAX - 2][VIBE_MAX_BLOCK]; } ports = { PORTS_MAGIC, 0, 0, 0, {{0}}, {{0}}, {{0}}, {{0}} };
static const float *inputs_x[VIBE_IO_MAX];      /* IN 3.. of this block (0, 1 unused: inputs[]) */
static float *outs_lr[2];                        /* OUT L/R of this block */
const float *vibe_input(int i) { return i >= 0 && i < VIBE_INPUTS ? inputs[i] : i >= VIBE_INPUTS && i < VIBE_IO_MAX ? inputs_x[i] : 0; }
int vibe_input_count(void) { int n = ports.host_in; return n >= 2 && n <= VIBE_IO_MAX ? n : 2; }
int vibe_output_count(void) { int n = ports.host_out; return n >= 2 && n <= VIBE_IO_MAX ? n : 2; }
float *vibe_output(int o)
{
  if (o == 0 || o == 1) return outs_lr[o];
  if (o < 2 || o >= vibe_output_count() || !outs_lr[0]) return 0;   /* (outside vibe_on_audio: none) */
  ports.out_used |= 1u << o;
  return ports.out[o - 2];
}
const char *vibe_input_name(int i) { if (i < 0 || i >= VIBE_IO_MAX) return ""; ports.in_name[i][15] = 0; return ports.in_name[i]; }
const char *vibe_output_name(int o) { if (o < 0 || o >= VIBE_IO_MAX) return ""; ports.out_name[o][15] = 0; return ports.out_name[o]; }

#ifdef __wasm__
#define VIBE_IMPORT(n) __attribute__((import_module("env"), import_name(#n)))
#define VIBE_EXPORT(n) __attribute__((export_name(#n)))
VIBE_IMPORT(vibe_host_log) void vibe_host_log(const char *s, int len);
VIBE_IMPORT(vibe_host_request) void vibe_host_request(int what);
VIBE_IMPORT(vibe_host_store) int vibe_host_store(int op, const char *name, int name_len, void *data, int len);
#else
/* native builds (device runtime, tests) provide these */
void vibe_host_log(const char *s, int len);
void vibe_host_request(int what);
int vibe_host_store(int op, const char *name, int name_len, void *data, int len);
#endif

void vibe_log(const char *t) { int n = 0; while (t[n]) n++; vibe_host_log(t, n); }
void vibe_request(int what) { vibe_host_request(what); }

/* storage (SDK 3): one host call, op 0 read, 1 write, 2 size, 3 delete; SDK 4: op 4 list (no name), op 5 read at an
   offset (the offset goes in the data's first 4 bytes, little endian, the host overwrites them with the file's bytes). The name is checked here, so every runtime
   can use it as a file name as it is. */
/* a name: 1-31 characters a-z 0-9 . _ -, not starting with '.'; SDK 4: also a path – up to 4 folders separated by '/',
   each part 1-63 such characters (not starting with '.'), at most 127 in all. Checked here, so every runtime can use it
   as a file path as it is (no "..", no absolute paths, no empty parts). */
static int store_name(const char *n) {
  int l = 0, seg = 0, parts = 1, path = 0;
  if (!n) return 0;
  for (int i = 0; n[i]; i++) if (n[i] == '/') path = 1;   /* (no strchr: the WebAssembly build has no libc) */
  for (; n[l]; l++) {
    char c = n[l];
    if (l >= 127) return 0;
    if (c == '/') { if (!seg || ++parts > 5) return 0; seg = 0; continue; }
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    if (!seg && c == '.') return 0;
    if (++seg > (path ? 63 : 31)) return 0;
  }
  return seg ? l : 0;
}
int vibe_store_read(const char *n, void *d, int max) { int l = store_name(n); return l && max >= 0 ? vibe_host_store(0, n, l, d, max) : -1; }
int vibe_store_write(const char *n, const void *d, int bytes) { int l = store_name(n); return l && bytes >= 0 ? vibe_host_store(1, n, l, (void *)d, bytes) : -1; }
int vibe_store_size(const char *n) { int l = store_name(n); return l ? vibe_host_store(2, n, l, 0, 0) : -1; }
int vibe_store_delete(const char *n) { int l = store_name(n); return l ? vibe_host_store(3, n, l, 0, 0) : -1; }
/* SDK 19: the device (op 14, the host fills the struct as it is laid out in vibe.h); no answer = the base, VIBE FM original */
_Static_assert(sizeof(vibe_device_info) == 52, "op 14's layout is the hosts' (Protokoll 15b): 52 bytes");
int vibe_device(vibe_device_info *info)
{
  if (!info) return 0;
  vibe_device_info b; uint8_t *p = (uint8_t *)&b; for (unsigned i = 0; i < sizeof b; i++) p[i] = 0;
  b.keys = VIBE_KEYS; b.pads = VIBE_PADS; b.display_w = 480; b.display_h = 320;
  const char *n = "VIBE FM"; for (int i = 0; n[i]; i++) b.name[i] = n[i];
  vibe_device_info h = b;
  int r = vibe_host_store(14, "", 0, &h, (int)sizeof h);
  b.pad_cols = 4; b.pad_rows = 4;
  if (r > 0 && h.keys >= VIBE_KEYS && h.keys <= 61 && h.pads >= VIBE_PADS && h.pads <= VIBE_PADS_MAX && h.encoders_extra <= 8
      && h.faders <= 16 && h.buttons_extra <= 16 && h.display_w >= 480 && h.display_h >= 320) {
    h.name[sizeof h.name - 1] = 0;
    if (!h.pad_cols || !h.pad_rows || h.pad_cols * h.pad_rows != h.pads) { h.pad_cols = 4; h.pad_rows = (uint8_t)((h.pads + 3) / 4); }   /* (no shape: rows of 4) */
    *info = h; return 1;                                  /* (a host's answer only when it keeps the base) */
  }
  *info = b; return 0;
}
int vibe_store_list(char *buf, int max) { return buf && max > 0 ? vibe_host_store(4, "", 0, buf, max) : -1; }
int vibe_store_read_at(const char *n, int off, void *d, int bytes) {
  int l = store_name(n); if (!l || off < 0 || bytes < 4) return -1;   /* (4 bytes at least: the offset travels in them) */
  unsigned char *p = d; p[0] = (unsigned char)off; p[1] = (unsigned char)(off >> 8); p[2] = (unsigned char)(off >> 16); p[3] = (unsigned char)(off >> 24);
  return vibe_host_store(5, n, l, d, bytes);
}

/* media (SDK 13): ops 6-12 of the same import (Protokoll 13a). A path is checked here as for the store, so a host gets
   only clean ones (it checks again: a firmware without the SDK may send anything): the source's key (1-47 of A-Z a-z
   0-9 _ -), then parts of 1-255 bytes of well-formed UTF-8 without control characters or '\', not "." or "..";
   1023 bytes in all. part_only: one part, not starting with '.' (the wish for vibe_media_create). */
static int utf8_len(const unsigned char *s, int n)     /* bytes of the well-formed character at s, 0 = broken */
{
  unsigned c = s[0];
  if (c < 0x80) return 1;
  int k = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 && c < 0xF0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
  if (!k || k > n) return 0;
  for (int i = 1; i < k; i++) if ((s[i] & 0xC0) != 0x80) return 0;
  if (k == 3 && ((c == 0xE0 && s[1] < 0xA0) || (c == 0xED && s[1] >= 0xA0))) return 0;   /* (overlong, surrogate) */
  if (k == 4 && ((c == 0xF0 && s[1] < 0x90) || (c == 0xF4 && s[1] >= 0x90))) return 0;
  return k;
}
static int media_path(const char *p, int part_only)
{
  if (!p) return 0;
  int n = 0; while (p[n]) { if (++n > 1023) return 0; }
  const unsigned char *s = (const unsigned char *)p; int i = 0;
  if (!part_only) {
    for (; i < n && s[i] != '/'; i++) { unsigned char c = s[i]; if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0; }
    if (i < 1 || i > 47) return 0;
    if (i == n) return n;                          /* (the source itself) */
    i++;
  }
  for (;;) {                                       /* a part */
    int st = i;
    while (i < n && s[i] != '/') {
      if (s[i] < 0x20 || s[i] == 0x7F || s[i] == '\\') return 0;
      int k = utf8_len(s + i, n - i); if (!k) return 0; i += k;
    }
    int l = i - st;
    if (l < 1 || l > 255 || (s[st] == '.' && (l == 1 || (l == 2 && s[st + 1] == '.')))) return 0;
    if (part_only) return i == n && s[st] != '.' ? n : 0;
    if (i == n) return n;
    i++;
  }
}
int vibe_media_sources(char *out, int max) { return out && max > 0 ? vibe_host_store(6, "", 0, out, max) : -1; }
int vibe_media_list(const char *dir, int skip, char *out, int max)
{
  int l = media_path(dir, 0); if (!l || !out || max < 4 || skip < 0) return -1;
  unsigned char *b = (unsigned char *)out; b[0] = (unsigned char)skip; b[1] = (unsigned char)(skip >> 8); b[2] = (unsigned char)(skip >> 16); b[3] = (unsigned char)(skip >> 24);
  return vibe_host_store(7, dir, l, out, max);
}
int vibe_media_stat(const char *path, uint64_t *bytes, int64_t *mtime)
{
  int l = media_path(path, 0); if (!l) return -1;
  uint64_t t[2] = { 0, 0 }; int r = vibe_host_store(8, path, l, t, 16);
  if (bytes) *bytes = r > 0 ? t[0] : 0; if (mtime) *mtime = r > 0 ? (int64_t)t[1] : 0;
  return r;
}
int vibe_media_read(const char *path, uint64_t off, void *data, int bytes)
{
  int l = media_path(path, 0); if (!l || !data || bytes < 1) return -1;
  if (bytes < 8) {                                 /* (the offset travels in the data's first 8 bytes) */
    unsigned char t[8]; int r = vibe_media_read(path, off, t, 8);
    if (r > bytes) r = bytes; for (int i = 0; i < r; i++) ((unsigned char *)data)[i] = t[i];
    return r;
  }
  unsigned char *b = data; for (int i = 0; i < 8; i++) b[i] = (unsigned char)(off >> (8 * i));
  return vibe_host_store(9, path, l, data, bytes);
}
int vibe_media_create(const char *name, char *path_out, int max)
{
  int l = media_path(name, 1); if (!l || !path_out || max < 2) return -1;
  path_out[0] = 0; return vibe_host_store(10, name, l, path_out, max);
}
int vibe_media_append(int h, const void *data, int bytes) { return h > 0 && data && bytes >= 0 ? vibe_host_store(11, 0, h, (void *)data, bytes) : -1; }
int vibe_media_close(int h, int keep) { return h > 0 ? vibe_host_store(12, 0, h, 0, keep ? 1 : 0) : -1; }
int vibe_media_dropped(char *path, int max, int *pad, int *x, int *y)
{
  static unsigned char b[12 + VIBE_MEDIA_PATH];    /* (i32 pad, x, y, then the path – the host checked it) */
  int r = vibe_host_store(13, "", 0, b, sizeof b), v[3] = { -1, -1, -1 };
  if (r == 1) {
    for (int i = 0; i < 3; i++) v[i] = (int)((unsigned)b[i * 4] | (unsigned)b[i * 4 + 1] << 8 | (unsigned)b[i * 4 + 2] << 16 | (unsigned)b[i * 4 + 3] << 24);
    int k = 0; if (path && max > 0) { while (k < max - 1 && b[12 + k]) { path[k] = (char)b[12 + k]; k++; } path[k] = 0; }
  } else if (path && max > 0) path[0] = 0;
  if (pad) *pad = v[0]; if (x) *x = v[1]; if (y) *y = v[2];
  return r == 1 ? 1 : r == 0 ? 0 : -1;
}

int vibe_media_add(int ask) { char c = 0; return vibe_host_store(15, "", 0, &c, ask ? 1 : 0) == 1 ? 1 : -1; }   /* (SDK 22) */

/* ================================================================ 2. drawing ==================================== */
static uint16_t *FB;
/* (SDK 23) the size of what vg_* draw into: the display (480 x 320, vg_begin) or the stage (vg_begin_size) */
static int W = VIBE_SCREEN_W, H = VIBE_SCREEN_H;

static inline uint16_t c565(uint32_t c) { return VIBE_HEX(c); }
static inline void blend(int x, int y, uint32_t c, float a) {
  if ((unsigned)x >= W || (unsigned)y >= H || a <= 0.0f) return;
  uint16_t *p = &FB[y * W + x];
  if (a >= 0.999f) { *p = c565(c); return; }
  uint16_t d = *p;
  int dr = (d >> 11) << 3, dg = ((d >> 5) & 63) << 2, db = (d & 31) << 3;
  int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
  int ia = (int)(a * 256.0f);
  r = dr + (((r - dr) * ia) >> 8); g = dg + (((g - dg) * ia) >> 8); b = db + (((b - db) * ia) >> 8);
  *p = VIBE_RGB(r, g, b);
}

void vg_begin(uint16_t *fb) { FB = fb; W = VIBE_SCREEN_W; H = VIBE_SCREEN_H; }
void vg_begin_size(uint16_t *fb, int w, int h) { FB = fb; W = w > 0 ? w : VIBE_SCREEN_W; H = h > 0 ? h : VIBE_SCREEN_H; }   /* (SDK 23: the stage) */
void vg_clear(uint32_t c) { uint16_t v = c565(c); for (int i = 0; i < W * H; i++) FB[i] = v; }
void vg_rect(int x, int y, int w, int h, uint32_t c) {
  if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x; if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  uint16_t v = c565(c);
  for (int j = y; j < y + h; j++) { uint16_t *r = &FB[j * W + x]; for (int i = 0; i < w; i++) r[i] = v; }
}
void vg_rect_a(int x, int y, int w, int h, uint32_t c, float a) {
  if (a >= 0.999f) { vg_rect(x, y, w, h, c); return; }
  for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) blend(i, j, c, a);
}
void vg_round(int x, int y, int w, int h, int r, uint32_t c) {
  if (r <= 0) { vg_rect(x, y, w, h, c); return; }
  vg_rect(x, y + r, w, h - 2 * r, c); vg_rect(x + r, y, w - 2 * r, r, c); vg_rect(x + r, y + h - r, w - 2 * r, r, c);
  for (int j = 0; j < r; j++) for (int i = 0; i < r; i++) {
    float dx = r - i - 0.5f, dy = r - j - 0.5f, d = vd_sqrt(dx * dx + dy * dy), a = vd_clamp(r - d + 0.5f, 0, 1);
    blend(x + i, y + j, c, a); blend(x + w - 1 - i, y + j, c, a); blend(x + i, y + h - 1 - j, c, a); blend(x + w - 1 - i, y + h - 1 - j, c, a);
  }
}
void vg_frame(int x, int y, int w, int h, int t, uint32_t c) { vg_rect(x, y, w, t, c); vg_rect(x, y + h - t, w, t, c); vg_rect(x, y, t, h, c); vg_rect(x + w - t, y, t, h, c); }
void vg_line(float x0, float y0, float x1, float y1, float wd, uint32_t c) {
  float minx = vd_minf(x0, x1) - wd, maxx = vd_maxf(x0, x1) + wd, miny = vd_minf(y0, y1) - wd, maxy = vd_maxf(y0, y1) + wd;
  float dx = x1 - x0, dy = y1 - y0, ll = dx * dx + dy * dy; if (ll < 1e-6f) ll = 1e-6f;
  for (int j = (int)miny; j <= (int)maxy; j++) for (int i = (int)minx; i <= (int)maxx; i++) {
    float px = i + 0.5f - x0, py = j + 0.5f - y0, t = vd_clamp((px * dx + py * dy) / ll, 0, 1);
    float ex = px - t * dx, ey = py - t * dy, d = vd_sqrt(ex * ex + ey * ey);
    blend(i, j, c, vd_clamp(wd * 0.5f - d + 0.5f, 0, 1));
  }
}
void vg_circle(float cx, float cy, float r, uint32_t c) {
  for (int j = (int)(cy - r - 1); j <= (int)(cy + r + 1); j++) for (int i = (int)(cx - r - 1); i <= (int)(cx + r + 1); i++) {
    float dx = i + 0.5f - cx, dy = j + 0.5f - cy; blend(i, j, c, vd_clamp(r - vd_sqrt(dx * dx + dy * dy) + 0.5f, 0, 1));
  }
}
static float edge(float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }
void vg_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c) {
  float area = edge(x0, y0, x1, y1, x2, y2); if (area == 0) return;
  int minx = (int)vd_minf(x0, vd_minf(x1, x2)), maxx = (int)vd_maxf(x0, vd_maxf(x1, x2)) + 1;
  int miny = (int)vd_minf(y0, vd_minf(y1, y2)), maxy = (int)vd_maxf(y0, vd_maxf(y1, y2)) + 1;
  for (int j = miny; j <= maxy; j++) for (int i = minx; i <= maxx; i++) {
    int hits = 0;   /* 4x supersampling for smooth edges */
    for (int s = 0; s < 4; s++) {
      float px = i + 0.25f + 0.5f * (s & 1), py = j + 0.25f + 0.5f * (s >> 1);
      float w0 = edge(x1, y1, x2, y2, px, py), w1 = edge(x2, y2, x0, y0, px, py), w2 = edge(x0, y0, x1, y1, px, py);
      if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) || (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) hits++;
    }
    if (hits) blend(i, j, c, hits * 0.25f);
  }
}

static uint32_t utf8_next(const char **s) {
  const unsigned char *p = (const unsigned char *)*s; uint32_t c = *p;
  if (c < 0x80) { *s += 1; return c; }
  if ((c >> 5) == 6) { c = ((c & 31) << 6) | (p[1] & 63); *s += 2; return c; }
  if ((c >> 4) == 14) { c = ((c & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); *s += 3; return c; }
  *s += 1; return '?';
}
static const vf_glyph *glyph(const vf_font *f, uint32_t cp) {
  if (cp >= 32 && cp < 127 && f->g[cp - 32].cp == cp) return &f->g[cp - 32];
  for (int i = 0; i < f->count; i++) if (f->g[i].cp == cp) return &f->g[i];
  return glyph(f, '?');
}
/* (SDK 23) font | VG_X2: the same face twice the size, pixel for pixel – for the stage (960 x 540 and more) */
#define VG_SCALE(font) (((font) & VG_X2) ? 2 : 1)
#define VG_FACE(font) (&vf_fonts[(font) & (VG_X2 - 1)])
int vg_text_width(const char *t, int font) {
  const vf_font *f = VG_FACE(font); int w = 0;
  while (*t) w += glyph(f, utf8_next(&t))->adv;
  return w * VG_SCALE(font);
}
int vg_font_height(int font) { return VG_FACE(font)->height * VG_SCALE(font); }
int vg_text(const char *t, int x, int y, int font, uint32_t c, int align) {
  const vf_font *f = VG_FACE(font); const int sc = VG_SCALE(font);
  int w = vg_text_width(t, font);
  if (align == VG_CENTER) x -= w / 2; else if (align == VG_RIGHT) x -= w;
  while (*t) {
    const vf_glyph *g = glyph(f, utf8_next(&t));
    const uint8_t *bits = vf_bits + g->off;
    for (int j = 0, k = 0; j < g->h; j++) for (int i = 0; i < g->w; i++, k++) {
      int v = (bits[k >> 1] >> ((k & 1) * 4)) & 15;
      if (!v) continue;
      if (sc == 1) blend(x + g->x + i, y + g->y + j, c, v / 15.0f);
      else { int px = x + (g->x + i) * 2, py = y + (g->y + j) * 2; float a = v / 15.0f; blend(px, py, c, a); blend(px + 1, py, c, a); blend(px, py + 1, c, a); blend(px + 1, py + 1, c, a); }
    }
    x += g->adv * sc;
  }
  return w;
}
char *vg_itoa(int v, char *buf) {
  char tmp[12]; int n = 0, neg = v < 0; unsigned u = neg ? -v : v;
  do { tmp[n++] = '0' + u % 10; u /= 10; } while (u);
  int k = 0; if (neg) buf[k++] = '-'; while (n) buf[k++] = tmp[--n]; buf[k] = 0; return buf;
}
char *vg_ftoa(float v, int dec, char *buf) {
  int scale = 1; for (int i = 0; i < dec; i++) scale *= 10;
  int neg = v < 0; if (neg) v = -v;
  int iv = (int)(v * scale + 0.5f), ip = iv / scale, fp = iv % scale;
  char *p = buf; if (neg && iv) *p++ = '-';
  vg_itoa(ip, p); while (*p) p++;
  if (dec) { *p++ = '.'; for (int i = dec - 1; i >= 0; i--) { p[i] = '0' + fp % 10; fp /= 10; } p += dec; *p = 0; }
  return buf;
}
uint32_t vg_mix(uint32_t a, uint32_t b, float t) {
  int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255, br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
  return ((uint32_t)(ar + (br - ar) * t) << 16) | ((uint32_t)(ag + (bg - ag) * t) << 8) | (uint32_t)(ab + (bb - ab) * t);
}

/* ---- 3.11.2026: HOME = what every control does (Protokoll 3.2) – the standard overview, one call ------------------ */
void vg_controls(const char *title, const char *const *rows, int n)
{
  vg_clear(0x0B1220);
  int tw = vg_text("CONTROLS", 10, 8, VG_MED, 0xFF6A1A, VG_LEFT);
  if (title && title[0]) vg_text(title, 22 + tw, 10, VG_SMALL, 0xE6EAF2, VG_LEFT);
  vg_text("HOME / \xE2\x86\xA9 back \xC2\xB7 HOME 1 s: all firmwares", 470, 10, VG_SMALL, 0x5B6577, VG_RIGHT);
  vg_rect(0, 31, VIBE_SCREEN_W, 1, 0xFF6A1A);
  const int per = 11, two = n > per, w = two ? 226 : 460;
  int nw = 0;                                    /* the name column: as wide as the longest name */
  for (int i = 0; i < n && i < 2 * per; i++) { int k = vg_text_width(rows[2 * i] ? rows[2 * i] : "", VG_SMALL); if (k > nw) nw = k; }
  nw += 14; if (nw > w / 2) nw = w / 2;
  for (int i = 0; i < n && i < 2 * per; i++) {
    int x = 10 + (i / per) * 234, y = 40 + (i % per) * 25;
    char b[96]; const char *s = rows[2 * i + 1] ? rows[2 * i + 1] : ""; int l = 0;
    while (s[l] && l < (int)sizeof b - 1) { b[l] = s[l]; l++; } b[l] = 0;
    while (l > 0 && vg_text_width(b, VG_BOLD) > w - nw - 12) { do l--; while (l > 0 && ((unsigned char)b[l] & 0xC0) == 0x80); b[l] = 0; }   /* (too long: cut) */
    vg_round(x, y, w, 22, 3, 0x18223A);
    vg_text(rows[2 * i] ? rows[2 * i] : "", x + 7, y + 4, VG_SMALL, 0x8A93A6, VG_LEFT);
    vg_text(b, x + 7 + nw, y + 4, VG_BOLD, 0xE6EAF2, VG_LEFT);
  }
}

/* ---- SDK 10: FOCUS (vibe_gfx.h) – the controls of the last frame, DATA walks them, ENTER uses them ---------------- */
#define VF_MAX 64
typedef struct { int id, x, y, w, h; float *v, lo, hi, step; } vf_item;
static vf_item VF[VF_MAX], VFN[VF_MAX]; static int vf_n, vf_nn, vf_cur = -1, vf_edit, vf_off, vf_md, vf_on; static float vf_acc;
static float sdk_sr;                             /* (defined below, SDK 9) */
static int vf_sw, vf_shift; static uint64_t vf_t;     /* vf_shift_walk: on · SHIFT held · the last touch (frames) */
void vf_shift_walk(int on) { vf_sw = on; }
static int vf_index(int id);
void vf_mode(int m) { vf_md = m; vf_on = 0; vf_edit = 0; }
int vf_active(void) { return vf_md == VF_ALWAYS || vf_on; }
static void vf_toggle(void)                      /* VF_ON_HOLD: the long press */
{
  vf_on = !vf_on; vf_edit = 0; vf_acc = 0;
  if (vf_on && vf_index(vf_cur) < 0 && vf_n) vf_cur = VF[0].id;   /* (on: the frame shows at once) */
}
void vf_begin(void) { vf_nn = 0; }
static void vf_add(int id, int x, int y, int w, int h, float *v, float lo, float hi, float step)
{ if (vf_nn < VF_MAX) { vf_item *t = &VFN[vf_nn++]; t->id = id; t->x = x; t->y = y; t->w = w; t->h = h; t->v = v; t->lo = lo; t->hi = hi; t->step = step; } }
void vf_button(int id, int x, int y, int w, int h) { vf_add(id, x, y, w, h, 0, 0, 0, 0); }
#define VF_ENC 0x40000000                        /* (ids of the stand-ins: never a firmware's own) */
#define VF_KEY 0x40001000
void vf_encoder(int enc, int x, int y, int w, int h) { vf_add(VF_ENC + enc, x, y, w, h, 0, 0, 0, 0); }
void vf_key(int b, int x, int y, int w, int h) { vf_add(VF_KEY + b, x, y, w, h, 0, 0, 0, 0); }
void vf_value(int id, int x, int y, int w, int h, float *v, float lo, float hi, float step) { vf_add(id, x, y, w, h, v, lo, hi, step); }
static int vf_index(int id) { for (int i = 0; i < vf_n; i++) if (VF[i].id == id) return i; return -1; }
void vf_end(uint32_t c)
{
  for (int i = 0; i < vf_nn; i++) VF[i] = VFN[i];
  vf_n = vf_nn;
  if (vf_sw && vf_on && !vf_edit && !vf_shift && vibe_frames() - vf_t > (uint64_t)(sdk_sr * 6.0f)) vf_on = 0;   /* (SHIFT walk: 6 s untouched = off) */
  if (vf_off || vf_cur < 0 || !vf_active()) return;
  int i = vf_index(vf_cur); if (i < 0) { vf_edit = 0; return; }   /* (its control is not on this page: no frame) */
  const vf_item *t = &VF[i];
  vg_frame(t->x - 2, t->y - 2, t->w + 4, t->h + 4, 2, c);
  if (vf_edit) { vg_frame(t->x - 5, t->y - 5, t->w + 10, t->h + 10, 1, c); vg_tri(t->x - 9, t->y + t->h / 2 - 5, t->x - 9, t->y + t->h / 2 + 5, t->x - 4, t->y + t->h / 2, c); }
}
void vf_enable(int on) { vf_off = !on; if (vf_off) vf_edit = 0; }
int vf_focus(void) { return vf_cur; }
int vf_editing(void) { return vf_edit; }
void vf_set_focus(int id) { vf_cur = id; vf_edit = 0; vf_acc = 0; }
/* SDK 18: a click (VIBE_EV_TOUCH) on a control of the last frame – a button is pressed, a stand-in key pressed and let go;
 * a slider (at least three times as wide as high) takes the value where it is clicked and dragged, a small value cell is
 * edited (DATA turns it, dragging up / down too), a stand-in encoder as well. A click on no control: VF_NO, the firmware's.
 * Opt-in: vf_clicks(1) in vibe_on_init (VIBE DJ, 31.10.) – without it a click stays VF_NO, as before */
static int vf_drag = -1, vf_y0, vf_clk; static float vf_v0;
void vf_clicks(int on) { vf_clk = on; }
static int vf_touch_set(const vf_item *t, int x, int y, int *id)
{
  float n;
  if (t->w >= 3 * t->h) { n = t->lo + (float)(x - t->x) / (float)(t->w > 1 ? t->w : 1) * (t->hi - t->lo); if (t->step > 0) n = t->lo + (float)(int)((n - t->lo) / t->step + 0.5f) * t->step; }
  else n = vf_v0 + (float)(vf_y0 - y) * (t->hi - t->lo) / 150.0f;   /* (a cell: 150 px up = the whole range) */
  if (n < t->lo) n = t->lo; if (n > t->hi) n = t->hi;
  if (n == *t->v) return VF_MOVED;
  *t->v = n; if (id) *id = t->id; return VF_CHANGED;
}
static int vf_touch(const vibe_event *e, int *id)
{
  int x = VIBE_TOUCH_X(e), y = VIBE_TOUCH_Y(e);
  if (e->id != VIBE_TOUCH_DOWN) {                /* moved or let go: the value being dragged */
    if (vf_drag < 0) return VF_NO;
    int j = vf_index(vf_drag); if (e->id == VIBE_TOUCH_UP) vf_drag = -1;
    return j < 0 || !VF[j].v || e->id == VIBE_TOUCH_UP ? VF_MOVED : vf_touch_set(&VF[j], x, y, id);
  }
  vf_drag = -1;
  for (int j = vf_n - 1; j >= 0; j--) {          /* (the last drawn lies on top) */
    const vf_item *t = &VF[j];
    if (x < t->x - 3 || x >= t->x + t->w + 3 || y < t->y - 3 || y >= t->y + t->h + 3) continue;
    vf_cur = t->id; vf_edit = 0; vf_acc = 0;
    if (t->id >= VF_KEY) { vibe_event k = { VIBE_EV_BUTTON, (uint8_t)(t->id - VF_KEY), 1, { 0, 0, 0 }, 0 }; vibe_on_event(&k); k.down = 0; vibe_on_event(&k); return VF_MOVED; }
    if (t->id >= VF_ENC) { vf_edit = 1; if (vf_md == VF_ON_HOLD) vf_on = 1; return VF_MOVED; }
    if (!t->v) { if (id) *id = t->id; return VF_PRESS; }
    vf_drag = t->id; vf_y0 = y; vf_v0 = *t->v;
    if (t->w >= 3 * t->h) return vf_touch_set(t, x, y, id);
    vf_edit = 1; if (vf_md == VF_ON_HOLD) vf_on = 1; return VF_MOVED;
  }
  return VF_NO;
}
int vf_event(const vibe_event *e, int *id)
{
  if (e->type == VIBE_EV_TOUCH) return vf_off || !vf_n || !vf_clk ? VF_NO : (vf_t = vibe_frames(), vf_touch(e, id));
  if (vf_off || !vf_n || !vf_active()) return VF_NO;
  vf_t = vibe_frames();
  int i = vf_index(vf_cur);
  if (e->type == VIBE_EV_ENCODER && e->id == VE_DATA) {
    if (vf_edit && i >= 0 && VF[i].id >= VF_ENC && VF[i].id < VF_KEY) {   /* editing a stand-in: the encoder turns */
      vibe_event t = { VIBE_EV_ENCODER, (uint8_t)(VF[i].id - VF_ENC), 0, { 0, 0, 0 }, e->value }; vibe_on_event(&t); return VF_MOVED;
    }
    if (vf_edit && i >= 0 && VF[i].v) {          /* editing: DATA changes the value */
      float *v = VF[i].v, n = *v + e->value * VF[i].step; if (n < VF[i].lo) n = VF[i].lo; if (n > VF[i].hi) n = VF[i].hi;
      if (n == *v) return VF_MOVED;
      *v = n; if (id) *id = VF[i].id; return VF_CHANGED;
    }
    if (i < 0) { vf_cur = VF[0].id; vf_acc = 0; return VF_MOVED; }   /* (the first turn: the frame appears on the first control) */
    vf_acc += e->value;
    while (vf_acc >= 1) { vf_acc -= 1; if (i < vf_n - 1) i++; }
    while (vf_acc <= -1) { vf_acc += 1; if (i > 0) i--; }
    vf_cur = VF[i].id; return VF_MOVED;
  }
  if (e->type != VIBE_EV_BUTTON) return VF_NO;
  if (e->id == VB_RETURN && vf_edit) { if (e->down) vf_edit = 0; return VF_MOVED; }
  if (e->id == VB_RETURN && vf_md == VF_ON_HOLD) { if (e->down) vf_on = 0; return VF_MOVED; }   /* (FOCUS off: DATA is the firmware's again) */
  if (e->id != VB_ENTER && e->id != VB_DATA_PUSH) return VF_NO;
  if (i < 0) return VF_NO;                       /* (no frame yet: ENTER stays the firmware's) */
  if (!e->down) return VF_MOVED;
  if (VF[i].v || (VF[i].id >= VF_ENC && VF[i].id < VF_KEY)) { vf_edit = !vf_edit; return VF_MOVED; }
  if (VF[i].id >= VF_KEY) {                      /* a stand-in key: pressed and let go */
    vibe_event t = { VIBE_EV_BUTTON, (uint8_t)(VF[i].id - VF_KEY), 1, { 0, 0, 0 }, 0 }; vibe_on_event(&t); t.down = 0; vibe_on_event(&t); return VF_MOVED;
  }
  if (id) *id = VF[i].id; return VF_PRESS;
}

/* ================================================================ 3. sound maths ================================= */
typedef union { float f; uint32_t i; } fbits;
float vd_sin(float ph) {
  ph -= (float)(int)ph; if (ph < 0) ph += 1.0f;          /* 0..1 */
  float x = ph * VD_TAU - VD_PI;                         /* -pi..pi, then sin(x + pi) = -sin(x) */
  float y = 1.27323954f * x - 0.405284735f * x * vd_absf(x);
  y = 0.225f * (y * vd_absf(y) - y) + y;
  return -y;
}
float vd_exp2(float x) {
  if (x < -126.0f) return 0.0f; if (x > 126.0f) x = 126.0f;
  int i = (int)x; if (x < 0 && x != (float)i) i--;
  float f = x - (float)i;
  float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
  fbits b; b.i = (uint32_t)(i + 127) << 23; return p * b.f;
}
float vd_log2(float x) {
  if (x <= 0) return -126.0f;
  fbits b; b.f = x; int e = (int)((b.i >> 23) & 255) - 127; b.i = (b.i & 0x007FFFFF) | 0x3F800000;
  float m = b.f;   /* 1..2 */
  float p = -1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m;   /* ≈ ln(m) */
  return p * 1.4426950f + (float)e;                                       /* ln → log2 (fixed 19.10.2026: was ln) */
}
float vd_pow(float a, float b) { return vd_exp2(b * vd_log2(a)); }
float vd_tanh(float x) { if (x > 3) return 1; if (x < -3) return -1; float x2 = x * x; return x * (27 + x2) / (27 + 9 * x2); }
float vd_sqrt(float x) { return x <= 0 ? 0 : __builtin_sqrtf(x); }
float vd_mtof(float n) { return 440.0f * vd_exp2((n - 69.0f) / 12.0f); }
float vd_coef(float s, float sr) { if (s <= 0) return 0; return vd_exp2(-1.442695f / (s * sr)); }

static uint32_t rng = 0x12345678u;
uint32_t vd_rand(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

float vd_blep(float t, float dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
  return 0.0f;
}
float vd_saw(float ph, float dt) { return 2.0f * ph - 1.0f - vd_blep(ph, dt); }
float vd_square(float ph, float dt) {
  float v = ph < 0.5f ? 1.0f : -1.0f; float p2 = ph + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
  return v + vd_blep(ph, dt) - vd_blep(p2, dt);
}
void vd_svf_set(vd_svf_c *c, float fc, float q, float sr) {
  fc = vd_clamp(fc, 10.0f, sr * 0.45f);
  float ph = fc / sr * 0.5f;                               /* tan(pi*fc/sr) as sin/cos, phase in turns */
  float g = vd_sin(ph) / vd_sin(ph + 0.25f);
  c->k = 1.0f / vd_maxf(q, 0.3f);
  c->a1 = 1.0f / (1.0f + g * (g + c->k)); c->a2 = g * c->a1; c->a3 = g * c->a2;
}
vd_svf_out vd_svf_run(vd_svf *f, float in, float fc, float q, float sr) { vd_svf_c c; vd_svf_set(&c, fc, q, sr); return vd_svf_tick(f, &c, in); }
void vd_env_set(vd_env *e, float a, float d, float s, float r, float sr) {
  e->a_inc = a > 0.0005f ? 1.0f / (a * sr) : 1.0f; e->d_coef = vd_coef(d / 3.0f, sr); e->r_coef = vd_coef(r / 3.0f, sr); e->sustain = s;
}
void vd_env_gate(vd_env *e, int on) { e->stage = on ? 1 : (e->stage ? 3 : 0); }
float vd_env_next(vd_env *e) {
  switch (e->stage) {
    case 1: e->level += e->a_inc; if (e->level >= 1.0f) { e->level = 1.0f; e->stage = 2; } break;
    case 2: e->level = e->sustain + (e->level - e->sustain) * e->d_coef; break;
    case 3: e->level *= e->r_coef; if (e->level < 0.0001f) { e->level = 0; e->stage = 0; } break;
    default: e->level = 0;
  }
  return e->level;
}

/* ================================================================ 4. the host glue ============================== */
/* the functions a runtime calls (browser: exported from the .wasm; device: linked into vibe-runtime, runtime/linux).
   VIBE_HOST_ONLY: a host that runs .wasm files (vibe-host, WAMR) takes only the drawing from this file, not the glue */
#ifndef VIBE_HOST_ONLY
#ifndef __wasm__
#define VIBE_EXPORT(n)
#endif
static struct { float in[VIBE_MAX_BLOCK], l[VIBE_MAX_BLOCK], r[VIBE_MAX_BLOCK], in2[VIBE_MAX_BLOCK]; } io;
static uint16_t framebuffer[VIBE_SCREEN_W * VIBE_SCREEN_H];

VIBE_EXPORT(vibe_sdk_version) int vibe_sdk_version(void) { return VIBE_HOST_PROTOCOL; }   /* (the host protocol: 7 – SDK 8 adds only optional exports) */
/* SDK 5: the time table the host fills (VIBE FM Protocol 7.1), the frame counter, the flows */
static struct { uint32_t heard_lo, heard_hi, latency_out, latency_in, flags, pad[3]; } vtime;
static float sdk_sr = 48000.0f;                               /* (SDK 9: the rate, for the clock out) */
static volatile uint32_t frames_lo, frames_hi, frames_seq;   /* written by the audio flow only (a seqlock: odd = busy) */
uint64_t vibe_frames(void)
{
  uint32_t a, b, h, l;
  do { a = frames_seq; VIBE_ACQUIRE(); h = frames_hi; l = frames_lo; VIBE_ACQUIRE(); b = frames_seq; } while (a != b || (a & 1u));
  return (uint64_t)h << 32 | l;
}
int vibe_event_heard(uint64_t *frame)
{
  if (!(vtime.flags & 1u)) return 0;
  *frame = (uint64_t)vtime.heard_hi << 32 | vtime.heard_lo;
  return 1;
}
int vibe_latency_out(void) { return (int)vtime.latency_out; }
int vibe_latency_in(void) { return (int)vtime.latency_in; }
__attribute__((weak)) int vibe_on_flows(void) { return 1; }   /* VIBE_PARALLEL_AUDIO in a firmware: 2 */
VIBE_EXPORT(vibe_wasm_time) void *vibe_wasm_time(void) { return &vtime; }
VIBE_EXPORT(vibe_wasm_flows) int vibe_wasm_flows(void) { return vibe_on_flows(); }

/* ---- SDK 17 (Protokoll 8): worker jobs – fork and join inside one audio block ----------------------------------------
 * No import: a host with workers starts vibe_wasm_worker(w) once per worker, each in a flow of its own with its own stack
 * (WAMR: wasm_runtime_spawn_exec_env), pinned to a core of its own; it loops here until vibe_wasm_workers_stop. Job w runs
 * on worker w. vibe_jobs_begin bumps jb_go[w] and wakes it (memory.atomic.notify); the worker spins a little, then sleeps
 * in memory.atomic.wait (a futex). vibe_jobs_wait spins (the audio flow never sleeps). Jobs without a live worker – no
 * host workers at all: the plugin, the browser, an older VIBE – run right there in vibe_jobs_begin, in order. */
__attribute__((weak)) int vibe_on_jobs(void) { return 0; }                 /* VIBE_JOBS(n) in a firmware: n (0..3) */
__attribute__((weak)) void vibe_on_job(int k, int frames) { (void)k; (void)frames; }
static int32_t jb_n = -1;                         /* the firmware's jobs, asked once */
static int32_t jb_go[3], jb_alive, jb_done, jb_par, jb_quit, jb_frames, jb_injob, jb_reverse;
static int jb_count(void) { if (jb_n < 0) { int n = vibe_on_jobs(); jb_n = n < 0 ? 0 : n > 3 ? 3 : n; } return jb_n; }
static void jb_inline(unsigned m, int frames)
{
  for (int i = 0; i < 3; i++) {
    int k = jb_reverse ? 2 - i : i;
    if (m >> k & 1u) { jb_injob = k + 1; vibe_on_job(k, frames); jb_injob = 0; }
  }
}
void vibe_jobs_begin(unsigned mask, int frames)
{
  mask &= (1u << jb_count()) - 1u;
  unsigned par = mask & (unsigned)__atomic_load_n(&jb_alive, __ATOMIC_ACQUIRE);
  jb_par = (int32_t)par;
  if (par) {
    __atomic_store_n(&jb_frames, frames, __ATOMIC_RELAXED);
    __atomic_store_n(&jb_done, 0, __ATOMIC_RELEASE);
#if defined(__wasm__) && defined(__wasm_atomics__)
    for (int k = 0; k < 3; k++) if (par >> k & 1u) { __atomic_fetch_add(&jb_go[k], 1, __ATOMIC_SEQ_CST); __builtin_wasm_memory_atomic_notify(&jb_go[k], 1); }
#endif
  }
  jb_inline(mask & ~par, frames);                 /* (no worker for these: here, now) */
}
void vibe_jobs_wait(void)
{
  unsigned par = (unsigned)jb_par; if (!par) return;
  while (((unsigned)__atomic_load_n(&jb_done, __ATOMIC_ACQUIRE) & par) != par) { }   /* (spin: microseconds, never a sleep in audio) */
  jb_par = 0;
}
VIBE_EXPORT(vibe_wasm_workers) int vibe_wasm_workers(void) { return jb_count(); }
VIBE_EXPORT(vibe_wasm_job) void vibe_wasm_job(int k, int frames)   /* (one job, called directly: a host's own pool, a test) */
{
  if (k >= 0 && k < jb_count() && frames > 0) { jb_injob = k + 1; vibe_on_job(k, frames); jb_injob = 0; }
}
VIBE_EXPORT(vibe_wasm_worker) int vibe_wasm_worker(int w)   /* (the host's worker w: back only after _stop – or -1) */
{
  if (w < 0 || w >= jb_count()) return -1;
#if defined(__wasm__) && defined(__wasm_atomics__)
  int32_t seen = __atomic_load_n(&jb_go[w], __ATOMIC_ACQUIRE);
  __atomic_fetch_or(&jb_alive, 1 << w, __ATOMIC_SEQ_CST);
  for (;;) {
    int spin = 4000;                              /* (a moment awake: the next block is often only microseconds away) */
    while (__atomic_load_n(&jb_go[w], __ATOMIC_ACQUIRE) == seen) {
      if (__atomic_load_n(&jb_quit, __ATOMIC_ACQUIRE)) { __atomic_fetch_and(&jb_alive, ~(1 << w), __ATOMIC_SEQ_CST); return 0; }
      if (spin > 0) spin--; else __builtin_wasm_memory_atomic_wait32(&jb_go[w], seen, -1);
    }
    seen = __atomic_load_n(&jb_go[w], __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&jb_quit, __ATOMIC_ACQUIRE)) { __atomic_fetch_and(&jb_alive, ~(1 << w), __ATOMIC_SEQ_CST); return 0; }
    vibe_on_job(w, __atomic_load_n(&jb_frames, __ATOMIC_RELAXED));
    __atomic_fetch_or(&jb_done, 1 << w, __ATOMIC_RELEASE);
  }
#else
  return -1;                                      /* (a build without atomics: no workers, jobs in line) */
#endif
}
VIBE_EXPORT(vibe_wasm_workers_stop) void vibe_wasm_workers_stop(void)   /* (the host, before it lets the instance go) */
{
  __atomic_store_n(&jb_quit, 1, __ATOMIC_SEQ_CST);
#if defined(__wasm__) && defined(__wasm_atomics__)
  for (int k = 0; k < 3; k++) { __atomic_fetch_add(&jb_go[k], 1, __ATOMIC_SEQ_CST); __builtin_wasm_memory_atomic_notify(&jb_go[k], 1); }
#endif
}
VIBE_EXPORT(vibe_wasm_worker_lost) void vibe_wasm_worker_lost(int w)   /* (its job trapped: it is gone – a wait must not hang) */
{
  if (w < 0 || w > 2) return;
  __atomic_fetch_and(&jb_alive, ~(1 << w), __ATOMIC_SEQ_CST);
  __atomic_fetch_or(&jb_done, 1 << w, __ATOMIC_RELEASE);
}
/* tests (wasmtest, vibe-check): the jobs in line in reverse order – a job that leans on another's memory then sounds
   different; and which job runs in line now (a job must not reach the host: no vibe_host_* while this is > 0) */
VIBE_EXPORT(vibe_wasm_jobs_reverse) void vibe_wasm_jobs_reverse(int on) { jb_reverse = on; }
VIBE_EXPORT(vibe_wasm_in_job) int vibe_wasm_in_job(void) { return jb_injob; }
VIBE_EXPORT(vibe_wasm_init) void vibe_wasm_init(float sr) { sdk_sr = sr > 0.0f ? sr : 48000.0f; vibe_on_init(sr); }
/* ---- SDK 8: notes – the keys, the pads and MIDI's note on / off become VIBE_EV_NOTE (vibe_notes) -------------------- */
static int notes_flags, keys_oct = 1;
static int8_t key_note[VIBE_KEYS], pad_note[VIBE_PADS_MAX];   /* the note each held key / pad started (-1 = none) */
void vibe_notes(int flags)
{
  notes_flags = flags;
  for (int i = 0; i < VIBE_KEYS; i++) key_note[i] = -1;
  for (int i = 0; i < VIBE_PADS_MAX; i++) pad_note[i] = -1;
}
void vibe_keys_octave(int o) { keys_oct = o < 0 ? 0 : o > 7 ? 7 : o; }
int  vibe_keys_octave_get(void) { return keys_oct; }
static void note_ev(int note, int down, float vel, int ch, int src)
{
  vibe_event n = { VIBE_EV_NOTE, (uint8_t)note, (uint8_t)(down != 0), { (uint8_t)ch, (uint8_t)src, 0 }, down ? vel : 0.0f };
  vibe_on_event(&n);
}
/* 1 = the SDK took it (sent as a note, or used for the octave) */
static int notes_take(const vibe_event *e)
{
  if (!(notes_flags & VIBE_NOTES)) return 0;
  if (e->type == VIBE_EV_KEY && e->id < VIBE_KEYS) {
    if (e->down) { int n = 36 + 12 * keys_oct + e->id; if (n > 127) n = 127; key_note[e->id] = (int8_t)n; note_ev(n, 1, e->value, 0, VIBE_NOTE_KEYS); }
    else if (key_note[e->id] >= 0) { note_ev(key_note[e->id], 0, 0.0f, 0, VIBE_NOTE_KEYS); key_note[e->id] = -1; }
    return 1;
  }
  if (e->type == VIBE_EV_PAD && (notes_flags & VIBE_NOTES_PADS) && e->id < VIBE_PADS_MAX && 36 + e->id <= 127) {   /* (SDK 20: a bigger grid's pads too, as far as notes go) */
    if (e->down) { pad_note[e->id] = (int8_t)(36 + e->id); note_ev(36 + e->id, 1, e->value, 9, VIBE_NOTE_PADS); }
    else if (pad_note[e->id] >= 0) { note_ev(pad_note[e->id], 0, 0.0f, 9, VIBE_NOTE_PADS); pad_note[e->id] = -1; }
    return 1;
  }
  if (e->type == VIBE_EV_MIDI) {
    int st = e->midi[0] & 0xF0, ch = e->midi[0] & 15;
    if (st == 0x90 && e->midi[2]) { note_ev(e->midi[1] & 127, 1, (float)e->midi[2] / 127.0f, ch, VIBE_NOTE_MIDI); return 1; }
    if (st == 0x80 || st == 0x90) { note_ev(e->midi[1] & 127, 0, 0.0f, ch, VIBE_NOTE_MIDI); return 1; }
    return 0;
  }
  if (e->type == VIBE_EV_BUTTON && e->down && !(notes_flags & VIBE_NOTES_OWN_OCTAVE)) {
    if (e->id == VB_OCT_DOWN) vibe_keys_octave(keys_oct - 1);   /* (the button still reaches the firmware: it may show it) */
    if (e->id == VB_OCT_UP) vibe_keys_octave(keys_oct + 1);
  }
  return 0;
}

/* ---- SDK 8: parameters ------------------------------------------------------------------------------------------- */
static const vibe_param *params;
static int nparams;
static float pvalue[VIBE_MAX_PARAMS];
static volatile uint32_t pchanged[VIBE_MAX_PARAMS / 32];     /* set by the firmware's vibe_param_set, read and cleared by the host */
int vibe_params(const vibe_param *list, int count)
{
  params = list; nparams = count < 0 ? 0 : count > VIBE_MAX_PARAMS ? VIBE_MAX_PARAMS : count;
  for (int i = 0; i < nparams; i++) pvalue[i] = list[i].def;
  return nparams;
}
static float pclamp(int i, float v) { float a = params[i].min, b = params[i].max; if (a > b) { float t = a; a = b; b = t; } return v < a ? a : v > b ? b : v; }
void  vibe_param_set(int i, float v) { if (i < 0 || i >= nparams) return; pvalue[i] = pclamp(i, v); pchanged[i >> 5] |= 1u << (i & 31); }
float vibe_param_get(int i) { return i >= 0 && i < nparams ? pvalue[i] : 0.0f; }
/* the host's side: how many, each one's description (its vibe_param in memory: 4-byte pointers on wasm32), the value,
   set from outside (→ VIBE_EV_PARAM to the firmware), and which ones the firmware changed since the host last asked */
VIBE_EXPORT(vibe_wasm_param_count) int vibe_wasm_param_count(void) { return nparams; }
VIBE_EXPORT(vibe_wasm_param_info) const void *vibe_wasm_param_info(int i) { return i >= 0 && i < nparams ? (const void *)&params[i] : 0; }
VIBE_EXPORT(vibe_wasm_param_value) float vibe_wasm_param_value(int i) { return vibe_param_get(i); }
static const char *const *pgroups;                         /* (SDK 8: an editor's groups – vibe_param_groups) */
void vibe_param_groups(const char *const *groups) { pgroups = groups; }
VIBE_EXPORT(vibe_wasm_param_group) const char *vibe_wasm_param_group(int i) { return pgroups && i >= 0 && i < nparams ? pgroups[i] : 0; }
VIBE_EXPORT(vibe_wasm_param_set) void vibe_wasm_param_set(int i, float v) {
  if (i < 0 || i >= nparams) return;
  pvalue[i] = pclamp(i, v);
  vibe_event e = { VIBE_EV_PARAM, (uint8_t)i, 0, { 0, 0, 0 }, pvalue[i] };
  vibe_on_event(&e);
}
VIBE_EXPORT(vibe_wasm_param_changed) uint32_t vibe_wasm_param_changed(int word) {   /* 32 parameters a word; cleared on read */
  if (word < 0 || word >= VIBE_MAX_PARAMS / 32) return 0;
  uint32_t m = pchanged[word]; pchanged[word] &= ~m; return m;
}

/* ---- SDK 8: the host's position – a table the host fills before each audio block (found by its export's name) ------- */
static vibe_transport_info vtransport = { 0, 0, 0.0f, 4, 4, 0.0, 0.0, 0, 0.0, 0.0 };
const vibe_transport_info *vibe_transport(void) { return &vtransport; }
VIBE_EXPORT(vibe_wasm_transport) void *vibe_wasm_transport(void) { return &vtransport; }

/* ---- SDK 9: MIDI out – a queue in the firmware's memory the host drains after every audio block (Protokoll 7.4) ----
   One writer (the firmware, or the SDK's clock), one reader (the host): data, VIBE_RELEASE, then the counter (6.2).
   Layout (wasm32): w @0 · r @4 · dropped @8 · sysex_len @12 · q[256] @16 (6 bytes each: frame u16, st, d1, d2, len) ·
   sysex[256] @1552. A message with len 0 and st 0xF0 is the SysEx buffer (sysex_len bytes, F0 … F7). */
typedef struct { uint16_t frame; uint8_t st, d1, d2, len; } vibe_midi_msg;
#define MOUT_N 256
static struct { volatile uint32_t w, r, dropped, sysex_len; vibe_midi_msg q[MOUT_N]; uint8_t sysex[256]; } mout;
static int mout_in_audio, mout_frames = 1;          /* inside vibe_on_audio: this block's length (for _at) */
static int mout_push(int st, int d1, int d2, int len, int frame)
{
  uint32_t w = mout.w;
  if (w - mout.r >= MOUT_N) { mout.dropped++; return -1; }
  if (frame < 0) frame = 0; else if (frame >= mout_frames) frame = mout_frames - 1;
  vibe_midi_msg *m = &mout.q[w % MOUT_N];
  m->frame = (uint16_t)frame; m->st = (uint8_t)st; m->d1 = (uint8_t)d1; m->d2 = (uint8_t)d2; m->len = (uint8_t)len;
  VIBE_RELEASE(); mout.w = w + 1u;
  return 0;
}
static int midi_len(int st) { int h = st & 0xF0; return st >= 0xF8 ? 1 : h == 0xC0 || h == 0xD0 ? 2 : st == 0xF1 || st == 0xF3 ? 2 : st == 0xF2 ? 3 : st == 0xF6 ? 1 : st < 0xF0 ? 3 : 0; }
int vibe_midi_send_at(uint8_t st, uint8_t d1, uint8_t d2, int frame)
{
  int n = midi_len(st);
  if (st < 0x80 || !n || (n > 1 && d1 > 127) || (n > 2 && d2 > 127)) return -1;
  return mout_push(st, n > 1 ? d1 : 0, n > 2 ? d2 : 0, n, mout_in_audio ? frame : 0);
}
int vibe_midi_send(uint8_t st, uint8_t d1, uint8_t d2) { return vibe_midi_send_at(st, d1, d2, 0); }
int vibe_midi_send_sysex(const uint8_t *b, int n)
{
  if (!b || n < 2 || n > 256 || b[0] != 0xF0 || b[n - 1] != 0xF7 || mout_in_audio) return -1;
  if (mout.w - mout.r >= MOUT_N || mout.sysex_len) return -1;   /* (one SysEx at a time: the host takes it with the block) */
  for (int i = 0; i < n; i++) mout.sysex[i] = b[i];
  mout.sysex_len = (uint32_t)n;
  return mout_push(0xF0, 0, 0, 0, 0);
}
VIBE_EXPORT(vibe_wasm_midi_out) void *vibe_wasm_midi_out(void) { return &mout; }
/* the clock: 24 pulses a quarter at the firmware's tempo, made frame-exact before each block; silent while a host leads */
static int clk_on, clk_run; static float own_bpm = 120.0f, host_bpm; static double clk_phase;
void  vibe_clock_out(int on) { clk_on = on != 0; if (!on) clk_phase = 0.0; }
void  vibe_tempo_set(float bpm) { own_bpm = bpm < 20.0f ? 20.0f : bpm > 400.0f ? 400.0f : bpm; }
float vibe_tempo_get(void) { return own_bpm; }
void  vibe_transport_out(int playing)
{
  if (playing) { clk_phase = 1.0; clk_run = 1; mout_push(0xFA, 0, 0, 1, 0); }   /* (phase 1: the first F8 on the next block's frame 0) */
  else { clk_run = 0; mout_push(0xFC, 0, 0, 1, 0); }
}
static void clk_block(int frames)
{
  if (!clk_on || host_bpm > 0.0f || (vtransport.has && vtransport.playing && vtransport.bpm > 0.0f)) { clk_phase = 0.0; return; }
  double per = (double)own_bpm * 24.0 / 60.0 / (double)sdk_sr;   /* pulses a frame */
  for (int f = 0; f < frames; f++) {
    clk_phase += per;
    if (clk_phase >= 1.0) { clk_phase -= 1.0; mout_push(0xF8, 0, 0, 1, f); }
  }
  (void)clk_run;
}

/* ---- SDK 9: outputs – dual mono and the headphones' own signal (Protokoll 8.1). The host reads the table once after
   init (flags, names) and the cue buffers after each block; it writes host_cue (1 = it plays the cue on its own outputs).
   Layout (wasm32): flags @0 · host_cue @4 · cue_written @8 (this block) · name_l @12, name_r @16 (ptr) · cue @20: 2 x 256 f32 */
static struct { int32_t flags, host_cue; volatile int32_t cue_written; const char *name_l, *name_r; float cue[2][VIBE_MAX_BLOCK]; } vout;
void  vibe_outputs(int flags) { vout.flags = flags & (VIBE_OUT_DUAL_MONO | VIBE_OUT_CUE); }
void  vibe_output_names(const char *l, const char *r) { vout.name_l = l; vout.name_r = r; }
float *vibe_cue(int c) { if (!(vout.flags & VIBE_OUT_CUE) || c < 0 || c > 1) return 0; vout.cue_written = 1; return vout.cue[c]; }
int   vibe_cue_host(void) { return vout.host_cue; }
VIBE_EXPORT(vibe_wasm_outputs) void *vibe_wasm_outputs(void) { return &vout; }
VIBE_EXPORT(vibe_wasm_ports) void *vibe_wasm_ports(void) { return &ports; }   /* (SDK 26) more inputs and outputs */
/* ---- SDK 12: the stream (Spotify Connect) – a table the host fills: the info (vibe_stream_info, 340 bytes), then
   w @340, r @344 (frames, ever counting), L[STREAM_N] @348, R[STREAM_N] after it. The host writes frames and moves w,
   the firmware reads them and moves r. */
#define STREAM_N 16384
static struct { vibe_stream_info info; volatile uint32_t w, r; float L[STREAM_N], R[STREAM_N]; } stream;
_Static_assert(sizeof(vibe_stream_info) == 340, "the stream table's layout is the hosts' (w @340)");
VIBE_EXPORT(vibe_wasm_stream) void *vibe_wasm_stream(void) { return &stream; }
static int fw_areas;                              /* (SDK 12) vibe_areas: the host sends VIBE_EV_AREA for − / + */
void vibe_areas(int count) { fw_areas = count > 0 ? count : 0; }
VIBE_EXPORT(vibe_wasm_areas) int vibe_wasm_areas(void) { return fw_areas; }
static int fw_extras;                             /* (SDK 19 extras, 8.11.2026) vibe_extras_own: VB_X1 .. reach the firmware as buttons */
void vibe_extras_own(int on) { fw_extras = on ? 1 : 0; }
VIBE_EXPORT(vibe_wasm_extras) int vibe_wasm_extras(void) { return fw_extras; }
const vibe_stream_info *vibe_stream(void) { return &stream.info; }
int vibe_stream_read(float *l, float *r, int max)
{
  uint32_t w = stream.w; VIBE_ACQUIRE();
  uint32_t rd = stream.r, n = w - rd; if (n > (uint32_t)max) n = (uint32_t)max; if (n > STREAM_N) n = 0;
  for (uint32_t i = 0; i < n; i++) { l[i] = stream.L[(rd + i) % STREAM_N]; r[i] = stream.R[(rd + i) % STREAM_N]; }
  VIBE_RELEASE(); stream.r = rd + n;
  return (int)n;
}

/* ---- SDK 9: the Editor's description – tables the host reads once after vibe_on_init (Protokoll 9.2) ---------------- */
typedef struct { const char *tab; uint32_t rgb; } vibe_tab_color;
static struct {
  uint32_t version;                 /* 1 */
  const vibe_card *cards; int32_t ncards;
  const uint8_t *controls;          /* VIBE_MAX_PARAMS (enum vibe_control) */
  uint32_t accent;                  /* 0 = none */
  const vibe_tab_color *tabs; int32_t ntabs;
  const char *const *help;          /* VIBE_MAX_PARAMS pointers */
  int32_t ncurves;
} ed;
static uint8_t ed_ctl[VIBE_MAX_PARAMS]; static const char *ed_help[VIBE_MAX_PARAMS];
static vibe_tab_color ed_tabc[8];
static struct { const char *card; vibe_curve_fn fn; } ed_curve[16];
static float ed_pts[64];
void vibe_editor_cards(const vibe_card *c, int n) { ed.version = 1; ed.cards = c; ed.ncards = n < 0 ? 0 : n > 64 ? 64 : n; }
void vibe_editor_control(int i, int c) { if (i >= 0 && i < VIBE_MAX_PARAMS && c >= 0 && c <= VIBE_CTL_XY_Y) { ed.version = 1; ed_ctl[i] = (uint8_t)c; ed.controls = ed_ctl; } }
void vibe_editor_accent(uint32_t rgb) { ed.version = 1; ed.accent = rgb & 0xFFFFFFu; }
void vibe_editor_tab_color(const char *t, uint32_t rgb)
{
  if (!t) return;
  for (int i = 0; i < ed.ntabs; i++) if (ed_tabc[i].tab == t) { ed_tabc[i].rgb = rgb & 0xFFFFFFu; return; }
  if (ed.ntabs < 8) { ed_tabc[ed.ntabs].tab = t; ed_tabc[ed.ntabs].rgb = rgb & 0xFFFFFFu; ed.ntabs++; ed.tabs = ed_tabc; ed.version = 1; }
}
void vibe_editor_curve(const char *card, vibe_curve_fn fn) { if (card && fn && ed.ncurves < 16) { ed_curve[ed.ncurves].card = card; ed_curve[ed.ncurves].fn = fn; ed.ncurves++; ed.version = 1; } }
void vibe_param_help(int i, const char *t) { if (i >= 0 && i < VIBE_MAX_PARAMS) { ed.version = 1; ed_help[i] = t; ed.help = ed_help; } }
VIBE_EXPORT(vibe_wasm_editor) const void *vibe_wasm_editor(void) { return ed.version ? (const void *)&ed : 0; }
VIBE_EXPORT(vibe_wasm_editor_curve_card) const char *vibe_wasm_editor_curve_card(int i) { return i >= 0 && i < ed.ncurves ? ed_curve[i].card : 0; }
VIBE_EXPORT(vibe_wasm_editor_curve) int vibe_wasm_editor_curve(int i, const char *tab)   /* fills vibe_wasm_editor_points(); returns their count */
{
  if (i < 0 || i >= ed.ncurves || !ed_curve[i].fn) return 0;
  int n = ed_curve[i].fn(tab ? tab : "", ed_curve[i].card, ed_pts, 64);
  if (n < 2) return 0; if (n > 64) n = 64;
  for (int k = 0; k < n; k++) ed_pts[k] = ed_pts[k] < 0.0f ? 0.0f : ed_pts[k] > 1.0f ? 1.0f : ed_pts[k];
  return n;
}
VIBE_EXPORT(vibe_wasm_editor_points) const float *vibe_wasm_editor_points(void) { return ed_pts; }
static char ed_tabbuf[32];                        /* (the host writes the tab's name here and passes its address to _curve) */
VIBE_EXPORT(vibe_wasm_editor_tabbuf) char *vibe_wasm_editor_tabbuf(void) { return ed_tabbuf; }

/* ---- SDK 15: the Editor talks back (Protokoll 9.3) – a curve drawn, a button clicked; SysEx in (Protokoll 7.5) -------
 * The host calls these in the firmware's flow (where it calls vibe_wasm_event), the tab's name in vibe_wasm_editor_tabbuf. */
static vibe_curve_draw_fn ed_draw[16];
static struct { const char *card, *label; int32_t id; } ed_btn[16];   /* 12 bytes each: card ptr, label ptr, id */
static int32_t ed_nbtn;
static char ed_evtab[32];
void vibe_editor_curve_draw(const char *card, vibe_curve_draw_fn fn)
{
  if (!card || !fn) return;
  for (int i = 0; i < ed.ncurves; i++) {
    const char *a = ed_curve[i].card, *b = card; while (*a && *a == *b) a++, b++;
    if (*a == *b) { ed_draw[i] = fn; return; }
  }
}
void vibe_editor_button(const char *card, const char *label, int id)
{
  if (card && label && ed_nbtn < 16) { ed_btn[ed_nbtn].card = card; ed_btn[ed_nbtn].label = label; ed_btn[ed_nbtn].id = id; ed_nbtn++; ed.version = 1; }
}
const char *vibe_editor_event_tab(void) { return ed_evtab; }
VIBE_EXPORT(vibe_wasm_editor_curve_drawable) int vibe_wasm_editor_curve_drawable(int i) { return i >= 0 && i < ed.ncurves && ed_draw[i] ? 1 : 0; }
VIBE_EXPORT(vibe_wasm_editor_curve_draw) void vibe_wasm_editor_curve_draw(int i, const char *tab, int point, int count, float value)
{
  if (i < 0 || i >= ed.ncurves || !ed_draw[i] || count < 2 || count > 64 || point < 0 || point >= count) return;
  ed_draw[i](tab ? tab : "", ed_curve[i].card, point, count, value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value);
}
VIBE_EXPORT(vibe_wasm_editor_buttons) int vibe_wasm_editor_buttons(void) { return ed_nbtn; }
VIBE_EXPORT(vibe_wasm_editor_button) const void *vibe_wasm_editor_button(int i) { return i >= 0 && i < ed_nbtn ? (const void *)&ed_btn[i] : 0; }
VIBE_EXPORT(vibe_wasm_editor_press) void vibe_wasm_editor_press(int i, const char *tab)
{
  if (i < 0 || i >= ed_nbtn) return;
  int n = 0; if (tab) while (tab[n] && n < 31) { ed_evtab[n] = tab[n]; n++; }
  ed_evtab[n] = 0;
  vibe_event e = { VIBE_EV_EDITOR, (uint8_t)ed_btn[i].id, 1, { 0, 0, 0 }, 0 }; vibe_on_event(&e);
}
static uint8_t sx_in[4096];
const uint8_t *vibe_sysex_in(void) { return sx_in; }
VIBE_EXPORT(vibe_wasm_sysex_buf) uint8_t *vibe_wasm_sysex_buf(void) { return sx_in; }   /* (the host writes the bytes here …) */
VIBE_EXPORT(vibe_wasm_sysex_in) void vibe_wasm_sysex_in(int n)                       /* (… and says how many) */
{
  if (n < 2 || n > (int)sizeof sx_in || sx_in[0] != 0xF0) return;
  vibe_event e = { VIBE_EV_SYSEX, 0, 1, { 0, 0, 0 }, (float)n }; vibe_on_event(&e);
}

/* ---- SDK 16: bytes between the firmware and its own Editor page (Protokoll 9.4) ----------------------------------
 * In: the host writes into the inbox and calls _data_in(n) in the firmware's flow. Out: { u32 len, u8 bytes[4096] } –
 * the firmware fills bytes, then len; the host takes them after a block or an event and sets len back to 0. */
static uint8_t ed_in[4096];
static struct { volatile uint32_t len; uint8_t b[4096]; } ed_out;
const uint8_t *vibe_editor_data(void) { return ed_in; }
int vibe_editor_post(const uint8_t *bytes, int n)
{
  if (!bytes || n < 1 || n > (int)sizeof ed_out.b || ed_out.len) return -1;
  for (int i = 0; i < n; i++) ed_out.b[i] = bytes[i];
  ed_out.len = (uint32_t)n;
  return 0;
}
VIBE_EXPORT(vibe_wasm_editor_inbox) uint8_t *vibe_wasm_editor_inbox(void) { return ed_in; }
VIBE_EXPORT(vibe_wasm_editor_data_in) void vibe_wasm_editor_data_in(int n)
{
  if (n < 1 || n > (int)sizeof ed_in) return;
  vibe_event e = { VIBE_EV_EDITOR_DATA, 0, 1, { 0, 0, 0 }, (float)n }; vibe_on_event(&e);
}
VIBE_EXPORT(vibe_wasm_editor_outbox) void *vibe_wasm_editor_outbox(void) { return (void *)&ed_out; }

/* SDK 12: SHIFT + DATA = the soft keys, in every firmware (Sven 28.10.: a controller without F keys – an MPK mini – reaches
 * them): while SHIFT is held, DATA moves a frame over F1-F6 (drawn over the firmware's picture, at the house layout's
 * places), DATA pressed (or ENTER) presses that key – SHIFT let go for it first, so it is the plain F key. The firmware
 * sees neither the turns nor that press. (OberVave's and GROOVE's SHIFT + DATA tempo moved to their GLOBAL pages.) */
static int sk_shift, sk_on, sk_cur, sk_swallow = -1; static float sk_acc;
static void sk_press(void)
{
  vibe_event f = { VIBE_EV_BUTTON, (uint8_t)(VB_F1 + sk_cur), 1, { 0, 0, 0 }, 0.9f }; vibe_on_event(&f); f.down = 0; f.value = 0; vibe_on_event(&f);
  sk_on = 0;
}
static int sk_take(const vibe_event *e)          /* 1 = the soft-key gesture took it */
{
  if (e->type == VIBE_EV_BUTTON && e->id == sk_swallow) { if (!e->down) sk_swallow = -1; return 1; }   /* (the press's release) */
  if (e->type == VIBE_EV_BUTTON && e->id == VB_SHIFT) { sk_shift = e->down; vf_shift = e->down; if (!e->down) sk_on = 0; return 0; }
  if (vf_sw && sk_shift && e->type == VIBE_EV_ENCODER && e->id == VE_DATA && vf_md == VF_ON_HOLD && !vf_off) {   /* (vf_shift_walk: FOCUS instead of the soft-key frame) */
    vf_t = vibe_frames();
    if (!vf_on) { vf_on = 1; vf_edit = 0; vf_acc = 0; if (vf_index(vf_cur) < 0 && vf_n) vf_cur = VF[0].id; return 1; }   /* (the first turn: the frame shows) */
    return 0;                                    /* (on: the firmware's vf_event walks or edits) */
  }
  if (sk_shift && e->type == VIBE_EV_ENCODER && e->id == VE_DATA) {
    if (!sk_on) {                                /* the frame appears: SHIFT let go for the firmware (value -1 = "not a tap"), so it shows its plain keys – the ones pressed */
      sk_on = 1; sk_acc = 0;
      vibe_event u = { VIBE_EV_BUTTON, VB_SHIFT, 0, { 0, 0, 0 }, -1.0f }; vibe_on_event(&u);
    }
    sk_acc += e->value;
    while (sk_acc >= 1) { sk_acc -= 1; if (sk_cur < 5) sk_cur++; }
    while (sk_acc <= -1) { sk_acc += 1; if (sk_cur > 0) sk_cur--; }
    return 1;
  }
  if (sk_on && e->type == VIBE_EV_BUTTON && (e->id == VB_ENTER || e->id == VB_DATA_PUSH)) { if (e->down) { sk_swallow = e->id; sk_press(); } return 1; }
  return 0;
}
static void sk_draw(void)                        /* the frame over the soft key, its caption above */
{
  if (!sk_on || !FB) return;
  int x = 3 + sk_cur * 80;
  vg_frame(x - 2, 282, 78, 36, 3, 0xFF8A1F);
  char t[24] = "F"; t[1] = (char)('1' + sk_cur); t[2] = ' '; t[3] = 0;
  { int k = 3; for (int i = 0; labels[VB_F1 + sk_cur][i] && k < 12; i++) t[k++] = labels[VB_F1 + sk_cur][i]; t[k] = 0; }
  int w = vg_text_width(t, VG_BOLD) + 16, cx = x + 37 - w / 2; if (cx < 2) cx = 2; if (cx + w > VIBE_SCREEN_W - 2) cx = VIBE_SCREEN_W - 2 - w;
  vg_round(cx, 256, w, 22, 5, 0xFF8A1F); vg_text(t, cx + w / 2, 260, VG_BOLD, 0x111214, VG_CENTER);
}

/* SDK 10, VF_ON_HOLD: ENTER / DATA pressed held back until let go – short: to the firmware, long (0.6 s): FOCUS on / off */
static int vf_hold = -1, vf_fired; static uint64_t vf_hold_t;
static void vf_hold_check(void)
{
  if (vf_hold >= 0 && !vf_fired && vibe_frames() - vf_hold_t > (uint64_t)(sdk_sr * 0.6f)) { vf_fired = 1; vf_toggle(); }
}
VIBE_EXPORT(vibe_wasm_event) void vibe_wasm_event(int type, int id, int down, float value, int midi) {
  vibe_event e = { (uint8_t)type, (uint8_t)id, (uint8_t)down, { (uint8_t)(midi & 255), (uint8_t)((midi >> 8) & 255), (uint8_t)((midi >> 16) & 255) }, value };
  vf_hold_check();
  link_event(type, id, down);                    /* (SDK 24) */
  if (sk_take(&e)) return;                       /* (SDK 12: SHIFT + DATA = the soft keys) */
  if (vf_md == VF_ON_HOLD && type == VIBE_EV_BUTTON && (id == VB_ENTER || id == VB_DATA_PUSH)) {
    if (down) { if (vf_hold < 0) { vf_hold = id; vf_hold_t = vibe_frames(); vf_fired = 0; } return; }
    if (id != vf_hold) return;
    int fired = vf_fired; vf_hold = -1;
    if (fired) return;                            /* (the long press was FOCUS', the firmware never saw it) */
    e.down = 1; vibe_on_event(&e); e.down = 0;    /* a short press: down and up together */
  }
  if (type == VIBE_EV_TRANSPORT && id == VIBE_TRANSPORT_TEMPO) host_bpm = value > 0.0f ? value : 0.0f;   /* (SDK 9: a host leads – our clock out stays silent) */
  if (notes_take(&e)) return;
  vibe_on_event(&e);
}
VIBE_EXPORT(vibe_wasm_io) float *vibe_wasm_io(void) { return io.in; }       /* in | out_l | out_r | in2, VIBE_MAX_BLOCK each */
/* has_input: bit 0 = IN 1, bit 1 = IN 2 */
VIBE_EXPORT(vibe_wasm_audio) void vibe_wasm_audio(int frames, int has_input) {
  inputs[0] = (has_input & 1) ? io.in : 0; inputs[1] = (has_input & 2) ? io.in2 : 0;
  {                                             /* (SDK 26) IN 3.. and OUT 3.. of this block */
    int ni = vibe_input_count(), no = vibe_output_count(), f = frames > 0 && frames <= VIBE_MAX_BLOCK ? frames : VIBE_MAX_BLOCK;
    for (int k = VIBE_INPUTS; k < VIBE_IO_MAX; k++) inputs_x[k] = k < ni && (has_input >> k & 1) ? ports.in[k - 2] : 0;
    for (int k = 2; k < no; k++) for (int i = 0; i < f; i++) ports.out[k - 2][i] = 0.0f;
    ports.out_used = 0; outs_lr[0] = io.l; outs_lr[1] = io.r;
  }
  mout_frames = frames > 0 ? frames : 1; clk_block(mout_frames);   /* (SDK 9: the clock's pulses of this block, then the firmware's MIDI) */
  if (vout.flags & VIBE_OUT_CUE) { for (int i = 0; i < mout_frames && i < VIBE_MAX_BLOCK; i++) vout.cue[0][i] = vout.cue[1][i] = 0.0f; vout.cue_written = 0; }
  mout_in_audio = 1;
  vibe_on_audio(inputs[0], io.l, io.r, frames);
  mout_in_audio = 0;
  inputs[0] = inputs[1] = 0;
  for (int k = VIBE_INPUTS; k < VIBE_IO_MAX; k++) inputs_x[k] = 0;
  outs_lr[0] = outs_lr[1] = 0;
  {                                             /* the timeline moves on */
    uint32_t l = frames_lo + (uint32_t)frames;
    frames_seq = frames_seq + 1u; VIBE_RELEASE();
    if (l < frames_lo) frames_hi = frames_hi + 1u;
    frames_lo = l; VIBE_RELEASE();
    frames_seq = frames_seq + 1u;
  }
}
VIBE_EXPORT(vibe_wasm_rear) void *vibe_wasm_rear(void) { return &rear; }
VIBE_EXPORT(vibe_wasm_draw) uint16_t *vibe_wasm_draw(void) { vf_hold_check(); link_begin(); vg_begin(framebuffer); vibe_on_draw(framebuffer); vg_begin(framebuffer); sk_draw(); link_end(); return framebuffer; }

/* ---- SDK 23: the stage – a second, bigger picture the firmware draws itself (docs/sdk-23-buehne.md, Protokoll 9.5) ---- */
static struct { uint16_t w, h; uint32_t background; uint8_t want, flags, pad[2]; } stage_info;   /* (the host reads it once after init) */
static uint16_t *stage_fb;
__attribute__((weak)) void vibe_on_stage(uint16_t *fb, int w, int h) { (void)fb; (void)w; (void)h; }
int vibe_stage(uint16_t *fb, int w, int h, uint32_t background, int flags) {
  if (!fb || w < VIBE_STAGE_MIN_W || h < VIBE_STAGE_MIN_H || w > VIBE_STAGE_MAX_W || h > VIBE_STAGE_MAX_H) { stage_info.want = 0; stage_fb = 0; return 0; }
  stage_info.w = (uint16_t)w; stage_info.h = (uint16_t)h; stage_info.background = background; stage_info.want = 1; stage_info.flags = (uint8_t)flags; stage_fb = fb;
  return 1;
}
VIBE_EXPORT(vibe_wasm_stage_info) void *vibe_wasm_stage_info(void) { return &stage_info; }
VIBE_EXPORT(vibe_wasm_stage_draw) uint16_t *vibe_wasm_stage_draw(void) {
  if (!stage_info.want || !stage_fb) return 0;
  vg_begin_size(stage_fb, stage_info.w, stage_info.h); vibe_on_stage(stage_fb, stage_info.w, stage_info.h); vg_begin(framebuffer);
  return stage_fb;
}
VIBE_EXPORT(vibe_wasm_leds) void *vibe_wasm_leds(void) { return &leds; }
VIBE_EXPORT(vibe_wasm_pad_leds) void *vibe_wasm_pad_leds(void) { return &padx; }   /* SDK 20: n, rgb[128], lvl[128] (Protokoll 15b) */
VIBE_EXPORT(vibe_wasm_labels) void *vibe_wasm_labels(void) { return labels; }   /* VB_COUNT x 8 chars */
VIBE_EXPORT(vibe_wasm_link) void *vibe_wasm_link(void) { return &link_out; }   /* SDK 24: serial, page[20], name[7][12], value[7][12] */
VIBE_EXPORT(vibe_wasm_link_screen) void *vibe_wasm_link_screen(void) { return &link_scr; }   /* SDK 25: serial (+1 a change), px (0: none) */
#endif /* VIBE_HOST_ONLY */

/* ================================================================ 5. libc bits =================================== */
#ifdef __wasm__
/* weak: a ported firmware that brings its own (e.g. zp12's libc.c) wins */
__attribute__((weak)) void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
__attribute__((weak)) void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
__attribute__((weak)) int memcmp(const void *x, const void *y, size_t n) { const unsigned char *a = x, *b = y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }   /* (zig cc linked one in quietly; plain wasm-ld does not) */
__attribute__((weak)) void *memmove(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; } return d; }
/* (and the string calls a ported firmware may make without its own libc having them – weak as well) */
__attribute__((weak)) size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
__attribute__((weak)) int strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (unsigned char)*a - (unsigned char)*b; }
__attribute__((weak)) int strncmp(const char *a, const char *b, size_t n) { for (; n && *a && *a == *b; n--) a++, b++; return n ? (unsigned char)*a - (unsigned char)*b : 0; }
__attribute__((weak)) char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
__attribute__((weak)) char *strcat(char *d, const char *s) { char *r = d; while (*d) d++; while ((*d++ = *s++)) {} return r; }   /* (FIZZ's core) */
__attribute__((weak)) char *strncat(char *d, const char *s, size_t n) { char *r = d; while (*d) d++; while (n-- && *s) *d++ = *s++; *d = 0; return r; }
__attribute__((weak)) char *strncpy(char *d, const char *s, size_t n) { char *r = d; for (; n && *s; n--) *d++ = *s++; for (; n; n--) *d++ = 0; return r; }
__attribute__((weak)) char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }
__attribute__((weak)) void *memchr(const void *p, int c, size_t n) { const unsigned char *b = p; for (; n; n--, b++) if (*b == (unsigned char)c) return (void *)b; return 0; }
__attribute__((weak)) char *getenv(const char *n) { (void)n; return 0; }   /* (a host test's switches: none set on the VIBE) */
__attribute__((weak)) long strtol(const char *s, char **end, int base)
{
  long v = 0; int g = 1;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '-' || *s == '+') g = *s++ == '-' ? -1 : 1;
  if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 32) == 'x') { s += 2; base = 16; } else if (base == 0) base = s[0] == '0' ? 8 : 10;
  for (;; s++) { int d = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) >= 'a' && (*s | 32) <= 'z' ? (*s | 32) - 'a' + 10 : 99; if (d >= base) break; v = v * base + d; }
  if (end) *end = (char *)s;
  return v * g;
}
__attribute__((weak)) long atol(const char *s) { return strtol(s, 0, 10); }
__attribute__((weak)) int atoi(const char *s) { return (int)strtol(s, 0, 10); }
__attribute__((weak)) int abs(int v) { return v < 0 ? -v : v; }
__attribute__((weak)) long labs(long v) { return v < 0 ? -v : v; }
#endif

/* native: the inputs as pointers (the device runtime hands its capture buffers over directly) */
#ifndef __wasm__
void vibe_native_inputs(const float *in1, const float *in2) { inputs[0] = in1; inputs[1] = in2; }
#endif

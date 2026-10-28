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

void vibe_led_pad(int p, uint32_t rgb, float level) { if (p >= 0 && p < VIBE_PADS) { leds.pad_rgb[p] = rgb; leds.pad_lvl[p] = level; } }
void vibe_led_button(int b, uint32_t rgb, float level) { if (b >= 0 && b < VB_COUNT) { leds.btn_rgb[b] = rgb; leds.btn_lvl[b] = level; } }
void vibe_led_ring(int e, float v, int bipolar) { (void)e; (void)v; (void)bipolar; }   /* no rings on the device */
static char labels[VB_COUNT][8];
void vibe_label(int b, const char *t) { if (b < 0 || b >= VB_COUNT) return; int i = 0; for (; t && t[i] && i < 7; i++) labels[b][i] = t[i]; labels[b][i] = 0; }

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
const float *vibe_input(int i) { return i >= 0 && i < VIBE_INPUTS ? inputs[i] : 0; }

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
int vibe_store_list(char *buf, int max) { return buf && max > 0 ? vibe_host_store(4, "", 0, buf, max) : -1; }
int vibe_store_read_at(const char *n, int off, void *d, int bytes) {
  int l = store_name(n); if (!l || off < 0 || bytes < 4) return -1;   /* (4 bytes at least: the offset travels in them) */
  unsigned char *p = d; p[0] = (unsigned char)off; p[1] = (unsigned char)(off >> 8); p[2] = (unsigned char)(off >> 16); p[3] = (unsigned char)(off >> 24);
  return vibe_host_store(5, n, l, d, bytes);
}

/* ================================================================ 2. drawing ==================================== */
static uint16_t *FB;
#define W VIBE_SCREEN_W
#define H VIBE_SCREEN_H

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

void vg_begin(uint16_t *fb) { FB = fb; }
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
int vg_text_width(const char *t, int font) {
  const vf_font *f = &vf_fonts[font]; int w = 0;
  while (*t) w += glyph(f, utf8_next(&t))->adv;
  return w;
}
int vg_font_height(int font) { return vf_fonts[font].height; }
int vg_text(const char *t, int x, int y, int font, uint32_t c, int align) {
  const vf_font *f = &vf_fonts[font];
  int w = vg_text_width(t, font);
  if (align == VG_CENTER) x -= w / 2; else if (align == VG_RIGHT) x -= w;
  while (*t) {
    const vf_glyph *g = glyph(f, utf8_next(&t));
    const uint8_t *bits = vf_bits + g->off;
    for (int j = 0, k = 0; j < g->h; j++) for (int i = 0; i < g->w; i++, k++) {
      int v = (bits[k >> 1] >> ((k & 1) * 4)) & 15;
      if (v) blend(x + g->x + i, y + g->y + j, c, v / 15.0f);
    }
    x += g->adv;
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

/* ---- SDK 10: FOCUS (vibe_gfx.h) – the controls of the last frame, DATA walks them, ENTER uses them ---------------- */
#define VF_MAX 64
typedef struct { int id, x, y, w, h; float *v, lo, hi, step; } vf_item;
static vf_item VF[VF_MAX], VFN[VF_MAX]; static int vf_n, vf_nn, vf_cur = -1, vf_edit, vf_off, vf_md, vf_on; static float vf_acc;
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
int vf_event(const vibe_event *e, int *id)
{
  if (vf_off || !vf_n || !vf_active()) return VF_NO;
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
static uint16_t framebuffer[W * H];

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
VIBE_EXPORT(vibe_wasm_init) void vibe_wasm_init(float sr) { sdk_sr = sr > 0.0f ? sr : 48000.0f; vibe_on_init(sr); }
/* ---- SDK 8: notes – the keys, the pads and MIDI's note on / off become VIBE_EV_NOTE (vibe_notes) -------------------- */
static int notes_flags, keys_oct = 1;
static int8_t key_note[VIBE_KEYS], pad_note[VIBE_PADS];   /* the note each held key / pad started (-1 = none) */
void vibe_notes(int flags)
{
  notes_flags = flags;
  for (int i = 0; i < VIBE_KEYS; i++) key_note[i] = -1;
  for (int i = 0; i < VIBE_PADS; i++) pad_note[i] = -1;
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
  if (e->type == VIBE_EV_PAD && (notes_flags & VIBE_NOTES_PADS) && e->id < VIBE_PADS) {
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
  if (i < 0 || i >= ed.ncurves) return 0;
  int n = ed_curve[i].fn(tab ? tab : "", ed_curve[i].card, ed_pts, 64);
  if (n < 2) return 0; if (n > 64) n = 64;
  for (int k = 0; k < n; k++) ed_pts[k] = ed_pts[k] < 0.0f ? 0.0f : ed_pts[k] > 1.0f ? 1.0f : ed_pts[k];
  return n;
}
VIBE_EXPORT(vibe_wasm_editor_points) const float *vibe_wasm_editor_points(void) { return ed_pts; }
static char ed_tabbuf[32];                        /* (the host writes the tab's name here and passes its address to _curve) */
VIBE_EXPORT(vibe_wasm_editor_tabbuf) char *vibe_wasm_editor_tabbuf(void) { return ed_tabbuf; }

/* SDK 10, VF_ON_HOLD: ENTER / DATA pressed held back until let go – short: to the firmware, long (0.6 s): FOCUS on / off */
static int vf_hold = -1, vf_fired; static uint64_t vf_hold_t;
static void vf_hold_check(void)
{
  if (vf_hold >= 0 && !vf_fired && vibe_frames() - vf_hold_t > (uint64_t)(sdk_sr * 0.6f)) { vf_fired = 1; vf_toggle(); }
}
VIBE_EXPORT(vibe_wasm_event) void vibe_wasm_event(int type, int id, int down, float value, int midi) {
  vibe_event e = { (uint8_t)type, (uint8_t)id, (uint8_t)down, { (uint8_t)(midi & 255), (uint8_t)((midi >> 8) & 255), (uint8_t)((midi >> 16) & 255) }, value };
  vf_hold_check();
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
  mout_frames = frames > 0 ? frames : 1; clk_block(mout_frames);   /* (SDK 9: the clock's pulses of this block, then the firmware's MIDI) */
  if (vout.flags & VIBE_OUT_CUE) { for (int i = 0; i < mout_frames && i < VIBE_MAX_BLOCK; i++) vout.cue[0][i] = vout.cue[1][i] = 0.0f; vout.cue_written = 0; }
  mout_in_audio = 1;
  vibe_on_audio(inputs[0], io.l, io.r, frames);
  mout_in_audio = 0;
  inputs[0] = inputs[1] = 0;
  {                                             /* the timeline moves on */
    uint32_t l = frames_lo + (uint32_t)frames;
    frames_seq = frames_seq + 1u; VIBE_RELEASE();
    if (l < frames_lo) frames_hi = frames_hi + 1u;
    frames_lo = l; VIBE_RELEASE();
    frames_seq = frames_seq + 1u;
  }
}
VIBE_EXPORT(vibe_wasm_rear) void *vibe_wasm_rear(void) { return &rear; }
VIBE_EXPORT(vibe_wasm_draw) uint16_t *vibe_wasm_draw(void) { vf_hold_check(); vg_begin(framebuffer); vibe_on_draw(framebuffer); return framebuffer; }
VIBE_EXPORT(vibe_wasm_leds) void *vibe_wasm_leds(void) { return &leds; }
VIBE_EXPORT(vibe_wasm_labels) void *vibe_wasm_labels(void) { return labels; }   /* VB_COUNT x 8 chars */
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

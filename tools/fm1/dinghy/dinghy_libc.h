/* SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE. */
/* tools/fm1/dinghy/dinghy_libc.h – the few C library calls a Dinghy firmware's host parts make (dinghy_app.c: HERA's sprintf,
 * KAWA's strlen …), as dl_* – dinghy_app.c names them so for itself only. sprintf / snprintf: %d %i %u %x %X %c %s %%, with
 * a width, 0 and - flags and l / h (no floating point). */
#pragma once
#include <stdarg.h>
#include <stddef.h>
static size_t dl_strlen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static int dl_strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (unsigned char)*a - (unsigned char)*b; }
static int dl_strncmp(const char *a, const char *b, size_t n) { for (; n && *a && *a == *b; n--) a++, b++; return n ? (unsigned char)*a - (unsigned char)*b : 0; }
static char *dl_strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
static char *dl_strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }
static int dl_vsnprintf(char *o, size_t cap, const char *f, va_list ap)
{
  size_t n = 0;
#define DL_PUT(ch) do { if (n + 1 < cap) o[n] = (ch); n++; } while (0)
  for (; *f; f++) {
    if (*f != '%') { DL_PUT(*f); continue; }
    f++;
    int left = 0, zero = 0, w = 0;
    for (;; f++) { if (*f == '-') left = 1; else if (*f == '0') zero = 1; else break; }
    while (*f >= '0' && *f <= '9') w = w * 10 + (*f++ - '0');
    while (*f == 'l' || *f == 'h') f++;
    char buf[24]; const char *s = buf; int len = 0, neg = 0;
    switch (*f) {
    case 'd': case 'i': { long v = va_arg(ap, int); unsigned long u = v < 0 ? (neg = 1, (unsigned long)-v) : (unsigned long)v;
      do buf[len++] = (char)('0' + u % 10); while (u /= 10); break; }
    case 'u': case 'x': case 'X': { unsigned long u = va_arg(ap, unsigned), b = *f == 'u' ? 10 : 16;
      do { int d = (int)(u % b); buf[len++] = (char)(d < 10 ? '0' + d : (*f == 'x' ? 'a' : 'A') + d - 10); } while (u /= b); break; }
    case 'c': buf[0] = (char)va_arg(ap, int); len = 1; break;
    case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; len = (int)dl_strlen(s); break;
    case '%': buf[0] = '%'; len = 1; break;
    default: DL_PUT('%'); DL_PUT(*f); if (!*f) f--; continue;
    }
    int pad = w - len - neg;
    if (!left && !zero) while (pad-- > 0) DL_PUT(' ');
    if (neg) DL_PUT('-');
    if (!left && zero) while (pad-- > 0) DL_PUT('0');
    if (s == buf && (*f == 'd' || *f == 'i' || *f == 'u' || *f == 'x' || *f == 'X')) while (len) DL_PUT(buf[--len]);
    else for (int i = 0; i < len; i++) DL_PUT(s[i]);
    if (left) while (pad-- > 0) DL_PUT(' ');
  }
  if (cap) o[n < cap ? n : cap - 1] = 0;
#undef DL_PUT
  return (int)n;
}
static int dl_snprintf(char *o, size_t cap, const char *f, ...) { va_list ap; va_start(ap, f); int r = dl_vsnprintf(o, cap, f, ap); va_end(ap); return r; }
static int dl_sprintf(char *o, const char *f, ...) { va_list ap; va_start(ap, f); int r = dl_vsnprintf(o, (size_t)-1 >> 1, f, ap); va_end(ap); return r; }

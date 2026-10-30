#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE.
# tools/fm1/dinghy_app.py <firmware's tests/host.c> <out dinghy_app.c> [its platform.c]
# A Dinghy firmware's tests/host.c is its fake FM-1: the part every Dinghy firmware has (keys, screen, MIDI, settings) is
# tools/fm1/dinghy/host.c; what this firmware's platform has beyond it (FM-106: its preset bank, SysEx out; SHINE ON: its
# pad LEDs …) is taken from here – every top-level definition that is neither in dinghy/host.c nor host tooling (the
# script engine, PPM / WAV files, main). The result is a starting point: firmware/<id>/dinghy_app.c may be edited by hand.
import os, re, sys

GENERIC = set("""fm1_ms fm1_ticks fm1_irq_off fm1_irq_on FM1_KEYMAP fm1_led_bg fm1_led_bg_ns uac audio_lates fm1_in host_edges
host_released host_note_edges host_enc host_led host_glow host_leds fm1_input_edges fm1_input_note_edges fm1_enc_take fm1_led_key
fm1_led_key_glow fb lcd_sync lcd_fill lcd_blit host_store settings_load settings_save mq mq_w mq_r midi_get midi_send app_audio
app_main platform_poll fm1_adc_read str_cpy platform_battery_bars host_psect psect_load psect_save host_psect_dirty host_blob blob_load blob_save host_blob_dirty NPSECT NBLOB SETTINGS_MAX""".split())
TOOLING = re.compile(r'^(main|read_script|write_\w+|script\w*|SCRIPT\w*|SHOT|host_out|wav\w*|end_ms|frame_fps|host_ev_t|perf_\w+|host_dump|host_fails)$')
STDIO = re.compile(r'\b(FILE|fopen|fclose|fwrite|fread|fgets|fputc|perror|exit|sscanf|realloc|ioctl|getenv|atoi|open|close)\s*[(*]')

src = open(sys.argv[1], encoding='utf-8', errors='replace').read()
src = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), src, flags=re.S)
chunks, cur, depth, i = [], '', 0, 0
for line in src.split('\n'):
    s = line.strip()
    if depth == 0 and not cur.strip() and (s.startswith('#') or not s):
        if s.startswith('#'): chunks.append(line)
        continue
    cur += line + '\n'
    depth += line.count('{') - line.count('}')
    if depth == 0 and (s.endswith(';') or s.endswith('}')):
        chunks.append(cur); cur = ''
LIBC = set("memset memcpy memmove memcmp strlen strcmp strncmp strcpy strncpy strcat strchr sprintf snprintf vsnprintf printf".split())
out, kept = [], []
for c in chunks:
    t = c.strip()
    if t.startswith('#'):
        m = re.match(r'#\s*define\s+(\w+)', t)
        # (a host's own libc names – KETCH: #define memset libc_memset, against the host's C library – are not the VIBE's)
        if m and m.group(1) not in ('APP_NAME', 'APP_VER', 'FS', 'HALF_FRAMES', 'CV_MAX', 'FM1_NCOL') and m.group(1) not in LIBC:
            out.append(t); kept.append(m.group(1))
        # the firmware's own headers it takes in (DINGHY: "store.h"; GRIMM: "../firmware/hal/fm1_matrix.h"), from the
        # build's include paths (firmware/src, firmware) – not the host's C library, not what dinghy/host.c has already
        m = re.match(r'#\s*include\s+"([^"]+)"', t)
        if m and os.path.basename(m.group(1)) not in ('gfx.c', 'libc.c', 'host.h'):
            out.append('#include "%s"' % re.sub(r'^(\.\./)+firmware/', '', m.group(1))); kept.append('')
        continue
    head = t.split('{', 1)[0].split('=', 1)[0]
    m = re.search(r'(\w+)\s*(\(|\[|;|$)', head) if '(' not in head else re.search(r'(\w+)\s*\(', head)
    name = m.group(1) if m else ''
    if re.match(r'^(static\s+|typedef\s+)*(volatile\s+)?(struct|union|enum)\b[^;{]*\{', t):
        m = re.search(r'\}\s*(\w+)\s*(\[[^\]]*\])?\s*(=|;)', t)   # a struct variable / typedef: the name after its body
        name = m.group(1) if m else ''
    # (a test switch from the environment – OberVave's blob_load: OBV_V15 – is never set on the VIBE: the function stays)
    c = re.sub(r'\bgetenv\s*\(\s*"[^"]*"\s*\)', '((char *)0)', c)
    # (a second definition of the same name is the other branch of an #if the host chose – GRIMM's batt_raw: the first;
    #  EMSCRIPTEN_KEEPALIVE: a web build's exports, not the VIBE's)
    if not name or name in GENERIC or name in kept or TOOLING.match(name) or STDIO.search(c) or 'EMSCRIPTEN' in c:
        continue
    out.append(c.rstrip()); kept.append(name)
# what leans on the host tooling left out (KAWA: its script's events in a host_ev_t array) goes too, until nothing does
dropped = set(re.findall(r'\b(host_ev_t|SCRIPT\w*|SHOT|script\w*|wav\w*|perf_\w+|read_script|write_\w+)\b', src))
while True:
    bad = [i for i, c in enumerate(out) if not c.lstrip().startswith('#') and any(re.search(r'\b%s\b' % re.escape(d), c) for d in dropped)]
    if not bad: break
    for i in reversed(bad):
        dropped.add(kept[i] if i < len(kept) else '')
        del out[i]; del kept[i]
    dropped.discard('')
# the platform's own one-line clock getters (FM-106: fm106_now_ms) – on the hardware only, but the app calls them
if len(sys.argv) > 3:
    for m in re.finditer(r'^(?:static\s+)?(?:inline\s+)?uint32_t\s+(\w+)\s*\(void\)\s*\{\s*return\s+fm1_ms\s*;\s*\}', open(sys.argv[3]).read(), re.M):
        if m.group(1) not in GENERIC and m.group(1) not in kept: out.append(m.group(0)); kept.append(m.group(1))
with open(sys.argv[2], 'w') as f:
    f.write('/* firmware/<id>/dinghy_app.c – made by tools/fm1/dinghy_app.py from the firmware\'s tests/host.c: what its platform\n'
            ' * has beyond tools/fm1/dinghy/host.c. Edit by hand if needed (e.g. keep something on the VIBE with vibe_store). */\n\n')
    # the C library calls the kept parts make (HERA: sprintf, KAWA: strlen) – declared here, tools/fm1/dinghy/dinghy_libc.h has them
    used = sorted(f for f in ('sprintf', 'snprintf', 'strlen', 'strcmp', 'strncmp', 'strcpy', 'strchr') if re.search(r'\b%s\s*\(' % f, '\n'.join(out)))
    if used:                                        # (named dl_* inside this file only: the firmware's own libc stays as it is)
        f.write('#include "dinghy_libc.h"\n' + ''.join('#define %s dl_%s\n' % (u, u) for u in used))
    f.write('\n'.join(out) + '\n')
    f.write(''.join('#undef %s\n' % u for u in used))
print('dinghy_app.c: ' + (', '.join(kept) if kept else '(nothing beyond the common fake FM-1)'))

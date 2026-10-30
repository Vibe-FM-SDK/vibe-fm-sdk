#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE.
# Builds a converted FM-1 firmware (firmware/<id>/, made by tools/fm1-convert.sh) into build/<id>.wasm:
#   sh tools/fm1/build-fm1.sh <id>          (tools/build.sh <id> calls it through firmware/<id>/build.sh, and then makes
#                                            the .vibe.aot as for every firmware)
# The firmware's src/ is copied next to the fake FM-1 (tools/fm1/hal/) – its sources include "../hal/…" – and compiled
# with the adapter (tools/fm1/fm1_vibe.c) and the VIBE SDK, as Protokoll 6 asks (shared memory), then Asyncify makes its
# never-returning main loop a coroutine. Settings: firmware/<id>/fm1.conf (FAMILY felucca | dinghy, MAIN, NAME, VERSION,
# MIDI_Q). Dinghy: tools/fm1/fm1_dinghy.c with the fake FM-1 tools/fm1/dinghy/host.c (+ firmware/<id>/dinghy_app.c).
set -e
cd "$(dirname "$0")/../.."
id="$1"; F=firmware/$id; [ -f $F/fm1.conf ] || { echo "$F/fm1.conf missing (made by tools/fm1-convert.sh)"; exit 1; }
. ./$F/fm1.conf
if [ -z "$ZIG" ] || [ ! -x "$ZIG" ]; then
  if command -v zig >/dev/null 2>&1; then ZIG=zig
  elif [ -x "$HOME/.local/lib/python3.10/site-packages/ziglang/zig" ]; then ZIG="$HOME/.local/lib/python3.10/site-packages/ziglang/zig"
  else ZIG="$HOME/.local/opt/zig-linux-x86_64-0.13.0/zig"
  fi
fi
if [ -z "$WASM_OPT" ] || [ ! -x "$WASM_OPT" ]; then
  WASM_OPT=$(ls -d "$HOME"/.local/share/*-wasm/binaryen-*/bin/wasm-opt 2>/dev/null | sort | tail -n 1)
  if [ -z "$WASM_OPT" ]; then WASM_OPT=$(command -v wasm-opt 2>/dev/null || true); fi
fi
[ -x "$WASM_OPT" ] || { echo "wasm-opt (Binaryen) not found: set WASM_OPT (docs/entwickeln.md)"; exit 1; }
B=build/$id-src; rm -rf "${B:?}"; mkdir -p $B/obj
CFLAGS="-target wasm32-freestanding -O2 -fno-builtin -ffreestanding -w -matomics -mbulk-memory -Isdk/include -Isdk/src"
set -- -DFM1_MAIN="\"$MAIN\"" -DFM1_NAME="\"$NAME\"" -DFM1_VERSION="\"$VERSION\"" -DAPP_VER="\"$VERSION\""   # (APP_VER: what the firmware shows as its version)
[ "$MIDI_Q" = 1 ] && set -- "$@" -DFM1_MIDI_Q
# (__builtin_return_address: fm1-base records where a fault came from – nothing to record in WebAssembly)
set -- "$@" "-D__builtin_return_address(n)=((void *)0)"
# one core on the VIBE: the firmwares that can run their second-core work on the first are built that way (Melodee, Tessera)
set -- "$@" -DMELODEE_DUAL_CORE=0
# app.ld's marks (_rt_start/_rt_end/_rt_load …: JIANT and Melodee copy their RAM code and clear their pools between them):
# the ones the firmware declares extern, each one mark – nothing between, nothing to copy (build/<id>-src/marks.c defines them)
marks=$(grep -rhoE 'extern[^;]*\b_[a-z0-9]+_(start|end|load)\[\]' $F/upstream --include='*.c' --include='*.h' 2>/dev/null |
  grep -oE '\b_[a-z0-9]+_(start|end|load)\[' | sed -E 's/^_([a-z0-9]+)_(start|end|load)\[$/\1/' | sort -u)
: > $B/marks.c
for m in $marks; do set -- "$@" -D_${m}_end=_${m}_start -D_${m}_load=_${m}_start; echo "__attribute__((weak)) unsigned int _${m}_start[1];" >> $B/marks.c; done
# a firmware with its own fm1_ticks in its sources (OPN1, SID-1; CHIPBOX on its host): ours stays out (its hal/, its tests/ and fm1-base's host files not looked at)
grep -rqsE '^\s*(static\s+)?(inline\s+)?uint32_t\s+fm1_ticks\s*\(\s*void\s*\)\s*\{' --include='*.c' --include='*.h' $F/upstream/firmware/src && set -- "$@" -DFM1_OWN_TICKS
[ -n "${DEFINE:-}" ] && set -- "$@" "-D$DEFINE"   # (fm1.conf DEFINE=NAME="value": one define the firmware wants, e.g. its version)
[ -f $F/fm1map.h ] && set -- "$@" -DFM1_MAP="\"$PWD/$F/fm1map.h\""
[ -f $F/fm1_extra.c ] && set -- "$@" -DFM1_EXTRA="\"$PWD/$F/fm1_extra.c\""
if [ "$FAMILY" = dinghy ]; then
  # Dinghy: platform.c includes "../../tests/host.c" – the fake FM-1 goes there, next to a copy of src/ (and gen/)
  mkdir -p $B/firmware $B/tests; cp -r $F/upstream/firmware/src $B/firmware/src; cp tools/fm1/dinghy/host.c $B/tests/host.c
  for d in $F/upstream/firmware/*/; do n=$(basename "$d"); [ -e "$B/firmware/$n" ] || cp -r "$d" "$B/firmware/$n"; done
  [ -f $F/dinghy_app.c ] && set -- "$@" -DDINGHY_APP="\"$PWD/$F/dinghy_app.c\""
  grep -qs 'fm1_matrix\.h' $F/dinghy_app.c && set -- "$@" -DFM1_MATRIX_OWN
  # (dinghy_app.c: the firmware's own headers by firmware/src or firmware/, our dl_* C library calls by tools/fm1/dinghy)
  "$ZIG" cc $CFLAGS -D${HOSTDEF:-DINGHY_HOST} -I$B/firmware/src -I$B/firmware/gen -I$B/firmware -Itools/fm1/dinghy "$@" $(for d in $B/firmware/*/; do echo "-idirafter $d"; done) -c -o $B/obj/fm1_vibe.o tools/fm1/fm1_dinghy.c
  k=0; for u in ${UNITS:-}; do k=$((k + 1)); [ -f $B/firmware/src/$u ] || continue   # (its own units: OPN1's vgm.c, psg.c …;
    grep -rqsE "#include *\"$u\"" $B/firmware/src && continue                         #  not one its main unit takes in: FM-106)
    if "$ZIG" cc $CFLAGS -D${HOSTDEF:-DINGHY_HOST} -I$B/firmware/src -I$B/firmware/gen -I$B/firmware "$@" -c -o $B/obj/unit_$k.o $B/firmware/src/$u 2>$B/unit_$k.err
    then echo "unit of its own: $u"; else echo "left out (does not compile on its own): $u – $(grep -m1 'error:' $B/unit_$k.err)"; rm -f $B/obj/unit_$k.o; fi; done
elif [ "$FAMILY" = fm1base ]; then
  # fm1-base: base/device/unity.c (the platform and the app in one unit), our fake FM-1 first on the include path – in
  # front of felucca-bsp's chip and board headers, whose real register code must never come in
  cp -r tools/fm1/hal $B/hal
  # a copy of app/ and base/ to build from: felucca-bsp's own sources include its headers by their neighbours' path
  # ("fm1_usb.h" next to chip/usb.c), which no include path overrides – in the copy each header the fake FM-1 has is ours
  cp -r $F/upstream $B/up
  # fm1-base's key sensing simulator (lib/touch.c's host part) built in for the VIBE: the keys' velocity (fm1_vibe.c)
  [ -f $B/up/base/lib/touch.c ] && sed -i 's/^#ifdef PLAT_HOST$/#if defined(PLAT_HOST) || defined(VIBE_TOUCH_HOST)/' $B/up/base/lib/touch.c && set -- "$@" -DVIBE_TOUCH_HOST
  for h in $B/hal/*.h; do n=$(basename "$h")
    find $B/up/base -name "$n" -path '*bsp*' | while read -r o; do printf '#include "%s"\n' "$PWD/$h" > "$o"; done
  done
  for d in $FBINT; do set -- "$@" "-D$d"; done
  for d in $FBSTR; do set -- "$@" "-D${d%%=*}=\"${d#*=}\""; done
  incs=""; for d in $INCS; do incs="$incs -I$B/up/$d"; done
  "$ZIG" cc $CFLAGS -I$B/hal $incs "$@" -c -o $B/obj/fm1_vibe.o tools/fm1/fm1_vibe.c
  k=0; for u in $O2; do k=$((k + 1)); "$ZIG" cc $CFLAGS -I$B/hal $incs -I$B/up/$(dirname "$u") "$@" -c -o $B/obj/o2_$k.o $B/up/$u && echo "unit: $u"; done
else
  cp -r $F/upstream/firmware/src $B/src; cp -r tools/fm1/hal $B/hal
  for d in $F/upstream/firmware/*/; do n=$(basename "$d"); case "$n" in src|gen|hal|loader|stub) ;; *) cp -r "$d" "$B/$n" ;; esac; done   # (../engines/…)
  [ -d $F/upstream/build/gen ] && set -- "$@" -I$F/upstream/build/gen
  [ -d $F/upstream/firmware/gen ] && set -- "$@" -I$F/upstream/firmware/gen
  grep -rqE 'fm1_led_dim\[[^]]*\]\[' $B/src && set -- "$@" -DFM1_LED_PLANES=4   # (dim planes: Melodee, Tessera)
  for d in $B/*/; do case "$(basename "$d")" in src|hal|obj) ;; *) set -- "$@" -idirafter "$d" ;; esac; done   # (its siblings' headers: KETCH's sound/core.h)
  "$ZIG" cc $CFLAGS -I$B/hal -I$B/src "$@" -c -o $B/obj/fm1_vibe.o tools/fm1/fm1_vibe.c
  # units of their own below src/ (FiMba's dsp/kalimba.c, built at -O2 apart): every .c in a subfolder no source includes;
  # one that does not compile on its own (a leftover, such as TRIBE's app/editor.c) is left out – the link says if it was needed
  k=0
  # (the firmware's build names them – fm1.conf UNITS, from its O2_UNITS: exactly those)
  for u in ${UNITS:-$(cd $B/src && find . -mindepth 2 -name '*.c' | sed 's|^\./||')}; do
    [ -f "$B/src/$u" ] || continue
    [ -n "${UNITS:-}" ] || ! grep -rqE "#include *\"($u|$(basename "$u"))\"" $B/src 2>/dev/null || continue
    k=$((k + 1))
    if "$ZIG" cc $CFLAGS -I$B/hal -I$B/src -I$B/src/$(dirname "$u") "$@" -c -o $B/obj/unit_$k.o $B/src/$u 2>$B/unit_$k.err; then
      echo "unit of its own: $u"
    else
      echo "left out (does not compile on its own): $u – $(grep -m1 'error:' $B/unit_$k.err)"; rm -f $B/obj/unit_$k.o
    fi
  done
fi
[ -s $B/marks.c ] && "$ZIG" cc $CFLAGS -c -o $B/obj/marks.o $B/marks.c
"$ZIG" cc $CFLAGS -c -o $B/obj/vibe_sdk.o sdk/src/vibe_sdk.c
"$ZIG" cc $CFLAGS -c -o $B/obj/vibe_font.o sdk/src/vibe_font.c
"$ZIG" wasm-ld --no-entry --export-dynamic --export=__heap_base --export=__data_end --shared-memory --Map=build/$id.map \
  --max-memory=67108864 -z stack-size=262144 -o build/$id.raw.wasm $B/obj/*.o
# Asyncify: only what can reach asyncify.start_unwind (sim_yield) gets instrumented – the rest stays as fast as it was
"$WASM_OPT" build/$id.raw.wasm --enable-threads --enable-bulk-memory --enable-mutable-globals --enable-sign-ext \
  --asyncify --pass-arg=asyncify-imports@asyncify.start_unwind -O2 --strip-debug --strip-producers -o build/$id.wasm

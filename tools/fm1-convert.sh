#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE.
# FM-1 firmware → VIBE firmware (and so: browser and VST, the same .wasm), without touching the firmware:
#   tools/fm1-convert.sh <folder | git URL> <id> [--name NAME] [--version V] [--author A]
#   e.g. tools/fm1-convert.sh https://github.com/isod89/sloop-fm1 sloop --name SLOOP
# Makes firmware/<id>/ (upstream/ = a copy of the firmware, fm1.conf, firmware.json, build.sh, README.md) and builds it:
# build/<id>.wasm + build/<id>.vibe.aot, checked with vibe-check. Then: tools/deploy-device.sh <id>.
# Both families: Felucca (fm1_cstart + the fm1_* HAL: SLOOP, zp12, GHOULBOX, JIANT …) and Dinghy (platform.c with
# app_audio / app_main: FM-106, FRODHES, SHINE ON, OberVave 2 …; what its platform has beyond the common fake FM-1 is taken
# from its tests/host.c into firmware/<id>/dinghy_app.c).
# Controls: tools/fm1/fm1_vibe.c / fm1_dinghy.c (defaults); a firmware may get firmware/<id>/fm1map.h and fm1_extra.c by hand,
# or with FM1_EXTRAS=<folder> at once (the converter repo's curated extras/<id>/).
set -e
cd "$(dirname "$0")/.."
src="$1"; id="$2"; shift 2 || { sed -n 3,10p "$0"; exit 2; }
name=""; version=""; author=""
while [ $# -gt 0 ]; do case "$1" in --name) name="$2"; name_given=1; shift ;; --version) version="$2"; shift ;; --author) author="$2"; shift ;; esac; shift; done
echo "$id" | grep -qE '^[a-z0-9_-]{1,31}$' || { echo "id: a-z 0-9 _ -, 1-31 characters"; exit 2; }
F=firmware/$id; [ -e $F ] && { echo "$F exists already – remove it first (or choose another id)"; exit 1; }
T=build/fm1-import/$id; rm -rf "${T:?}"; mkdir -p build/fm1-import
case "$src" in
  http*://*|git@*) git clone -q --depth 200 "$src" $T ;;
  *) [ -d "$src" ] || { echo "not found: $src"; exit 1; }; cp -r "$src" $T ;;
esac

# 1. which family? Felucca: a source that defines fm1_cstart; Dinghy: platform.c with app_audio
# (Dinghy first: its platform.c has an fm1_cstart of its own for the hardware)
# fm1-base (Charles Vestal's: an app/ on base/, felucca-bsp underneath; AMB, MONK, LOOM, POLLY, MODELAR …): first
if [ -f "$T/base/device/unity.c" ] && [ -d "$T/app" ]; then
family=fm1base; main="$T/base/device/unity.c"; srcdir="$T/app"; root="$T"
else
family=felucca
# (find -exec, not grep $(find): with no file grep would read stdin and wait forever)
plat=$(find $T -name platform.c -path '*firmware/src*' -exec grep -l "app_audio" {} + 2>/dev/null | head -1 || true)
main=""; [ -n "$plat" ] || main=$(find $T -name '*.c' -path '*src*' -exec grep -l "void fm1_cstart *(void)" {} + 2>/dev/null | head -1 || true)
if [ -z "$main" ]; then
  [ -n "$plat" ] || { echo "no FM-1 firmware found (neither fm1_cstart nor a Dinghy platform.c)"; exit 1; }
  family=dinghy; srcdir=$(dirname "$plat")
  # the app: the unit that includes platform.c (dinghy.c is Dinghy's own template – another one wins)
  main=$(grep -l '#include "platform.c"' "$srcdir"/*.c | grep -v '/dinghy\.c$' | head -1 || true)
  [ -n "$main" ] || main=$(grep -l '#include "platform.c"' "$srcdir"/*.c | head -1 || true)
  [ -n "$main" ] || { echo "Dinghy: no source includes platform.c"; exit 1; }
  root=$(cd "$srcdir/../.." && pwd)
  [ -f "$root/tests/host.c" ] || { echo "Dinghy: tests/host.c missing (its fake FM-1 names what the platform must have)"; exit 1; }
  # the switch that makes platform.c take tests/host.c (DINGHY_HOST; OberVave's line: OBV_HOST)
  hostdef=$(grep -B2 'tests/host.c' "$plat" | grep -oE '(ifdef|defined\(?) *\(?[A-Z0-9_]+_HOST' | grep -oE '[A-Z0-9_]+_HOST' | tail -1)
  [ -n "$hostdef" ] || hostdef=DINGHY_HOST
else
  srcdir=$(dirname "$main")
  # fm1_cstart deeper than src/ (FiMba, X0X, FoMni …: src/app/main_fm1.c): src/ is the sources' root, the unit includes "app/…"
  rel=$(basename "$main")
  while [ "$(basename "$srcdir")" != src ] && [ "$(basename "$(dirname "$srcdir")")" != src ] && [ "$srcdir" != "$T" ] && [ "$srcdir" != / ]; do
    case "$srcdir" in */firmware/src/*) rel="$(basename "$srcdir")/$rel"; srcdir=$(dirname "$srcdir") ;; *) break ;; esac
  done
  if [ "$(basename "$(dirname "$srcdir")")" = src ]; then rel="$(basename "$srcdir")/$rel"; srcdir=$(dirname "$srcdir"); fi
  root=$(cd "$srcdir/../.." && pwd)
  # the file the build compiles is the one that #includes it (Felucca builds one unit: felucca.c includes main.c …)
  for _ in 1 2 3; do
    up=$(grep -l "#include \"$rel\"" "$srcdir"/*.c 2>/dev/null | head -1 || true)
    [ -n "$up" ] && [ "$up" != "$main" ] && { main=$up; rel=$(basename "$up"); } || break
  done
fi
[ -d "$root/firmware/src" ] || { echo "unexpected layout: $srcdir (expected <repo>/firmware/src)"; exit 1; }
fi
echo "$family family · the firmware's unit: $(basename "$main")"

# 2. generated headers (tables, samples, fonts): there already, or made by the firmware's own tools/build.py
if [ $family = felucca ] && [ ! -d "$root/build/gen" ] && [ -f "$root/tools/build.py" ]; then
  echo "making build/gen with the firmware's tools/build.py …"
  ( cd "$root" && python3 -c "import sys; sys.path.insert(0, 'tools'); import build; build.generate()" ) || echo "(build.py generate() failed – the build will say what is missing)"
fi
# (generate() stopping early – Melodee's makes its JieLi loader first: the table generators it names one by one,
#  tools/gen_*.py … GEN / "<header>", for each header not made yet)
if [ $family = felucca ] && [ -f "$root/tools/build.py" ]; then
  grep -oE 'tools/gen_[A-Za-z0-9_]+\.py"?, *GEN */ *"[A-Za-z0-9_.]+\.h"' "$root/tools/build.py" 2>/dev/null |
    sed -E 's|^(tools/gen_[A-Za-z0-9_]+\.py).*"([A-Za-z0-9_.]+\.h)"$|\1 \2|' | while read -r g h; do
      [ -f "$root/build/gen/$h" ] || [ ! -f "$root/$g" ] && continue
      mkdir -p "$root/build/gen"; ( cd "$root" && python3 "$g" "build/gen/$h" </dev/null ) && echo "made build/gen/$h ($g)"
    done
fi

# 3. facts for fm1.conf and firmware.json
midi=0; grep -rqs --include="*.c" "midi_in_q" "$srcdir" && midi=1   # (its USB MIDI in queue: in src/, or in fm1-base's base/ – felucca-bsp's usb.c)
[ $family = fm1base ] && grep -rqs --include="*.c" "midi_in_q" "$T/base" && midi=1
[ -n "$name" ] || name=$(echo "$id" | tr 'a-z' 'A-Z')
case "$src" in http*://*|git@*) ;; *) T_GIT=no ;; esac
[ -n "$version" ] || [ "${T_GIT:-}" = no ] || version=$(git -C $T describe --tags --abbrev=0 2>/dev/null || true)   # (a cloned repo: its release tag, e.g. Beta5)
[ -n "$version" ] || version=$( (grep -ohE 'v?[0-9]+\.[0-9]+(\.[0-9]+)?' "$root/VERSION" "$root/SLOOP.md" "$root/README.md" 2>/dev/null || true) | head -1 | sed 's/^v//')
[ -n "$version" ] || version="1.0"
lic=$(head -c 600 "$root/LICENSE" 2>/dev/null | grep -oE 'GNU GENERAL PUBLIC LICENSE|MIT License|Apache License|BSD' | head -1 || true)
case "$lic" in "GNU GENERAL PUBLIC LICENSE") grep -qs "Version 3" "$root/LICENSE" && lic=GPL-3.0 || lic=GPL-2.0 ;; "MIT License") lic=MIT ;; "Apache License") lic=Apache-2.0 ;; BSD) lic=BSD ;; *) lic=unknown ;; esac
[ -n "$author" ] || author=$( (cd "$root" && git log -1 --format=%an 2>/dev/null) || true); [ -n "$author" ] || author="unknown (FM-1)"

# 4. firmware/<id>/
if [ $family = fm1base ]; then
  # app/ and base/ as they are (base with felucca-bsp), its generated headers made, its build settings into fm1.conf
  mkdir -p $F/upstream; cp -r "$T/app" "$T/base" $F/upstream/; find $F/upstream -name .git -prune -exec rm -rf {} +
  python3 tools/fm1/fm1base.py $F/upstream/app $F/upstream/base $F/upstream/gen > $F/fm1base.conf || { echo "fm1-base: its generated headers failed"; exit 1; }
  [ -n "${name_given:-}" ] || { n=$(sed -n "s/^NAME='\(.*\)'$/\1/p" $F/fm1base.conf); [ -n "$n" ] && name=$n; }
else
mkdir -p $F/upstream/firmware
cp -r "$srcdir" $F/upstream/firmware/src
[ -d "$root/build/gen" ] && { mkdir -p $F/upstream/build; cp -r "$root/build/gen" $F/upstream/build/gen; }
[ -d "$root/firmware/gen" ] && cp -r "$root/firmware/gen" $F/upstream/firmware/gen
# what sits next to src/ (e.g. engines/, a submodule its sources include as "../engines/…")
for d in "$(dirname "$srcdir")"/*/; do n=$(basename "$d"); case "$n" in src|gen|loader|stub) ;; hal) [ $family = dinghy ] && cp -r "$d" "$F/upstream/firmware/$n" ;; *) cp -r "$d" "$F/upstream/firmware/$n" ;; esac; done   # (not the loader; the real hardware layer only for a Dinghy firmware, whose headers of constants its store / matrix code takes – last on its include path, the fake FM-1 stands in)
fi
if [ $family = dinghy ]; then
  mkdir -p $F/upstream/tests; cp "$root/tests/host.c" $F/upstream/tests/host.c
  python3 tools/fm1/dinghy_app.py $F/upstream/tests/host.c $F/dinghy_app.c $F/upstream/firmware/src/platform.c
fi
for f in LICENSE LICENSES README.md VERSION; do [ -e "$root/$f" ] && cp -r "$root/$f" $F/upstream/; done
( cd "$root" && git rev-parse HEAD 2>/dev/null > /dev/null && git rev-parse HEAD ) > $F/upstream/COMMIT 2>/dev/null || rm -f $F/upstream/COMMIT
cat > $F/fm1.conf <<EOF
# made by tools/fm1-convert.sh from $src
FAMILY=$family
HOSTDEF=${hostdef:-}
MAIN=$(basename "$main")
NAME='$name'
VERSION='$version'
MIDI_Q=$midi
EOF
[ -f $F/fm1base.conf ] && grep -E "^(INCS|O2|FBINT|FBSTR)=" $F/fm1base.conf >> $F/fm1.conf && rm -f $F/fm1base.conf
# a Dinghy firmware's units of their own: what its host build adds to the main unit (OPN1's tests/run_host.sh: EXTRA="… vgm.c")
if [ $family = dinghy ]; then
  units=$(cat "$root"/tests/*.sh "$root"/build.sh 2>/dev/null | grep -oE 'firmware/src/[A-Za-z0-9_]+\.c' | sed 's|^firmware/src/||' | sort -u |
    grep -vxF -e "$(basename "$main")" -e platform.c | tr '\n' ' ' | sed 's/ $//')
  [ -n "$units" ] && echo "UNITS='$units'" >> $F/fm1.conf
fi
# the units the firmware's own build compiles apart (tools/build.py's O2_UNITS, below src/: TRIBE's tribe/*.c, FiMba's dsp/*.c)
if [ $family = felucca ] && [ -f "$root/tools/build.py" ]; then
  units=$(python3 -c "import re,sys; m=re.search(r'O2_UNITS\s*=\s*\[(.*?)\]', open(sys.argv[1]).read(), re.S); print(' '.join(u for u in re.findall(r'\"([A-Za-z0-9_./-]+\.c)\"', m.group(1)) if '..' not in u) if m else '')" "$root/tools/build.py")
  [ -n "$units" ] && echo "UNITS='$units'" >> $F/fm1.conf
fi
cat > $F/firmware.json <<EOF
{ "name": "$name", "version": "$version", "author": "$author", "description": "FM-1 firmware, converted (tools/fm1)", "license": "$lic", "protocol": 7, "order": 50 }
EOF
cat > $F/build.sh <<'EOF'
#!/bin/sh
# an FM-1 firmware, converted by tools/fm1-convert.sh: built by tools/fm1/build-fm1.sh (called by tools/build.sh)
exec sh "$(dirname "$0")/../../tools/fm1/build-fm1.sh" "$(basename "$(cd "$(dirname "$0")" && pwd)")"
EOF
adapter=tools/fm1/fm1_vibe.c; more=""
[ $family = dinghy ] && { adapter=tools/fm1/fm1_dinghy.c; more=", the rest of its platform: \`dinghy_app.c\` (from its tests/host.c)"; }
cat > $F/README.md <<EOF
# $name on VIBE (converted FM-1 firmware)

Made by \`tools/fm1-convert.sh\` from \`$src\`. \`upstream/\` is the firmware **unchanged** (licence: $lic, see
\`upstream/LICENSE\`); it runs on a fake FM-1 (\`tools/fm1\`) through the adapter \`$adapter\`. Passing the converted
firmware on means passing on its source with it, as its licence asks.

Family: **$family** (the adapter's first lines list the controls). Other controls: \`fm1map.h\`,
own extras: \`fm1_extra.c\`$more.
EOF

# 4b. a curated firmware's own extras (the converter repo's extras/<id>/): fm1map.h, fm1_extra.c, dinghy_app.c and
#     firmware.json replace the defaults, the lines of "conf" are added to fm1.conf (e.g. DEFINE=…, VERSION=…)
if [ -n "${FM1_EXTRAS:-}" ] && [ -d "$FM1_EXTRAS" ]; then
  for f in fm1map.h fm1_extra.c dinghy_app.c cheatsheet.html; do [ -f "$FM1_EXTRAS/$f" ] && cp "$FM1_EXTRAS/$f" $F/; done
  # firmware.json: its fields over the made one – but the version stays the firmware's own (its tag)
  [ -f "$FM1_EXTRAS/firmware.json" ] && python3 -c "import json,sys; a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2])); b.pop('version',None); a.update(b); json.dump(a,open(sys.argv[1],'w'),ensure_ascii=False)" $F/firmware.json "$FM1_EXTRAS/firmware.json"
  if [ -f "$FM1_EXTRAS/conf" ]; then
    while IFS= read -r l; do k="${l%%=*}"; [ -n "$k" ] && grep -v "^$k=" $F/fm1.conf > $F/fm1.conf.n && mv $F/fm1.conf.n $F/fm1.conf; echo "$l" >> $F/fm1.conf; done < "$FM1_EXTRAS/conf"
  fi
  echo "extras from $FM1_EXTRAS"
fi

# 5. build (.wasm + .vibe.aot) and check
sh tools/build.sh $id
# the check decides: a firmware that fails it ends with exit 1 (the files stay in build/ to look at)
ok=0; node tools/vibe-check.mjs build/$id.wasm --seconds 20 $([ -f build/$id.map ] && echo --map build/$id.map) > build/$id.check.txt || ok=$?
tail -12 build/$id.check.txt
[ $ok = 0 ] || exit 1
# and played as a person would (MIDI, keys, pads, the DAW's play, the encoders): a firmware with no sound anywhere or no
# screen is not one to hand out – its recording and screens stay in build/$id.play/ to listen to and look at
ok=0; node tools/vibe-play.mjs build/$id.wasm --out build/$id.play > build/$id.play.txt || ok=$?
cat build/$id.play.txt
[ $ok = 0 ] || { echo "vibe-play: $id plays no sound or shows no screen"; exit 1; }
echo "→ on the VIBE: tools/deploy-device.sh $id"

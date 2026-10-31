#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE.
# Builds a VIBE FM firmware (a folder with .c files and a firmware.json) to WebAssembly and packs it for the plugin:
#
#   sh tools/build.sh examples/hello          ->  build/hello.wasm  +  build/hello.zip (hello.wasm + hello.json)
#   sh tools/build.sh my-synth                ->  build/my-synth.wasm + build/my-synth.zip
#
# Drop the .zip onto the VIBE FM VST window and it installs and plays. The folder's name is the firmware's id.
# Needs Zig 0.13.0 (its clang compiles C to WebAssembly): `zig` on your PATH, or ZIG=/path/to/zig.
set -e
cd "$(dirname "$0")/.."
DIR="${1:?usage: sh tools/build.sh <firmware folder>}"; DIR="${DIR%/}"
[ -f "$DIR/firmware.json" ] || { echo "$DIR/firmware.json missing – copy one from examples/hello"; exit 1; }
ID=$(basename "$DIR")
case "$ID" in *[!A-Za-z0-9._-]*) echo "the folder's name is the firmware's id: letters, digits, - _ . only"; exit 1 ;; esac
ZIG="${ZIG:-zig}"
command -v "$ZIG" >/dev/null 2>&1 || [ -x "$ZIG" ] || { echo "zig not found – install Zig 0.13.0 (README) or set ZIG=/path/to/zig"; exit 1; }

# one file for every VIBE FM host (the plugin, the browser, the VIBE): shared memory, built with atomics (VIBE FM Protokoll 6)
T="build/obj/$ID"; rm -rf "$T"; mkdir -p "$T"
X=""; [ -d "$DIR/freestanding" ] && X="-I$DIR/freestanding"    # (libc headers a firmware brings itself, if any)
for f in "$DIR"/*.c sdk/src/vibe_sdk.c sdk/src/vibe_font.c; do
  "$ZIG" cc -target wasm32-freestanding -O2 -ffast-math -fno-builtin-memset -matomics -mbulk-memory \
    -Isdk/include -Isdk/src $X -Wall -Wno-unused-function -c -o "$T/$(basename "$f" .c).o" "$f"
done
"$ZIG" wasm-ld --no-entry --export-dynamic --export=__heap_base --export=__data_end \
  --shared-memory --max-memory=67108864 -z stack-size=262144 -o "build/$ID.wasm" "$T"/*.o
echo "built build/$ID.wasm ($(( $(wc -c < "build/$ID.wasm") / 1024 )) KB)"

# the plugin's package: <id>.wasm + <id>.json in a zip (or drop the .wasm alone – it then shows its id as its name);
# SDK 16: an editor/ folder (your own Editor page, EDITOR-PAGE.md) goes in as <id>.editor/ – 2 MB at most
cp "$DIR/firmware.json" "build/$ID.json"
rm -f "build/$ID.zip"; rm -rf "build/$ID.editor"
if [ -d "$DIR/editor" ]; then
  [ -f "$DIR/editor/index.html" ] || { echo "$DIR/editor/ has no index.html"; exit 1; }
  KB=$(du -sk "$DIR/editor" | cut -f1); [ "$KB" -le 2048 ] || { echo "$DIR/editor/ is ${KB} KB – 2 MB (2048 KB) at most"; exit 1; }
  cp -r "$DIR/editor" "build/$ID.editor"
fi
rm -f "build/$ID.png"; [ -f "$DIR/icon.png" ] && cp "$DIR/icon.png" "build/$ID.png"   # (its face: the VIBE's menu tile, the hub's card)
if command -v zip >/dev/null 2>&1; then (cd build && zip -q -r "$ID.zip" "$ID.wasm" "$ID.json" $( [ -f "$ID.png" ] && echo "$ID.png" ) $( [ -d "$ID.editor" ] && echo "$ID.editor" ))
elif command -v python3 >/dev/null 2>&1; then python3 -c "
import zipfile, sys, os
z = zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED); b = os.path.dirname(sys.argv[1])
for f in sys.argv[2:]: z.write(f, f.split('/')[-1])
ed = sys.argv[1][:-4] + '.editor'
if os.path.isdir(ed):
  for root, _, files in os.walk(ed):
    for f in files: p = os.path.join(root, f); z.write(p, os.path.relpath(p, b))
" "build/$ID.zip" "build/$ID.wasm" "build/$ID.json" $( [ -f "build/$ID.png" ] && echo "build/$ID.png" )
fi
[ -d "build/$ID.editor" ] && echo "its Editor page: build/$ID.editor/ (in the zip)"
[ -f "build/$ID.png" ] || echo "(no icon.png – give your firmware a face: README, \"icon.png\")"
[ -f "build/$ID.zip" ] && echo "packed build/$ID.zip – drop it onto the VIBE FM VST window" || echo "(no zip/python3: drop build/$ID.wasm and build/$ID.json onto the window instead)"

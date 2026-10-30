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
ID=$(basename "$DIR")
if [ -f "$DIR/build.sh" ]; then
  exec sh "$DIR/build.sh"
elif [ -f "firmware/$ID/build.sh" ]; then
  exec sh "firmware/$ID/build.sh"
fi
[ -f "$DIR/firmware.json" ] || { echo "$DIR/firmware.json missing – copy one from examples/hello"; exit 1; }
case "$ID" in *[!A-Za-z0-9._-]*) echo "the folder's name is the firmware's id: letters, digits, - _ . only"; exit 1 ;; esac
if [ -z "$ZIG" ] || [ ! -x "$ZIG" ]; then
  if command -v zig >/dev/null 2>&1; then ZIG=zig
  elif [ -x "$HOME/.local/lib/python3.10/site-packages/ziglang/zig" ]; then ZIG="$HOME/.local/lib/python3.10/site-packages/ziglang/zig"
  else ZIG="$HOME/.local/opt/zig-linux-x86_64-0.13.0/zig"
  fi
fi
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

# the plugin's package: <id>.wasm + <id>.json in a zip (or drop the .wasm alone – it then shows its id as its name)
cp "$DIR/firmware.json" "build/$ID.json"
rm -f "build/$ID.zip"
if command -v zip >/dev/null 2>&1; then (cd build && zip -q "$ID.zip" "$ID.wasm" "$ID.json")
elif command -v python3 >/dev/null 2>&1; then python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'w',zipfile.ZIP_DEFLATED); [z.write(f, f.split('/')[-1]) for f in sys.argv[2:]]" "build/$ID.zip" "build/$ID.wasm" "build/$ID.json"
fi
[ -f "build/$ID.zip" ] && echo "packed build/$ID.zip – drop it onto the VIBE FM VST window" || echo "(no zip/python3: drop build/$ID.wasm and build/$ID.json onto the window instead)"

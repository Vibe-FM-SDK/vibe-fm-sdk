#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE.
# The VIBE FM emulator on your own computer – the device in the browser, playing what you built:
#
#   sh tools/build.sh examples/hello        (build something first – every build/<id>.wasm + <id>.json is listed)
#   python3 tools/emulator.py               -> http://127.0.0.1:8766 opens: choose your firmware in the top bar
#
# Also "Load your own .wasm …" in that list, or drop a .wasm onto the device. Your firmware's files (vibe_store_*) live in
# the browser. Nothing leaves your computer. Chrome or Edge: Web MIDI for your keyboard, the audio input for IN 1 / IN 2.
# (The page must be cross-origin isolated – the firmware's memory is shared – so it is served with COOP/COEP from here,
# not opened as a file.)
import http.server, json, os, shutil, sys, webbrowser
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
EMU, BUILD = os.path.join(ROOT, 'emulator'), os.path.join(ROOT, 'build')
fw = os.path.join(EMU, 'firmware'); os.makedirs(fw, exist_ok=True)
lib = []
# firmwares/: OberVave 3.1 and VIBE DJ (with its DECKS and demo tracks) – to try the device, and to see a full firmware
FWS = os.path.join(ROOT, 'firmwares')
for i in ('obervave', 'player'):
    if not os.path.exists(os.path.join(FWS, i + '.wasm')): continue
    shutil.copyfile(os.path.join(FWS, i + '.wasm'), os.path.join(fw, i + '.wasm'))
    meta = json.load(open(os.path.join(FWS, i + '.json'), encoding='utf-8'))
    entry = {'id': i, 'file': 'firmware/' + i + '.wasm', 'order': len(lib), **meta}
    if os.path.exists(os.path.join(FWS, i + '.cheat.html')):
        shutil.copyfile(os.path.join(FWS, i + '.cheat.html'), os.path.join(fw, i + '.cheat.html')); entry['cheat'] = 'firmware/' + i + '.cheat.html'
    if os.path.isdir(os.path.join(FWS, i + '.seed')):
        shutil.rmtree(os.path.join(fw, i + '.seed'), ignore_errors=True); shutil.copytree(os.path.join(FWS, i + '.seed'), os.path.join(fw, i + '.seed'))
    lib.append(entry)
for n in sorted(os.listdir(BUILD)) if os.path.isdir(BUILD) else []:
    if not n.endswith('.wasm'): continue
    i = n[:-5]; card = os.path.join(BUILD, i + '.json')
    if any(f['id'] == i for f in lib): continue   # (your own build of OberVave: build/obervave-src.wasm, listed below)
    meta = json.load(open(card, encoding='utf-8')) if os.path.exists(card) else {}
    shutil.copyfile(os.path.join(BUILD, n), os.path.join(fw, n))
    lib.append({'id': i, 'file': 'firmware/' + n, 'name': meta.get('name', i.upper()), 'version': meta.get('version', ''),
                'author': meta.get('author', ''), 'description': meta.get('description', ''), 'license': meta.get('license', ''),
                'order': len(lib)})
json.dump(lib, open(os.path.join(fw, 'index.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
# controllers/: the controller profiles (CONTROLLERS.md) – yours too: put a .json there, restart the emulator
CT = os.path.join(ROOT, 'controllers')
profiles = [json.load(open(os.path.join(CT, n), encoding='utf-8')) for n in sorted(os.listdir(CT)) if n.endswith('.json')] if os.path.isdir(CT) else []
json.dump(profiles, open(os.path.join(EMU, 'controllers.json'), 'w', encoding='utf-8'), ensure_ascii=False)
print('firmwares:', ', '.join(f['id'] for f in lib))
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin'); self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cache-Control', 'no-cache'); super().end_headers()
    def log_message(self, *a): pass
port = int(sys.argv[1]) if len(sys.argv) > 1 else 8766
os.chdir(EMU)
url = 'http://127.0.0.1:%d/' % port
print('the emulator:', url, '(Ctrl+C stops it)')
try: webbrowser.open(url)
except Exception: pass
http.server.ThreadingHTTPServer(('127.0.0.1', port), H).serve_forever()

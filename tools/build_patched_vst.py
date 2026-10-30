import re, os, shutil, subprocess, tempfile

root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
sdk_build = os.path.join(root_dir, 'build')
os.makedirs(sdk_build, exist_ok=True)

out_vst = os.path.join(sdk_build, 'VIBE FM VST.vst3')
out_clap = os.path.join(sdk_build, 'VIBE FM VST.clap')

vst_src = r'C:\Program Files\Common Files\VST3\VIBE FM VST.vst3.bak'
if not os.path.exists(vst_src):
    vst_src = r'C:\Program Files\Common Files\VST3\VIBE FM VST.vst3'

clap_src = r'C:\Program Files\Common Files\CLAP\VIBE FM VST.clap'
clap_bak = r'C:\Program Files\Common Files\CLAP\VIBE FM VST.clap.bak'

css_src = open(os.path.join(root_dir, 'emulator', 'css', 'vibe.css'), 'r', encoding='utf-8').read()
js_src = open(os.path.join(root_dir, 'emulator', 'js', 'editor.js'), 'r', encoding='utf-8').read()

# Minify CSS to fit in 28390 bytes
css_clean = re.sub(r'/\*.*?\*/', '', css_src, flags=re.DOTALL)
css_clean = re.sub(r'[ \t]+', ' ', css_clean)
css_clean = re.sub(r'\n\s*\n+', '\n', css_clean)
css_bytes = css_clean.replace('\r\n', '\n').encode('utf-8')
target_css_len = 28390
print(f"Minified CSS size: {len(css_bytes)} / {target_css_len} bytes ({target_css_len - len(css_bytes)} bytes spare)")
assert len(css_bytes) <= target_css_len, f"CSS too big: {len(css_bytes)} > {target_css_len}"
css_padded = css_bytes + b' ' * (target_css_len - len(css_bytes))

# Clean JS minifier (strips comments and extra whitespace only; keeps all original identifiers intact)
def minify_js(code):
    code = re.sub(r'/\*.*?\*/', '', code, flags=re.DOTALL)
    out = []
    for line in code.splitlines():
        line = re.sub(r'^\s*//.*$', '', line).strip()
        if not line:
            continue
        literals = []
        def save_lit(m):
            literals.append(m.group(0))
            return f"\x01{len(literals)-1}\x02"
        protected = re.sub(r"('(?:\\.|[^'\\])*'|\"(?:\\.|[^\"\\])*\"|`(?:\\.|[^`\\])*`)", save_lit, line)
        protected = re.sub(r'\s+', ' ', protected)
        protected = re.sub(r'\s*([{};,=\(\)\[\]\?\:\+\-\*\|&<>!])\s*', r'\1', protected)
        for i in range(len(literals) - 1, -1, -1):
            protected = protected.replace(f"\x01{i}\x02", literals[i])
        out.append(protected)
    res = []
    for i, l in enumerate(out):
        res.append(l)
        if i + 1 < len(out):
            nxt = out[i + 1]
            if nxt.startswith('.') or l.endswith(('return', 'else', 'const', 'let', 'var', 'async', 'export', 'class')):
                res.append(' ')
            elif not l.endswith((';', '{', '}', ',')):
                res.append(';')
    return ''.join(res)

js_clean = minify_js(js_src)

# Validate JS syntax using node --check
with tempfile.NamedTemporaryFile('w', suffix='.mjs', delete=False, encoding='utf-8') as tf:
    tf.write(js_clean)
    tf_path = tf.name
try:
    subprocess.run(['node', '--check', tf_path], check=True)
    print("JS syntax check PASSED!")
finally:
    os.remove(tf_path)

js_bytes = js_clean.encode('utf-8')
target_js_len = 18064
print(f"Minified JS size: {len(js_bytes)} / {target_js_len} bytes ({target_js_len - len(js_bytes)} bytes spare)")
assert len(js_bytes) <= target_js_len, f"JS too big: {len(js_bytes)} > {target_js_len}"
js_padded = js_bytes + b' ' * (target_js_len - len(js_bytes))

# Patch curve req_seq increment bug in C++ host (41 39 b7 40 2d 0e 00 75 36 -> 41 39 b7 40 2d 0e 00 eb 36)
curve_bug_pat = bytes.fromhex('4139b7402d0e007536')
curve_fix_pat = bytes.fromhex('4139b7402d0e00eb36')

# 1. Patch VST3
with open(vst_src, 'rb') as f:
    vst_data = bytearray(f.read())

idx = vst_data.find(curve_bug_pat)
if idx != -1:
    vst_data[idx : idx + len(curve_fix_pat)] = curve_fix_pat
    print(f"Patched C++ curve refresh bug in VST3 at {hex(idx)}")

vst_css_offset = 0x670020
vst_js_offset = 0x693230

vst_data[vst_css_offset : vst_css_offset + target_css_len] = css_padded
vst_data[vst_js_offset : vst_js_offset + target_js_len] = js_padded

with open(out_vst, 'wb') as f:
    f.write(vst_data)
print(f"Successfully built patched VST3 at: {out_vst} ({len(vst_data)} bytes)")

# 2. Patch CLAP
clap_in = clap_bak if os.path.exists(clap_bak) else clap_src
if os.path.exists(clap_in):
    with open(clap_in, 'rb') as f:
        clap_data = bytearray(f.read())

    idx = clap_data.find(curve_bug_pat)
    if idx != -1:
        clap_data[idx : idx + len(curve_fix_pat)] = curve_fix_pat
        print(f"Patched C++ curve refresh bug in CLAP at {hex(idx)}")

    clap_css_offset = 0x622ed0
    clap_js_offset = 0x6460e0

    clap_data[clap_css_offset : clap_css_offset + target_css_len] = css_padded
    clap_data[clap_js_offset : clap_js_offset + target_js_len] = js_padded

    with open(out_clap, 'wb') as f:
        f.write(clap_data)
    print(f"Successfully built patched CLAP at: {out_clap} ({len(clap_data)} bytes)")

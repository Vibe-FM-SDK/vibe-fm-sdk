#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# tools/fm1/fm1base.py <app dir> <base dir> <gen dir> – an fm1-base firmware (Charles Vestal's: an app/ on base/, which is
# felucca-bsp underneath) for tools/fm1-convert.sh: its fm1.toml read, its generated headers made as base's tools/build.py
# makes them (the font, the screen reader's words, the app's own generators), and its build settings printed as
# fm1.conf lines: INCS (include dirs, relative to the firmware's upstream/), O2 (units of their own) and DEFINE.
import re, subprocess, sys
from pathlib import Path

app, base, gen = (Path(a).resolve() for a in sys.argv[1:4])
up = app.parent                                    # (upstream/: app/ and base/ side by side)

def conf(path):                                    # fm1.toml, the plain part of TOML it uses (no tomllib on Python 3.10)
    out, lines, acc = {}, [], ""
    for line in path.read_text(encoding="utf-8").splitlines():       # (an array over several lines joined into one)
        line = re.sub(r"\s+#.*$", "", line).strip()
        if acc or re.match(r'^[A-Za-z0-9_]+\s*=\s*\[', line) and line.count("[") > line.count("]"):
            acc += " " + line
            if acc.count("[") > acc.count("]"):
                continue
            line, acc = acc.strip(), ""
        lines.append(line)
    for line in lines:
        m = re.match(r'^([A-Za-z0-9_]+)\s*=\s*(.+)$', line)
        if not m:
            continue
        k, v = m.group(1), m.group(2).strip()
        if v.startswith("["):
            out[k] = re.findall(r'"([^"]*)"', v)
        elif v.startswith('"'):
            out[k] = v.strip('"')
        else:
            out[k] = {"true": True, "false": False}.get(v, v)
    return out

c = conf(app / "fm1.toml") if (app / "fm1.toml").is_file() else {}
gen.mkdir(parents=True, exist_ok=True)
tools = base / "tools"
for cmd in ([tools / "gen_font.py", gen / "felucca_font.h"],
            [tools / "gen_words.py", app / c.get("words", "words.tsv"), app / "web" / "words", gen / "ann_words.h"]):
    if Path(cmd[0]).is_file():
        r = subprocess.run([sys.executable, *map(str, cmd)], capture_output=True, text=True,
                           env={"FM1_FONT_FULL": " ".join(c.get("font_full", [])), "PATH": "/usr/bin:/bin"})
        if r.returncode:
            sys.stderr.write(r.stderr[-2000:]); sys.exit(f"fm1base: {Path(cmd[0]).name} failed")
for line in c.get("gen", []):                      # the app's own generators
    cmd = [a.replace("{gen}", str(gen)) for a in line.split()]
    r = subprocess.run([sys.executable, str(app / cmd[0]), *cmd[1:]], cwd=app, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr[-2000:]); sys.exit(f"fm1base: {cmd[0]} failed")

rel = lambda p: str(Path(p).resolve().relative_to(up))
incs = [base / "device", base / "include", base / "lib", app, gen, *(app / d for d in c.get("include", []) + c.get("device_include", [])),
        base / "bsp" / "boards" / "fm1", base / "bsp" / "chip"]
print("INCS='" + " ".join(rel(d) for d in incs if d.is_dir()) + "'")
o2 = [p for f in c.get("o2", []) for p in (sorted(app.glob(f)) if any(ch in f for ch in "*?[") else [app / f])]   # (FRASIL: "dsp/*.c")
print("O2='" + " ".join(rel(p) for p in o2 if p.is_file()) + "'")
# defines: numbers (FBINT) and strings (FBSTR, no spaces: the build quotes them) – on the VIBE: no USB audio, radio,
# serial console, mirror, backup or second core
print("FBINT='FM1B_UAC=0 FM1B_UAC_OUT=0 FM1B_BLE=0 FM1B_WIFI=0 FM1B_CDC=0 FM1B_MIRROR=0 FM1B_BACKUP=0 FM1B_CPU2=0'")
strs = ["FELUCCA_ID=" + re.sub(r"[^A-Za-z0-9_-]", "", str(c.get("id_prefix", "FM-1_")) + "00")]
if c.get("name"):
    strs.append("APP_NAME=" + re.sub(r"[^A-Za-z0-9_.-]", "", str(c["name"])))
print("FBSTR='" + " ".join(strs) + "'")
print("NAME='" + re.sub(r"[^A-Za-z0-9 ._()+-]", "", str(c.get("name", app.parent.name))) + "'")

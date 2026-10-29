// SPDX-License-Identifier: Apache-2.0 – Copyright 2026 Sven Trogus (zvenson). VIBE FM SDK, see sdk/LICENSE and sdk/NOTICE.
// media-host.mjs – the media ops of SDK 13 (vibe_host_store op 6-12, docs/protokoll.md 13a) for the Node tools
// (wasmtest.mjs, vibe-check.mjs). Behaves like a real host that reads in its own thread: every new thing asked for is
// first answered -2 ("not yet", `pending` times – each followed by VIBE_EV_MEDIA READY before the next block), reads stop at 64 KB block edges (fewer bytes than asked – a firmware
// must read on), a source can vanish at any moment (remove) – then everything of it says -1.
// Sources: { key: { dir: "/path" } } (a folder, locked in: no "..", no links followed) or { key: { files: { "a/b.wav": Buffer } } }
// (in memory), each with optional label, write: true (writes go here, "VIBE FM/<id>/"), usb: true.
import fs from 'fs';
import path from 'path';

const BLK = 65536;
const keyOk = (k) => /^[A-Za-z0-9_-]{1,47}$/.test(k);
function partOk(b) {   // bytes of one part: 1-255, well-formed UTF-8, no control characters, no '\' or '/', not "." or ".."
  if (b.length < 1 || b.length > 255) return false;
  if (b[0] === 0x2e && (b.length === 1 || (b.length === 2 && b[1] === 0x2e))) return false;
  for (const c of b) if (c < 0x20 || c === 0x7f || c === 0x5c || c === 0x2f) return false;
  try { new TextDecoder('utf-8', { fatal: true }).decode(b); } catch (e) { return false; }
  return true;
}
// "key/a/b" -> { key, parts: ['a','b'] } or null
export function parsePath(bytes) {
  if (bytes.length < 1 || bytes.length > 1023) return null;
  let k = bytes.indexOf(0x2f); if (k < 0) k = bytes.length;
  const key = Buffer.from(bytes.subarray(0, k)).toString('latin1');
  if (!keyOk(key)) return null;
  const parts = [];
  let i = k + 1;
  while (i < bytes.length) {
    let j = bytes.indexOf(0x2f, i); if (j < 0) j = bytes.length;
    const p = bytes.subarray(i, j);
    if (!partOk(p)) return null;
    parts.push(Buffer.from(p).toString('utf8'));
    i = j + 1;
  }
  return { key, parts };
}
const ci = (a, b) => { const x = a.toLowerCase(), y = b.toLowerCase(); return x < y ? -1 : x > y ? 1 : a < b ? -1 : a > b ? 1 : 0; };

export class MediaHost {
  constructor({ fw = 'firmware', sources = {}, pending = 1 } = {}) {
    this.fw = fw; this.pending = pending; this.src = new Map(); this.asked = false; this.ev = []; this.seen = new Map();
    this.handles = new Map(); this.next = 1; this.recent = []; this.gen = 1;
    this.count = { sources: 0, list: 0, stat: 0, read: 0, create: 0, append: 0, close: 0, pending: 0, gone: 0, badPath: 0, bytesRead: 0, written: [] };
    for (const [k, s] of Object.entries(sources)) this.add(k, s);
  }
  add(key, s) {
    if (!keyOk(key)) throw new Error('bad source key ' + key);
    if (s.write) for (const o of this.src.values()) o.write = false;
    this.src.set(key, { ...s, gen: this.gen++, files: s.files ? new Map(Object.entries(s.files)) : null });
    if (this.asked) this.ev.push({ id: 0, down: 1, value: this.src.size });
  }
  remove(key) {
    if (!this.src.delete(key)) return false;
    for (const h of this.handles.values()) if (h.key === key) h.failed = true;
    if (this.asked) this.ev.push({ id: 0, down: 0, value: this.src.size });
    return true;
  }
  events() { if (this.ready && this.asked) { this.ready = false; this.ev.push({ id: 2, down: 1, value: 0 }); } const e = this.ev; this.ev = []; return e; }   // (id 2 READY: what was "not yet" is here now)
  // -2 the first `pending` times something new is asked for (a host's thread fetching it)
  wait(tag) { const n = this.seen.get(tag) || 0; if (n >= this.pending) return false; this.seen.set(tag, n + 1); this.count.pending++; this.ready = true; return true; }

  // ---- the backing: a folder (locked in) or a map
  node(s, parts) {   // { type: 'f'|'d', size, mtime, abs?, buf? } or null
    if (s.files) {
      const p = parts.join('/');
      if (!p) return { type: 'd', size: 0, mtime: 0 };
      if (s.files.has(p)) return { type: 'f', size: s.files.get(p).length, mtime: 0, buf: s.files.get(p) };
      for (const k of s.files.keys()) if (k.startsWith(p + '/')) return { type: 'd', size: 0, mtime: 0 };
      return null;
    }
    let abs = s.dir;
    try {
      for (let i = 0; i < parts.length; i++) {
        abs = path.join(abs, parts[i]);
        const st = fs.lstatSync(abs);
        if (st.isSymbolicLink()) return null;
        if (i < parts.length - 1 && !st.isDirectory()) return null;
        if (i === parts.length - 1) return st.isFile() ? { type: 'f', size: st.size, mtime: Math.floor(st.mtimeMs / 1000), abs } : st.isDirectory() ? { type: 'd', size: 0, mtime: 0, abs } : null;
      }
      return fs.statSync(abs).isDirectory() ? { type: 'd', size: 0, mtime: 0, abs } : null;
    } catch (e) { return null; }
  }
  entries(s, parts, n) {
    const out = [];
    if (s.files) {
      const pre = parts.length ? parts.join('/') + '/' : '', dirs = new Set();
      for (const [k, b] of s.files) { if (!k.startsWith(pre)) continue; const rest = k.slice(pre.length), i = rest.indexOf('/'); if (i >= 0) dirs.add(rest.slice(0, i)); else out.push({ name: rest, dir: false, size: b.length }); }
      for (const d of dirs) out.push({ name: d, dir: true, size: 0 });
    } else {
      for (const e of fs.readdirSync(n.abs, { withFileTypes: true })) {
        if (e.name.startsWith('.') || e.isSymbolicLink()) continue;
        if (e.isDirectory()) out.push({ name: e.name, dir: true, size: 0 });
        else if (e.isFile()) out.push({ name: e.name, dir: false, size: fs.statSync(path.join(n.abs, e.name)).size });
      }
    }
    return out.filter((e) => !e.name.startsWith('.') && partOk(Buffer.from(e.name))).sort((a, b) => (b.dir - a.dir) || ci(a.name, b.name));
  }
  // ---- the firmware's call; mem = the firmware's memory buffer (ArrayBuffer / SharedArrayBuffer)
  store(op, mem, np, nl, dp, len) {
    const u8 = new Uint8Array(mem), dv = new DataView(mem);
    if (op === 6) {
      this.count.sources++; this.asked = true;
      let w = 0;
      for (const [k, s] of this.src) {
        const b = Buffer.from(`${k}\t${s.write ? 'w' : ''}${s.usb ? 'u' : ''}\t${s.label || k}\n`);
        if (w + b.length > len) break; u8.set(b, dp + w); w += b.length;
      }
      return w;
    }
    if (op >= 7 && op <= 9) {
      let nb = u8.slice(np, np + nl); while (nb.length > 1 && nb[nb.length - 1] === 0x2f) nb = nb.subarray(0, nb.length - 1);
      const p = parsePath(nb); if (!p) { this.count.badPath++; return -1; }
      const s = this.src.get(p.key); if (!s) { this.count.gone++; return -1; }
      const tag = `${op}|${s.gen}|${Buffer.from(nb).toString('utf8')}`;
      if (op === 7) {
        this.count.list++;
        if (len < 4) return -1;
        if (this.wait(tag)) return -2;
        const n = this.node(s, p.parts); if (!n || n.type !== 'd') return -1;
        const skip = dv.getUint32(dp, true), list = this.entries(s, p.parts, n).slice(skip);
        let w = 0;
        for (const e of list) { const b = Buffer.from(e.dir ? `d\t\t${e.name}\n` : `f\t${e.size}\t${e.name}\n`); if (w + b.length > len) break; u8.set(b, dp + w); w += b.length; }
        return w;
      }
      if (op === 8) {
        this.count.stat++;
        if (len < 16) return -1;
        if (!p.parts.length) { u8.fill(0, dp, dp + 16); return 2; }
        if (this.wait(tag)) return -2;
        const n = this.node(s, p.parts); if (!n) return -1;
        dv.setBigUint64(dp, BigInt(n.size), true); dv.setBigInt64(dp + 8, BigInt(n.mtime), true);
        return n.type === 'f' ? 1 : 2;
      }
      this.count.read++;
      if (len < 8 || !p.parts.length) return -1;
      const off = Number(dv.getBigUint64(dp, true));
      if (this.wait(tag + '|' + Math.floor(off / BLK))) return -2;
      const n = this.node(s, p.parts); if (!n || n.type !== 'f') { this.count.gone++; return -1; }
      if (off >= n.size) return 0;
      const cnt = Math.min(len, BLK - (off % BLK), n.size - off);   // (up to the block's edge)
      if (n.buf) u8.set(n.buf.subarray(off, off + cnt), dp);
      else { const fd = fs.openSync(n.abs, 'r'); try { fs.readSync(fd, u8, dp, cnt, off); } finally { fs.closeSync(fd); } }
      this.count.bytesRead += cnt;
      return cnt;
    }
    if (op === 10) {
      this.count.create++;
      const nb = u8.slice(np, np + nl);
      if (!partOk(nb) || nb[0] === 0x2e || nl > 200 || len < 2) { this.count.badPath++; return -1; }
      const [key, s] = [...this.src].find(([, x]) => x.write) || [];
      if (!s || this.handles.size >= 8) return -1;
      const dir = ['VIBE FM', this.fw];
      if (this.wait(`10|${s.gen}|dir`)) return -2;
      const name = Buffer.from(nb).toString('utf8'), dot = name.lastIndexOf('.');
      const base = dot > 0 ? name.slice(0, dot) : name, ext = dot > 0 ? name.slice(dot) : '';
      const n = this.node(s, dir), have = new Set([...(n ? this.entries(s, dir, n).map((e) => e.name) : []), ...this.recent, ...[...this.handles.values()].map((h) => h.name)].map((x) => x.toLowerCase()));
      let cand = name; for (let k = 2; have.has(cand.toLowerCase()); k++) cand = `${base} (${k})${ext}`;
      const full = Buffer.from(`${key}/VIBE FM/${this.fw}/${cand}\0`);
      if (full.length > len) return -1;
      u8.set(full, dp);
      const id = this.next++;
      this.handles.set(id, { key, name: cand, gen: s.gen, chunks: [], failed: false });
      this.recent.push(cand);
      return id;
    }
    if (op === 11 || op === 12) {
      const h = this.handles.get(nl); if (!h) return -1;
      if (op === 11) {
        this.count.append++;
        if (h.failed || !this.src.has(h.key) || this.src.get(h.key).gen !== h.gen) { h.failed = true; return -1; }
        h.chunks.push(Buffer.from(u8.slice(dp, dp + len)));
        return 0;
      }
      this.count.close++;
      this.handles.delete(nl);
      const s = this.src.get(h.key), keep = len !== 0;
      if (!keep) return 0;
      if (h.failed || !s || s.gen !== h.gen) { this.ev.push({ id: 1, down: 0, value: nl }); return 0; }
      const data = Buffer.concat(h.chunks), rel = `VIBE FM/${this.fw}/${h.name}`;
      if (s.files) s.files.set(rel, data);
      else { const d = path.join(s.dir, 'VIBE FM', this.fw); fs.mkdirSync(d, { recursive: true }); fs.writeFileSync(path.join(d, '.' + h.name + '.part'), data); fs.renameSync(path.join(d, '.' + h.name + '.part'), path.join(d, h.name)); }
      this.count.written.push({ path: `${h.key}/${rel}`, bytes: data.length });
      this.ev.push({ id: 1, down: 1, value: nl });
      return 0;
    }
    return -1;
  }
}

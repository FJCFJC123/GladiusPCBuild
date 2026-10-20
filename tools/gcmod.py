"""Turn a GameCube Gladius mod's data folder into a GladiusPort loose data folder.

    python tools/gcmod.py <GC vanilla data> <GC mod data> <Xbox gladius.bec> <out dir> [--dry]

The mod is diffed against the vanilla GC data (paths case-insensitive, contents by md5); only files it changes or
adds are written, under <out dir>/data/, ready to copy into the game's data folder. Per file:
  - text / config / scripts (.tok .tint .enc .scb .idx .bin .brf .pak ...) and animations (.pan): the mod's file as is.
    The vanilla GC and Xbox copies of these match byte for byte (13045 of 13045 .pan files; text differs only in
    comments), so a GC mod file means the same on the Xbox engine.
  - models / textures (.pax .ptx .tga) the mod copied from another vanilla file (a byte copy, renamed): the Xbox
    copy of that source file, read from gladius.bec.
  - textures with new content: GC formats 0x2000 (CMPR) / 0x2200 (CMPR colour + CMPR alpha in green) are decoded and
    re-encoded as Xbox DXT1 / DXT5 in the same PTTP layout. Other new models are reported, not written.
  - a changed file that already equals the Xbox copy is skipped (the Xbox data has it).
  - script sources (.scp), backups, added copies ("x - copy.tok") and the GC tool's .txt dumps are skipped.
A report (gcmod_report.txt) lists every file and what was done with it.
"""
import collections
import hashlib
import os
import struct
import sys
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ptx  # noqa: E402  (DXT encoder)

COPY_FROM_XBOX = {'.pax', '.ptx', '.tga'}           # platform-specific: never take the GC bytes
SKIP_EXT = {'.scp', '.bak', '.bat'}


# ------------------------------------------------------------------------------------------------ Xbox gladius.bec
class Bec:
    """gladius.bec v3: ' ceb', u16 version, u16 alignment, u32 count, u32 ?, 16-byte entries sorted by hash
    (u32 hash, u32 offset, u32 stored size: 0 = raw, top bit = duplicate; u32 size). hash = CRC32 table, no xors."""
    T = []

    def __init__(self, path):
        if not Bec.T:
            for i in range(256):
                c = i
                for _ in range(8):
                    c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
                Bec.T.append(c)
        self.f = open(path, 'rb')
        hdr = self.f.read(16)
        if hdr[:4] != b' ceb':
            raise SystemExit(f'{path}: not a gladius.bec')
        n = struct.unpack_from('<I', hdr, 8)[0]
        raw = self.f.read(n * 16)
        self.ents = {}
        for i in range(n):
            h, off, st, sz = struct.unpack_from('<4I', raw, i * 16)
            self.ents[h] = (off, st & 0x7FFFFFFF, sz)

    def hash(self, key):
        v = 0
        for ch in key.encode():
            v = Bec.T[(v ^ ch) & 0xFF] ^ (v >> 8)
        return v

    def get(self, rel):
        e = self.ents.get(self.hash('data/' + rel))
        if not e:
            return None
        off, st, sz = e
        self.f.seek(off)
        d = self.f.read(st or sz)
        if st and d[:1] == b'\x78':
            d = zlib.decompress(d)
        return d[:sz]


# ------------------------------------------------------------------------------------------------ GC textures
def _c565(c):
    return np.array([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], np.int32)


def _cmpr_block(d, o):
    c0, c1 = struct.unpack_from('>HH', d, o)
    a, b = _c565(c0), _c565(c1)
    if c0 > c1:
        pal = [np.append(a, 255), np.append(b, 255), np.append((2 * a + b) // 3, 255), np.append((a + 2 * b) // 3, 255)]
    else:
        pal = [np.append(a, 255), np.append(b, 255), np.append((a + b) // 2, 255), np.zeros(4, np.int32)]
    out = np.zeros((4, 4, 4), np.int32)
    for y in range(4):
        row = d[o + 4 + y]
        for x in range(4):
            out[y, x] = pal[(row >> (6 - 2 * x)) & 3]
    return out


def _cmpr(d, o, w, h):
    """GC CMPR: 8x8 tiles (row-major) of four 4x4 DXT1 blocks, big-endian endpoints, indices MSB-first"""
    img = np.zeros((h, w, 4), np.int32)
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for sy, sx in ((0, 0), (0, 4), (4, 0), (4, 4)):
                img[ty + sy:ty + sy + 4, tx + sx:tx + sx + 4] = _cmpr_block(d, o)
                o += 8
    return img, o


def gc_texture_to_xbox(d):
    """GC PTTP texture file -> Xbox bytes (same chunks, entries re-encoded), or None for an unknown format."""
    d = bytearray(d)
    p = d.find(b'PFHD')
    if p < 0:
        return None
    n = struct.unpack_from('<I', d, p + 12)[0]                  # chunk: tag, size, version, count
    for i in range(n):
        e = p + 16 + 32 * i
        fmt, mips, _, w, h, size, rel = struct.unpack_from('<HHIHHII', d, e)
        if fmt not in (0x2000, 0x2200) or mips > 1:
            return None
        pw, ph = max(1 << (w - 1).bit_length(), 8), max(1 << (h - 1).bit_length(), 8)
        if pw * ph // (1 if fmt == 0x2200 else 2) != size:      # power-of-two storage, 4 bpp per CMPR plane
            return None
        img, o = _cmpr(d, e + rel, pw, ph)
        if fmt == 0x2200:
            alpha, o = _cmpr(d, o, pw, ph)
            img[..., 3] = alpha[..., 1]
        xfmt = ptx.FMT_DXT5 if fmt == 0x2200 else ptx.FMT_DXT1
        enc = ptx.encode(img.clip(0, 255).astype(np.uint8), xfmt)
        if len(enc) > size:
            return None
        d[e + rel:e + rel + len(enc)] = enc
        struct.pack_into('<H', d, e, xfmt)
    return bytes(d)


# ------------------------------------------------------------------------------------------------ diff + import
def index(root):
    out = {}
    for dp, _, fn in os.walk(root):
        for f in fn:
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, root).replace(os.sep, '/').lower()
            h = hashlib.md5()
            with open(p, 'rb') as fh:
                for b in iter(lambda: fh.read(1 << 20), b''):
                    h.update(b)
            out[rel] = (h.hexdigest(), p)
    return out


def skip_reason(rel, ext, bec, added):
    name = rel.rsplit('/', 1)[-1]
    if ext in SKIP_EXT or name.endswith('.bak'):
        return 'skipped (source / backup)'
    if added and ' ' in name:                                   # vanilla has spaced names ("xbox demo.enc")
        return 'skipped (working copy - name has a space)'
    if ext == '.txt' and bec.get(rel) is None:
        return 'skipped (GC tool dump, not game data)'
    return None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    dry = '--dry' in sys.argv
    if len(args) != 4:
        raise SystemExit(__doc__)
    van_root, mod_root, bec_path, out_root = args
    bec = Bec(bec_path)
    print('indexing vanilla GC data ...')
    van = index(van_root)
    by_hash = collections.defaultdict(list)
    for rel, (h, _) in van.items():
        by_hash[h].append(rel)
    print('indexing mod data ...')
    mod = index(mod_root)
    report, stats = [], collections.Counter()

    def write(rel, data):
        if dry:
            return
        p = os.path.join(out_root, 'data', *rel.split('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(data)

    for rel in sorted(mod):
        h, path = mod[rel]
        if rel in van and van[rel][0] == h:
            continue
        kind = 'changed' if rel in van else 'added'
        ext = os.path.splitext(rel)[1]
        why = skip_reason(rel, ext, bec, kind == 'added')
        if why:
            act = why
        else:
            data = open(path, 'rb').read()
            xbox_now = bec.get(rel)
            src = [s for s in by_hash.get(h, []) if bec.get(s) is not None]
            if xbox_now is not None and xbox_now == data:
                act = 'skipped (already the Xbox copy)'
            elif ext in COPY_FROM_XBOX and src:
                write(rel, bec.get(src[0]))
                act = f'Xbox copy of {src[0]}'
            elif ext in COPY_FROM_XBOX:
                conv = gc_texture_to_xbox(data) if ext in ('.tga', '.ptx') else None
                if conv is None:
                    act = 'NOT WRITTEN - new GC model / texture format, needs a converter'
                else:
                    write(rel, conv)
                    act = 'converted GC texture -> Xbox DXT'
            else:
                write(rel, data)
                act = 'mod file'
        stats[act.split(' of ')[0].split(' (')[0] + (f' ({act.split(" (")[1]}' if ' (' in act else '')] += 1
        report.append(f'{kind}\t{rel}\t{act}')
    removed = sorted(r for r in van if r not in mod)
    for rel in removed:
        report.append(f'missing\t{rel}\tleft as is (the mod has no copy)')
    if not dry:
        os.makedirs(out_root, exist_ok=True)
        with open(os.path.join(out_root, 'gcmod_report.txt'), 'w') as f:
            f.write('\n'.join(report) + '\n')
    for k, v in sorted(stats.items(), key=lambda kv: -kv[1]):
        print(f'{v:6}  {k}')
    if removed:
        print(f'{len(removed):6}  vanilla files the mod lacks (left as is)')
    bad = [r for r in report if 'NOT WRITTEN' in r]
    for r in bad:
        print('  ' + r)
    print(('dry run - nothing written' if dry else f'written to {out_root}'))


if __name__ == '__main__':
    main()

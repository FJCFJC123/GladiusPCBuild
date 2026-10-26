"""Change list of a GameCube Gladius mod, from its gladius.bec against the vanilla one.

    python tools/becdiff.py <vanilla gladius.bec> <mod gladius.bec> <out dir> [--xbox <xbox gladius.bec>] [--names <data dir> ...]

A bec stores no file names, only a hash of each path (CRC32 table, no xors, lower-case "data/..."), so an unpack
names only the files its name list knows and writes the rest as numbered .bin files - which a copied data folder then
lacks (Rags Mod's Druid battle animations, characters/orin/orin_*.pan). This works on the hashes: every entry the mod
adds or changes is listed, named where a candidate name hashes to it, and written under <out dir>/data/ - a complete
mod data folder for tools/gcmod.py.

Candidate names: every file under the --names data folders (vanilla and the mod's own dump), and, for every unit set
and mesh in the mod's config/classdefs.tok and every character folder, <set>/<set>_<anim>.pan for every animation
name vanilla uses and every SKILLANIM / anim name in the mod's skills.tok, plus the model, texture and variant names.
Entries still unnamed are written as data/_unnamed/<hash>.<kind> and listed (kind from the content).
Unnamed additions with the bytes of a vanilla entry are a repack tool's duplicates of vanilla files it could not name
(listed as duplicate, not written). With --xbox, files byte-identical to the Xbox file of the same name are skipped
entirely (not listed, not written): a mod's base can carry Xbox-only files (Leo's mod: every <set>_basepose.pan), which
the port already has.
Report: <out dir>/becdiff_report.txt (added / changed / unnamed / duplicate / removed, size, name).
"""
import collections
import hashlib
import os
import re
import struct
import sys
import zlib

T = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0xEDB88320 if _c & 1 else _c >> 1
    T.append(_c)


def path_hash(name):
    v = 0
    for ch in name.lower().replace('\\', '/').encode():
        v = T[(v ^ ch) & 0xFF] ^ (v >> 8)
    return v


class Bec:
    """gladius.bec: ' ceb', u16 version, u16 alignment, u32 count, u32 ?, then 16-byte entries
    (u32 path hash, u32 offset, u32 flags << 24 | stored size (0 = raw); u32 size). zlib data starts 0x78."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        hdr = self.f.read(16)
        if hdr[:4] != b' ceb':
            raise SystemExit(f'{path}: not a gladius.bec')
        n = struct.unpack_from('<I', hdr, 8)[0]
        raw = self.f.read(n * 16)
        self.ents = {}
        for i in range(n):
            h, off, st, sz = struct.unpack_from('<4I', raw, i * 16)
            self.ents[h] = (off, st & 0xFFFFFF, sz)        # top byte: flags (GC 0x02 on raw entries)

    def get(self, h):
        off, st, sz = self.ents[h]
        self.f.seek(off)
        d = self.f.read(st or sz)
        if st and d[:1] == b'\x78':
            d = zlib.decompress(d)
        return d[:sz]


def kind(d):
    """File kind from the content (for unnamed entries)."""
    if d[:4] == b'VERS':
        return 'pan'
    if d[:4] == b'PTTP':
        return 'ptx'
    if d[:4] == b'TINT':
        return 'tint'
    if d[:4] == b'PAK1':
        return 'pak'
    if d[:2] in (b'//', b'\r\n') or all(32 <= b < 127 or b in (9, 10, 13) for b in d[:256]):
        return 'txt'
    return 'bin'


def candidates(name_dirs):
    names = set()
    for root in name_dirs:
        for dp, _, fn in os.walk(root):
            for f in fn:
                names.add('data/' + os.path.relpath(os.path.join(dp, f), root).replace(os.sep, '/').lower())
    anims = set()                                            # "<set>_<anim>.pan" suffixes vanilla uses
    for n in names:
        m = re.match(r'data/characters/([^/]+)/\1_(.+)\.pan$', n)
        if m:
            anims.add(m.group(2))
    sets = set()
    for root in name_dirs:
        cd = os.path.join(root, 'config', 'classdefs.tok')
        if os.path.exists(cd):
            t = open(cd, encoding='latin-1').read()
            for key in ('UNITSETNAME', 'MESH'):
                sets |= {s.lower() for s in re.findall(key + r':\s*"([^"]+)"', t)}
        sk = os.path.join(root, 'config', 'skills.tok')
        if os.path.exists(sk):
            t = open(sk, encoding='latin-1').read()
            anims |= {s.lower() for s in re.findall(r'ANIM[A-Z]*:\s*"([^"]+)"', t)}
        ch = os.path.join(root, 'characters')
        if os.path.isdir(ch):
            sets |= {d.lower() for d in os.listdir(ch)}
    for s in sets:
        base = f'data/characters/{s}/{s}'
        names.update({f'{base}.pan', f'{base}.pax', f'{base}.ptx', f'{base}cinematic.pax'})
        names.update(f'{base}_{a}.pan' for a in anims)
        names.update(f'{base}_var{v:02d}.ptx' for v in range(1, 13))
        names.update(f'data/units/tintsets/{s}_var{v:02d}.tint' for v in range(1, 13))
        names.add(f'data/buildpaks/ca_{s}.pak')
        names.add(f'data/texture/classes/large/{s}.tga')
    return {path_hash(n): n for n in names}


KIND_EXTS = {'pan': ['pan'], 'ptx': ['ptx', 'tga'], 'tint': ['tint'], 'txt': ['enc', 'tok', 'cut', 'scn', 'txt'],
             'grd': ['grd'], 'idx': ['idx'], 'bin': ['bin', 'idx', 'grd', 'pak']}


def content_kind(d):
    k = kind(d)
    if k == 'bin' and d[:8] == bytes.fromhex('0200000008000000'):
        return 'grd'
    return k


def content_names(d):
    """Base names a file names itself by: an encounter's '// X.enc' line / ENCOUNTERNAME, a texture's NMPT name."""
    out = set()
    t = d[:600].decode('latin-1')
    out |= {m.lower() for m in re.findall(r'//\s*([\w ]+)\.enc', t)}
    out |= {m.lower() for m in re.findall(r'ENCOUNTERNAME:\s*"([^"]+)"', t)}
    m = re.search(rb'NMPT.{12}([^\0]+)', d[:200], re.S)
    if m:
        out.add(os.path.splitext(m.group(1).decode('latin-1'))[0].lower())
    return out


def crc_from(v, s):
    for ch in s.encode():
        v = T[(v ^ ch) & 0xFF] ^ (v >> 8)
    return v


def zeros(v, n):
    for _ in range(n):
        v = T[v & 0xFF] ^ (v >> 8)
    return v


def deep_names(unnamed, names, datas):
    """Second pass for entries no candidate matched: every folder holding that kind of file x every base name known
    (and the names the content gives itself). The hash has no xors, so it is linear: hash(dir + s) =
    zeros(hash(dir), len(s)) ^ hash_from_0(s) - each folder is hashed once per suffix length, then each
    "<base>.<ext>" is one table lookup."""
    dirs, bases = collections.defaultdict(set), set()
    for n in names.values():
        d, f = n.rsplit('/', 1)
        b, _, e = f.rpartition('.')
        dirs[e].add(d + '/')
        bases.add(b)
    found, tables = {}, {}

    def table(e, length):                                   # zeros(hash(dir), length) -> dir, per extension
        key = (e, length)
        if key not in tables:
            tables[key] = {zeros(crc_from(0, d), length): d for d in dirs.get(e, ())}
        return tables[key]

    for h in unnamed:
        k = content_kind(datas[h])
        for e in KIND_EXTS.get(k, []):
            for b in content_names(datas[h]) | bases:
                s = f'{b}.{e}'
                d = table(e, len(s)).get(h ^ crc_from(0, s))
                if d is not None and path_hash(d + s) == h:
                    found[h] = d + s
                    break
            if h in found:
                break
    return found


def main():
    argv = sys.argv[1:]
    name_dirs = []
    if '--names' in argv:
        i = argv.index('--names')
        name_dirs, argv = argv[i + 1:], argv[:i]
    xbox = None
    if '--xbox' in argv:
        i = argv.index('--xbox')
        xbox, argv = Bec(argv[i + 1]), argv[:i] + argv[i + 2:]
    if len(argv) != 3:
        raise SystemExit(__doc__)
    van, mod, out = Bec(argv[0]), Bec(argv[1]), argv[2]
    names = candidates(name_dirs)
    diff = {}
    for h in sorted(mod.ents):
        d = mod.get(h)
        if h in van.ents and van.get(h) == d:
            continue
        if xbox and h in xbox.ents and xbox.get(h) == d:   # an Xbox file the mod's base carried over: already ported
            continue
        diff[h] = d
    unnamed = [h for h in diff if h not in names]
    if unnamed:
        print(f'{len(unnamed)} unnamed after the candidate names - searching every folder ...')
        names.update(deep_names(unnamed, names, diff))
    # A repack tool that could not name some vanilla entries stores them again under a hash of its own made-up name
    # (Leo's mod: 232 of vanilla's 241 nameless entries): an unnamed addition with the bytes of a vanilla entry is
    # that duplicate, not new content - reported, not written.
    dup = {}
    left = [h for h in diff if h not in van.ents and h not in names]
    if left:
        sizes = {mod.ents[h][2] for h in left}
        by_md5 = {}
        for vh, (_, _, sz) in van.ents.items():
            if sz in sizes:
                by_md5.setdefault(hashlib.md5(van.get(vh)).digest(), vh)
        for h in left:
            vh = by_md5.get(hashlib.md5(diff[h]).digest())
            if vh is not None:
                dup[h] = vh
    rows, stats = [], collections.Counter()
    for h, d in diff.items():
        new = h not in van.ents
        name = names.get(h)
        status = 'added' if new else 'changed'
        if h in dup:
            stats['duplicate of vanilla'] += 1
            rows.append(f'duplicate\t{h:08X}\t{len(d)}\tcopy of vanilla {dup[h]:08X} {names.get(dup[h], "(unnamed)")}')
            continue
        if name:
            rel = name[len('data/'):]
        else:
            rel = f'_unnamed/{h:08X}.{kind(d)}'
            status += ' UNNAMED'
        p = os.path.join(out, 'data', *rel.split('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(d)
        stats[status] += 1
        rows.append(f'{status}\t{h:08X}\t{len(d)}\tdata/{rel}')
    removed = sorted(set(van.ents) - set(mod.ents))
    for h in removed:
        rows.append(f'removed\t{h:08X}\t\t{names.get(h, "?")}')
    rows.sort(key=lambda r: (r.split('\t')[0], r.split('\t')[3]))
    with open(os.path.join(out, 'becdiff_report.txt'), 'w') as f:
        f.write('\n'.join(rows) + '\n')
    for k, v in sorted(stats.items()):
        print(f'{v:6}  {k}')
    if removed:
        print(f'{len(removed):6}  removed')
    print(f'written to {out}')


if __name__ == '__main__':
    main()

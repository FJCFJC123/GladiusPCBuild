"""Port a GameCube Gladius mod's gladius.bec onto the Xbox gladius.bec, and make an xdelta patch of the result.

    python tools/becmod.py <GC vanilla gladius.bec> <GC mod gladius.bec> <Xbox gladius.bec> <out dir>
                           [--xdelta <xdelta3.exe>] [--names <data dir> ...]

Every entry the mod adds or changes (against the GC vanilla bec, by hash and bytes) goes into a copy of the Xbox bec:
  - the mod's bytes equal the Xbox entry of that hash: nothing to do (an Xbox file the mod's folder carried).
  - a byte copy of a GC vanilla entry whose Xbox copy differs (models, textures - the platform-specific files): the
    Xbox copy of that source. Copies of GC-only entries keep the GC bytes (textures converted, below).
  - new content: textures (PTTP, GC CMPR 0x2000 / 0x2200) re-encoded as Xbox DXT1 / DXT5 (gcmod.gc_texture_to_xbox);
    other textures and new .pax / .pao models are reported, not written; everything else (text, scripts, .pan -
    the same bytes on both platforms) is the mod's file as is.
Entries the mod removes stay in the Xbox bec.

Xbox bec layout (what the engine checks, code 0x1FA580): header ' ceb', u16 version 3, u16 alignment 2048, u32 count,
u32 flags << 24 | largest stored (compressed) size (read buffer); 16-byte entries sorted by hash (hash, offset,
flags << 24 | stored size (0 = raw), size); data at 2048-aligned offsets, each followed by u32 crc of the file and,
when compressed, u32 crc of the zlib stream (crc = the path hash's table, no xors). The output keeps the Xbox data
region byte for byte (shifted by the table's growth) and appends the mod's entries, so the xdelta stays small.
Out: <out dir>/gladius.bec, becmod_report.txt, and with --xdelta gladius.bec.xdelta (source = the Xbox bec).
"""
import collections
import hashlib
import os
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import becdiff  # noqa: E402
import gcmod  # noqa: E402

ALIGN = 2048
MODEL_EXT = ('.pax', '.pao')


def _crc(d):
    """the bec's CRC32 (table, no initial / final xor) = zlib's crc32 with both xors undone"""
    return ~zlib.crc32(d, 0xFFFFFFFF) & 0xFFFFFFFF


def pad(n):
    return -n % ALIGN


def main():
    argv = sys.argv[1:]
    name_dirs, xdelta = [], None
    if '--names' in argv:
        i = argv.index('--names')
        name_dirs, argv = argv[i + 1:], argv[:i]
    if '--xdelta' in argv:
        i = argv.index('--xdelta')
        xdelta, argv = argv[i + 1], argv[:i] + argv[i + 2:]
    if len(argv) != 4:
        raise SystemExit(__doc__)
    van, mod, xbox = becdiff.Bec(argv[0]), becdiff.Bec(argv[1]), becdiff.Bec(argv[2])
    xbox_path, out = argv[2], argv[3]
    os.makedirs(out, exist_ok=True)
    names = becdiff.candidates(name_dirs) if name_dirs else {}

    print('diffing the mod against GC vanilla ...')
    diff = {}
    for h in sorted(mod.ents):
        d = mod.get(h)
        if h in van.ents and van.get(h) == d:
            continue
        diff[h] = d
    sizes = {len(d) for d in diff.values()}
    van_md5 = {}
    for vh, (_, _, sz) in van.ents.items():
        if sz in sizes:
            van_md5.setdefault(hashlib.md5(van.get(vh)).digest(), []).append(vh)
    # A repack tool stores the vanilla entries it cannot name again under a made-up name (becdiff.py): an added,
    # unnamed byte copy of a vanilla entry is such a duplicate - never loaded, not ported. The slow every-folder
    # name search runs on these candidates only.
    dup = [h for h, d in diff.items() if h not in van.ents and h not in names
           and hashlib.md5(d).digest() in van_md5 and not (h in xbox.ents and xbox.get(h) == d)]
    if dup and names:
        print(f'{len(dup)} unnamed copies of vanilla entries - searching every folder for their names ...')
        names.update(becdiff.deep_names(dup, names, diff))
    dup = {h for h in dup if h not in names}

    new, rows, stats = {}, [], collections.Counter()

    def nm(h):
        return names.get(h, f'(unnamed {h:08X})')

    for h, d in diff.items():
        status = 'changed' if h in van.ents else 'added'
        ext = os.path.splitext(names.get(h, ''))[1]
        x = xbox.get(h) if h in xbox.ents else None
        conv = None
        if x is not None and x == d:
            act = 'skip: same as the Xbox entry'
        elif h in dup:
            act = 'skip: repack duplicate of vanilla ' + nm(van_md5[hashlib.md5(d).digest()][0])
        else:
            srcs = van_md5.get(hashlib.md5(d).digest(), [])
            xsrc = next((s for s in srcs if s in xbox.ents and xbox.get(s) != d), None)
            if d[:4] == b'PTTP' and xsrc is None:
                conv = gcmod.gc_texture_to_xbox(d)
            if xsrc is not None:
                new[h] = xbox.get(xsrc)
                act = f'Xbox copy of {nm(xsrc)}'
            elif conv is not None:
                new[h] = conv
                act = 'GC texture converted to Xbox DXT'
            elif d[:4] == b'PTTP' or ext in MODEL_EXT:
                act = 'NOT WRITTEN: GC model / texture with no Xbox copy'
            else:
                new[h] = d
                act = 'mod file'
        key = act.split(' of ')[0].split(' duplicate')[0]
        stats[key] += 1
        rows.append(f'{status}	{act}	{h:08X}	{len(d)}	{nm(h)}')

    print('writing the Xbox bec ...')
    with open(xbox_path, 'rb') as f:
        hdr = bytearray(f.read(16))
        n = struct.unpack_from('<I', hdr, 8)[0]
        table = f.read(n * 16)
        ents = {}
        for i in range(n):
            eh, off, st, sz = struct.unpack_from('<4I', table, i * 16)
            ents[eh] = [off, st, sz]
        data_start = min(e[0] for e in ents.values())
        f.seek(0, 2)
        xbox_end = f.tell()
    count = len(set(ents) | set(new))
    new_start = 16 + count * 16
    new_start += pad(new_start)
    shift = max(0, new_start - data_start)
    shift += pad(shift)
    for e in ents.values():
        e[0] += shift
    tail, pos = [], xbox_end + shift
    pos += pad(pos)
    for h in sorted(new):
        d = new[h]
        z = zlib.compress(d, 9)
        if len(z) < len(d):
            blob, st = z + struct.pack('<2I', _crc(d), _crc(z)), len(z)
        else:
            blob, st = d + struct.pack('<I', _crc(d)), 0
        flags = ents[h][1] & 0xFF000000 if h in ents else 0
        ents[h] = [pos, flags | st, len(d)]
        blob += b'\0' * pad(len(blob))
        tail.append(blob)
        pos += len(blob)
    biggest = max(e[1] & 0xFFFFFF for e in ents.values())
    old = struct.unpack_from('<I', hdr, 12)[0]
    struct.pack_into('<II', hdr, 8, count, (old & 0xFF000000) | max(biggest, old & 0xFFFFFF))
    dst = os.path.join(out, 'gladius.bec')
    with open(dst, 'wb') as o, open(xbox_path, 'rb') as f:
        o.write(hdr)
        for h in sorted(ents):
            o.write(struct.pack('<4I', h, *ents[h]))
        o.write(b'\0' * (data_start + shift - o.tell()))
        f.seek(data_start)
        for b in iter(lambda: f.read(1 << 24), b''):
            o.write(b)
        o.write(b'\0' * pad(o.tell()))
        for b in tail:
            o.write(b)

    print('verifying ...')
    chk = becdiff.Bec(dst)
    bad = 0
    for h in ents:
        want = new[h] if h in new else xbox.get(h)
        if chk.get(h) != want:
            bad += 1
    with open(dst, 'rb') as f:                               # trailer crcs, as the engine checks them
        for h in new:
            off, st, sz = ents[h]
            st &= 0xFFFFFF
            f.seek(off)
            blob = f.read((st or sz) + 8)
            body = blob[:st or sz]
            c1, c2 = struct.unpack_from('<2I', blob, len(body))
            u = zlib.decompress(body) if st else body
            if c1 != _crc(u) or (st and c2 != _crc(body)):
                bad += 1
    rows.sort(key=lambda r: (r.split('\t')[1].split(' of ')[0], r.split('\t')[4]))
    with open(os.path.join(out, 'becmod_report.txt'), 'w') as f:
        f.write('\n'.join(rows) + '\n')
    for k, v in sorted(stats.items(), key=lambda kv: -kv[1]):
        print(f'{v:6}  {k}')
    print(f'{len(new)} entries written, {count} in the bec, verify: {"OK" if not bad else f"{bad} BAD"}')
    if bad:
        raise SystemExit(1)
    if xdelta:
        p = os.path.join(out, 'gladius.bec.xdelta')
        if os.path.exists(p):
            os.remove(p)
        subprocess.check_call([xdelta, '-e', '-9', '-S', 'none', '-B', str(1 << 30), '-s', xbox_path, dst, p])
        print(f'xdelta: {os.path.getsize(p):,} bytes -> {p}')


if __name__ == '__main__':
    main()

"""Give some faces of a Gladius Xbox static prop their own copy of a texture (the editor's texture paint on a shared
atlas patch: the faces that were painted keep the paint, every other face keeps the original).

    python tools/paxsplit.py prop.pax splits.json

splits.json: {"splits": [{"sub": <sub-mesh record index>, "tris": [[a, b, c], ...] (vertex indices of that
sub-mesh's vertex buffer, winding as drawn), "texture": "name", "new_texture": "name",
"data": "file with the painted texture's levels", "fmt": 0x4100 | 0x4300, "w": W, "h": H, "mips": N}]}

Per split, in the prop's own blocks (tools/xrnd.py, nothing existing moves - entries are appended):
  - the listed faces leave the sub-mesh: its remaining faces get a new strip (appended to the index block, the index
    entry repointed); the listed faces become a new sub-mesh (a copy of the record: same vertex buffer, new index
    entry), in the same list (A opaque / B transparent) right after the original
  - the new sub-mesh's material is a copy of the original's (programs + command streams) whose binds of the old
    texture object point at a new texture object named new_texture
  - TXTR / SELS entries for the new texture and sub-mesh, and the .ptx gets the painted texture appended (the shared
    texture itself stays as the editor saved it - the original pixels)
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ptx  # noqa: E402
import xrnd  # noqa: E402
from paxmerge import u32, cmd_stream  # noqa: E402
from xrnd import (SLOT_GEOM, SLOT_MESHPTRS, SLOT_SUBMESH, SLOT_IDXENT, SLOT_VBENT, SLOT_VBHDR, SLOT_MATPTRS,  # noqa: E402
                  SLOT_MATERIALS, SLOT_PROGRAMS, SLOT_CMD, SLOT_TEXTURES, SLOT_NAMES, SLOT_INDICES)


def strip_tris(ix, prim):
    """triangles of an index list, all in one winding (strip: odd ones flipped back), degenerate ones dropped"""
    out = []
    n = len(ix)
    if prim == 6:
        for k in range(n - 2):
            t = (ix[k], ix[k + 1], ix[k + 2]) if k % 2 == 0 else (ix[k], ix[k + 2], ix[k + 1])
            if len(set(t)) == 3:
                out.append(t)
    elif prim == 7:
        for k in range(1, n - 1):
            t = (ix[0], ix[k], ix[k + 1])
            if len(set(t)) == 3:
                out.append(t)
    else:
        for k in range(0, n - 2, 3):
            t = tuple(ix[k:k + 3])
            if len(set(t)) == 3:
                out.append(t)
    return out


def canon(t):
    """the same face whatever corner it starts at"""
    k = t.index(min(t))
    return t[k:] + t[:k]


def split(pax, ptx_data, splits):
    chunks = xrnd.read_chunks(pax)
    xi = next(i for i, c in enumerate(chunks) if c[0] == b'XRND')
    xr_tag, xr_ver, xr_num, xr_pay = chunks[xi]
    reloc, hdr, bl = xrnd.parse_xrnd(xr_pay, xr_ver)
    order = sorted(bl, key=lambda s: hdr[s // 4])
    n_sub, n_vb, n_ie, n_tex, n_mat = hdr[6], hdr[7], hdr[8], hdr[9], hdr[10]
    for s, size, n in ((SLOT_SUBMESH, 0x38, n_sub), (SLOT_IDXENT, 0x14, n_ie), (SLOT_VBENT, 0x14, n_vb),
                       (SLOT_VBHDR, 0x0C, n_vb), (SLOT_TEXTURES, 0x40, n_tex)):
        bl[s] = bytearray(bl[s][:size * n])
    for s in (SLOT_MATERIALS, SLOT_PROGRAMS, SLOT_CMD, SLOT_NAMES, SLOT_INDICES):
        bl[s] = bytearray(bl[s])
    geom = bytearray(bl[SLOT_GEOM])
    cnt_a, off_a, moff_a, cnt_b, off_b, moff_b = struct.unpack_from('<IIIIII', geom, 0x1C)
    cnt_b = cnt_b if cnt_b not in (0, 0xFFFFFFFF) and cnt_b < 0x10000 else 0
    mp, mt = bytes(bl[SLOT_MESHPTRS]), bytes(bl[SLOT_MATPTRS])
    lists = {'A': ([u32(mp, off_a + 4 * k) for k in range(cnt_a)], [u32(mt, moff_a + 4 * k) for k in range(cnt_a)]),
             'B': ([u32(mp, off_b + 4 * k) for k in range(cnt_b)], [u32(mt, moff_b + 4 * k) for k in range(cnt_b)])}
    names_block = bytes(bl[SLOT_NAMES])

    def add_name(s):
        off = len(bl[SLOT_NAMES])
        bl[SLOT_NAMES] += s.encode('latin1') + b'\0'
        return off

    def tex_name(to):
        no = u32(bl[SLOT_TEXTURES], to + 0x10)
        if no >= len(names_block):
            return ''
        return names_block[no:names_block.index(b'\0', no)].decode('latin1')

    def add_strip(tris):
        strip, _ = xrnd.tris_to_strip(tris)
        io = len(bl[SLOT_INDICES])
        io += -io % 4
        bl[SLOT_INDICES] += b'\0' * (io - len(bl[SLOT_INDICES]))
        bl[SLOT_INDICES] += struct.pack(f'<{len(strip)}H', *strip)
        return io, strip

    new_textures, new_sub_names, new_ptx, source_textures = [], [], [], []
    for sp in splits:
        so = sp['sub'] * 0x38
        where = next(((L, k) for L, (subs, _) in lists.items() for k, x in enumerate(subs) if x == so), None)
        if where is None:
            raise ValueError(f'sub-mesh {sp["sub"]} is not in the geometry lists')
        L, k = where
        sm = bytes(bl[SLOT_SUBMESH][so:so + 0x38])
        ie = u32(sm, 8)
        _, io, _, ic, prim = struct.unpack_from('<fIIII', bl[SLOT_IDXENT], ie)
        tris = strip_tris(list(struct.unpack_from(f'<{ic}H', bl[SLOT_INDICES], io)), prim)
        want = {canon(tuple(t)) for t in sp['tris']}
        moving = [t for t in tris if canon(t) in want]
        keep = [t for t in tris if canon(t) not in want]
        if not moving:
            print(f'  sub-mesh {sp["sub"]}: none of the painted faces found - skipped')
            continue
        # remaining faces: new strip, the entry repointed (a strip even if it was a list / fan)
        if keep:
            nio, strip = add_strip(keep)
        else:                                            # nothing left: one degenerate triangle
            nio = len(bl[SLOT_INDICES]) + (-len(bl[SLOT_INDICES]) % 4)
            bl[SLOT_INDICES] += b'\0' * (nio - len(bl[SLOT_INDICES]))
            strip = [moving[0][0]] * 3
            bl[SLOT_INDICES] += struct.pack('<3H', *strip)
        struct.pack_into('<fIIII', bl[SLOT_IDXENT], ie, 1.0, nio, xrnd.nondegenerate(strip), len(strip), 6)
        # the painted faces: a new index entry + sub-mesh record
        mio, mstrip = add_strip(moving)
        nie = len(bl[SLOT_IDXENT])
        bl[SLOT_IDXENT] += struct.pack('<fIIII', 1.0, mio, xrnd.nondegenerate(mstrip), len(mstrip), 6)
        nsm = bytearray(sm)
        struct.pack_into('<I', nsm, 8, nie)
        sub_name = f'{sp["new_texture"]}_faces'[:40]
        struct.pack_into('<I', nsm, 0x34, add_name(sub_name))
        nso = len(bl[SLOT_SUBMESH])
        bl[SLOT_SUBMESH] += nsm
        # material copy with the texture swapped
        mo = lists[L][1][k]
        nprog = u32(bl[SLOT_MATERIALS], mo + 4)
        old_to = new_to = None
        prog_offs = []
        for j in range(nprog):
            po = u32(bl[SLOT_MATERIALS], mo + 8 + 4 * j)
            pr = bytearray(bl[SLOT_PROGRAMS][po:po + 0x2C])
            st = bytearray(cmd_stream(bl[SLOT_CMD], u32(pr, 0x14)))
            i = 0
            while i + 2 <= len(st) and st[i]:
                ln = st[i + 1]
                if st[i] == 0x01 and st[i + 2] == 0 and ln >= 12:
                    to = u32(st, i + 8)
                    if tex_name(to).lower() == sp['texture'].lower():
                        if new_to is None:
                            old_to = to
                            nt = bytearray(bl[SLOT_TEXTURES][to:to + 0x40])
                            struct.pack_into('<I', nt, 0x10, add_name(sp['new_texture']))
                            new_to = len(bl[SLOT_TEXTURES])
                            bl[SLOT_TEXTURES] += nt
                        struct.pack_into('<I', st, i + 8, new_to)
                i += ln if ln >= 4 else 4
            co = len(bl[SLOT_CMD])
            bl[SLOT_CMD] += st
            struct.pack_into('<I', pr, 0x14, co)
            prog_offs.append(len(bl[SLOT_PROGRAMS]))
            bl[SLOT_PROGRAMS] += pr
        if new_to is None:
            raise ValueError(f'sub-mesh {sp["sub"]}\'s material does not bind {sp["texture"]}')
        nmo = len(bl[SLOT_MATERIALS])
        bl[SLOT_MATERIALS] += struct.pack(f'<fI{len(prog_offs)}I', 1.0, len(prog_offs), *prog_offs)
        lists[L][0].insert(k + 1, nso)
        lists[L][1].insert(k + 1, nmo)
        new_textures.append(sp['new_texture'])
        source_textures.append(sp['texture'])
        new_sub_names.append(sub_name)
        data = open(sp['data'], 'rb').read()
        new_ptx.append((sp['new_texture'], sp['fmt'], sp['mips'], sp['w'], sp['h'], data))
        print(f'  sub-mesh {sp["sub"]}: {len(moving)} painted faces -> own texture {sp["new_texture"]}, {len(keep)} keep '
              f'{sp["texture"]}')
    if not new_textures:
        return pax, ptx_data
    bl[SLOT_INDICES] += b'\0' * (-len(bl[SLOT_INDICES]) % 4)
    bl[SLOT_NAMES] += b'\0' * (-len(bl[SLOT_NAMES]) % 4)
    a, am = lists['A']
    b, bm = lists['B']
    bl[SLOT_MESHPTRS] = struct.pack(f'<{len(a) + len(b)}I', *(a + b))
    bl[SLOT_MATPTRS] = struct.pack(f'<{len(am) + len(bm)}I', *(am + bm))
    struct.pack_into('<III', geom, 0x1C, len(a), 0, 0)
    if b:
        struct.pack_into('<III', geom, 0x28, len(b), 4 * len(a), 4 * len(am))
    bl[SLOT_GEOM] = geom
    n_new = len(new_textures)
    hdr = list(hdr)
    hdr[6] += n_new
    hdr[8] += n_new
    hdr[9] += n_new
    hdr[10] += n_new
    new_xr = xrnd.build_xrnd(hdr, bl, order, reloc or (0,))
    out = []
    for t, v, n, p in chunks:
        if t == b'XRND':
            out.append((t, xr_ver, xr_num, new_xr))
        elif t == b'TXTR':
            # each new entry copies its source texture's entry (name, saved pointer, width, height, flags); zero sizes would draw
            # without the texture
            entry_len = xrnd.TXTR_ENTRY                 # fixed 0x98 (the payload carries padding)
            entries = [bytes(p[k * entry_len:(k + 1) * entry_len]) for k in range(n)]
            add = bytearray()
            for name, src in zip(new_textures, source_textures):
                keys = ((src + '.tga').lower().encode('latin1'), src.lower().encode('latin1'))
                e0 = next((x for x in entries if x[:0x80].split(b'\0')[0].lower() in keys), entries[0])
                e = bytearray(e0)
                nm = (name + '.tga').encode('latin1')[:0x7F]
                e[:0x80] = nm + b'\0' * (0x80 - len(nm))
                e[0x84:0x88] = b'\0' * 4                 # saved runtime pointer
                add += e
            out.append((t, v, n + n_new, bytes(p[:entry_len * n]) + bytes(add)))
        elif t == b'SELS':
            names = [s for s in bytes(p).split(b'\0') if s]
            subs, texs = names[:n_sub], names[n_sub:]
            names = subs + [s.encode('latin1') for s in new_sub_names] + texs + [(nm + '.tga').encode('latin1') for nm in new_textures]
            out.append((t, v, n, b''.join(s + b'\0' for s in names)))
        else:
            out.append((t, v, n, p))
    return xrnd.write_chunks(out), ptx.build_ptx_entries(ptx.read_ptx_entries(ptx_data) + new_ptx)


def main():
    pax_path, spec = sys.argv[1], sys.argv[2]
    splits = json.load(open(spec))['splits']
    ptx_path = os.path.splitext(pax_path)[0] + '.ptx'
    pax, pt = split(open(pax_path, 'rb').read(), open(ptx_path, 'rb').read(), splits)
    open(pax_path, 'wb').write(pax)
    open(ptx_path, 'wb').write(pt)
    print(f'split {len(splits)} painted region(s) into own textures -> {pax_path}')


if __name__ == '__main__':
    main()

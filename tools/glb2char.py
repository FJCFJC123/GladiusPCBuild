"""Skinned GLB -> Gladius Xbox character .pax (skinned XRND) on a retail character's skeleton.

    python tools/glb2char.py model.glb bec_unpacked/data/characters/legionnaire/legionnaire.pax out/legionnaire.pax

The GLB must be skinned to joints named like the template's bones (e.g. a model rigged to a skeleton exported by
tools/xchar.py); its mesh is taken as already in the template's bind pose (glTF y-up -> Gladius z-up). The template
supplies every chunk but XRND / TXTR (VERS, CPRT, SELS, STYP, NAME, SKEL), the per-LOD hide masks (XRND relocation
list) and the material / program records; textures are referenced by name (the .ptx is written separately).

Written XRND (see tools/xchar.py for the format): one vertex buffer, stream 0 stride 28 (position, packed normal,
UV, u8 weights[3] + pad) and stream 1 stride 6 (int16[3] = 3 x palette slot, -1 unused); one sub-mesh per texture
and per palette of at most --max-bones bones (retail max 45); sub-mesh mask 0 (drawn at every LOD); vertex-format
word 0x40000420 | (1 + weights per vertex).
"""
import argparse
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glb2pax  # noqa: E402
import xrnd  # noqa: E402
from xchar import read_skeleton  # noqa: E402

SLOT_PAL = 0x44
COMP = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
NC = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def acc(gltf, blob, i):
    a = gltf['accessors'][i]
    bv = gltf['bufferViews'][a['bufferView']]
    dt = np.dtype(COMP[a['componentType']])
    n = NC[a['type']]
    off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    stride = bv.get('byteStride', dt.itemsize * n)
    raw = np.frombuffer(blob, np.uint8, stride * (a['count'] - 1) + dt.itemsize * n, off)
    arr = np.lib.stride_tricks.as_strided(raw, (a['count'], dt.itemsize * n), (stride, 1)).copy().view(dt)
    arr = arr.reshape(a['count'], n).astype(np.float64 if dt == np.float32 else np.int64)
    if a.get('normalized'):
        arr = arr / float(np.iinfo(dt).max)
    return arr


def load_skinned(path):
    """-> [{'texture', 'pos' (n,3) z-up, 'nrm', 'uv', 'joints' (n,4) joint names, 'weights' (n,4), 'tris' (m,3)}]"""
    gltf, blob = glb2pax.read_glb(path)
    skin = gltf['skins'][0]
    jnames = [gltf['nodes'][j].get('name', f'joint{j}') for j in skin['joints']]
    meshes = [n['mesh'] for n in gltf['nodes'] if 'mesh' in n and 'skin' in n]
    out = []
    for mi in meshes:
        for p in gltf['meshes'][mi]['primitives']:
            at = p['attributes']
            if 'JOINTS_0' not in at:
                continue
            mat = gltf['materials'][p['material']] if 'material' in p else {}
            tex = glb2pax.texture_name(gltf, mat) or mat.get('name') or 'white'
            tex = os.path.splitext(tex)[0] if tex.lower().endswith(('.tga', '.png', '.jpg', '.jpeg')) else tex
            pos = acc(gltf, blob, at['POSITION'])
            nrm = acc(gltf, blob, at['NORMAL']) if 'NORMAL' in at else np.tile([0, 1.0, 0], (len(pos), 1))
            uv = acc(gltf, blob, at['TEXCOORD_0']) if 'TEXCOORD_0' in at else np.zeros((len(pos), 2))
            j = acc(gltf, blob, at['JOINTS_0']).astype(int)
            w = acc(gltf, blob, at['WEIGHTS_0'])
            idx = acc(gltf, blob, p['indices']).ravel().astype(int) if 'indices' in p else np.arange(len(pos))
            zup = lambda v: np.stack([v[:, 0], -v[:, 2], v[:, 1]], 1)
            out.append({'texture': tex, 'pos': zup(pos), 'nrm': zup(nrm), 'uv': uv,
                        'joints': np.array(jnames, object)[j], 'weights': w, 'tris': idx.reshape(-1, 3),
                        'sets': (p.get('extras') or {}).get('sets', [])})     # visibility sets (xchar export)
    return out


def command_template(cmds, prog, mats, mat, texobj=b'', names=b'', stored=None):
    """a material command stream that binds exactly one texture and sets a skinned vertex format - one binding a
    texture stored in the template's own .ptx when `stored` (lower-case names) is given: materials of shared textures
    stored elsewhere (eyes, teeth) render white when reused
    -> (stream bytes, offset of its texture-object field, offset of its format word)"""
    found = []
    for k in range(len(mats) // 4):
        moff, = struct.unpack_from('<I', mats, k * 4)
        poff, = struct.unpack_from('<I', mat, moff + 8)
        c0, = struct.unpack_from('<I', prog, poff + 0x14)
        c, tex, fmt = c0, [], []
        while c + 2 <= len(cmds) and cmds[c]:
            op, ln = cmds[c], cmds[c + 1]
            if op == 0x01 and ln >= 12:
                tex.append(c + 8 - c0)
            if op == 0x09:
                fmt.append(c + 4 - c0)
            c += max(ln, 4)
        if len(tex) == 1 and len(fmt) == 1 and struct.unpack_from('<I', cmds, c0 + fmt[0])[0] & 0x20:
            to, = struct.unpack_from('<I', cmds, c0 + tex[0])
            noff, = struct.unpack_from('<I', texobj, to + 0x10) if to + 0x14 <= len(texobj) else (None,)
            name = names[noff:names.index(b'\0', noff)].decode('latin1').lower() if noff is not None else ''
            found.append((stored is not None and name in stored, (bytes(cmds[c0:c + 4]), tex[0], fmt[0])))
    if not found:
        raise ValueError('template has no single-texture skinned material')
    return max(found, key=lambda f: f[0])[1]


def build_character(template, meshes, max_bones=40, skel=None, template_ptx=None, sizes=None):
    """skel: replacement SKEL payload (tools/retarget.py: the template's bones with the model's proportions)
    template_ptx: the template's .ptx bytes - picks templates of textures stored in it (not shared external ones)
    sizes: {texture: (width, height)} for the TXTR entries"""
    chunks = xrnd.read_chunks(template)
    by = {t: (t, v, n, p) for t, v, n, p in chunks}
    bones = read_skeleton({t: (v, n, p) for t, v, n, p in chunks})
    # visibility sets (STYP type 1) -> sub-mesh mask bits; LOD sets are left off so a piece shows at every LOD
    sels = [s.decode('latin1') for s in by[b'SELS'][3].split(b'\0') if s]
    styp = by[b'STYP'][3]
    vis = [sels[i] for t, i in (struct.unpack_from('<HH', styp, k * 4) for k in range(by[b'STYP'][2])) if t == 1]
    vis_bit = {s.lower(): b for b, s in enumerate(vis) if not s.lower().startswith('set_lod')}
    bone_of = {b['name'].lower(): i for i, b in enumerate(bones)}
    _, xver, xnum, xpay = by[b'XRND']
    reloc, thdr, tb = xrnd.parse_xrnd(xpay, xver)
    t_sub = tb[xrnd.SLOT_SUBMESH][:0x38]
    t_prog = tb[xrnd.SLOT_PROGRAMS][:0x2C]
    t_tex = tb[xrnd.SLOT_TEXTURES][:0x40]
    stored = None
    if template_ptx:
        from xchar import read_ptx
        stored = {n.lower() for n in read_ptx(template_ptx)}
    try:
        t_cmd, cmd_tex, cmd_fmt = command_template(tb[xrnd.SLOT_CMD], tb[xrnd.SLOT_PROGRAMS], tb[xrnd.SLOT_MATPTRS],
                                                   tb[xrnd.SLOT_MATERIALS], tb[xrnd.SLOT_TEXTURES],
                                                   tb[xrnd.SLOT_NAMES], stored)
    except ValueError:
        # every material of the class binds two textures (the Scarab: colour + shine ramp): borrow the retail
        # Legionnaire's single-texture skinned material stream (self-contained; its texture field is rewritten)
        donor = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'bec_unpacked', 'data', 'characters',
                             'legionnaire', 'legionnaire.pax')
        dch = {t: (v, n, p) for t, v, n, p in xrnd.read_chunks(open(donor, 'rb').read())}
        _, _, dtb = xrnd.parse_xrnd(dch[b'XRND'][2], dch[b'XRND'][0])
        t_cmd, cmd_tex, cmd_fmt = command_template(dtb[xrnd.SLOT_CMD], dtb[xrnd.SLOT_PROGRAMS],
                                                   dtb[xrnd.SLOT_MATPTRS], dtb[xrnd.SLOT_MATERIALS])
        print('  template has no single-texture skinned material: using the Legionnaire\'s')

    missing = sorted({j for m in meshes for j, w in zip(m['joints'].ravel(), m['weights'].ravel())
                      if w > 0 and str(j).lower() not in bone_of})
    if missing:
        raise ValueError(f'joints not in the template skeleton: {missing[:12]}{"..." if len(missing) > 12 else ""}')

    textures = []
    for m in meshes:
        if m['texture'] not in textures:
            textures.append(m['texture'])
    names = bytearray()
    name_off = {}
    for t in textures:
        name_off[t] = len(names)
        names += t.encode('latin1') + b'\0'
    names += b'\0' * (-len(names) % 4)

    # per vertex: up to 3 template bone indices + byte weights summing to 255
    subs = []                                            # (texture, palette, [(vbone[3], vw[3], pos, nrm, uv)], tris)
    for m in meshes:
        bi = np.vectorize(lambda j: bone_of.get(str(j).lower(), 0))(m['joints'])
        w = m['weights'].copy()
        order = np.argsort(-w, 1)[:, :3]
        bi = np.take_along_axis(bi, order, 1)
        w = np.take_along_axis(w, order, 1)
        w[w < 1e-3] = 0
        s = w.sum(1, keepdims=True)
        w = np.where(s > 0, w / np.maximum(s, 1e-9), [[1, 0, 0]])
        wb = np.round(w * 255).astype(int)
        wb[:, 0] += 255 - wb.sum(1)
        bi = np.where(wb > 0, bi, -1)
        # split the triangles into palettes of at most max_bones
        pal, tris = [], []

        def flush():
            if tris:
                subs.append((m, list(pal), np.array(tris)))

        for t in m['tris']:
            need = {int(b) for v in t for b in bi[v] if b >= 0}
            if len(set(pal) | need) > max_bones:
                flush()
                pal, tris = [], []
            for b in sorted(need - set(pal)):
                pal.append(b)
            tris.append(t)
        flush()
        m['_bi'], m['_wb'] = bi, wb

    vertices0, vertices1, indices = bytearray(), bytearray(), bytearray()
    # vertex colours (Melina's ZBrush polypaint): the retail 32-byte skinned vertex with a colour (xchar.py)
    has_col = any(m.get('col') is not None for m in meshes)
    if has_col:
        for m in meshes:
            if m.get('col') is None:
                m['col'] = np.ones((len(m['pos']), 4))
    stride = 32 if has_col else 28
    submesh, idxent, meshptrs, matptrs, materials, programs, cmds, palettes = (bytearray() for _ in range(8))
    nv = 0
    all_pos = []
    for k, (m, pal, tris) in enumerate(subs):
        tex = m['texture']
        slot = {b: i for i, b in enumerate(pal)}
        local = {}
        loc_tris = []
        nweights = 1
        for t in tris:
            tt = []
            for v in t:
                if v not in local:
                    local[v] = nv + len(local)
                    p, n, uv = m['pos'][v], m['nrm'][v], m['uv'][v]
                    wb, bi = m['_wb'][v], m['_bi'][v]
                    nweights = max(nweights, int((wb > 0).sum()))
                    ln = float(np.linalg.norm(n))
                    nn = n / ln if np.isfinite(ln) and ln > 1e-12 else np.array([0.0, 0.0, 1.0])  # degenerate normals
                    if has_col:                         # stride 32: colour (A8R8G8B8) after the normal
                        c = m['col'][v]
                        rgba = [int(round(255 * min(1.0, max(0.0, float(x))) ** (1 / 2.2))) for x in c[:3]]
                        a = int(round(255 * min(1.0, max(0.0, float(c[3]) if len(c) > 3 else 1.0))))
                        vertices0 += struct.pack('<fffIIffBBBB', *p, xrnd.pack_normal(*nn),
                                                 a << 24 | rgba[0] << 16 | rgba[1] << 8 | rgba[2], *uv,
                                                 *[int(x) for x in wb], 0)
                    else:
                        vertices0 += struct.pack('<fffIffBBBB', *p, xrnd.pack_normal(*nn), *uv, *[int(x) for x in wb], 0)
                    vertices1 += struct.pack('<hhh', *[slot[int(b)] * 3 if b >= 0 else -1 for b in bi])
                    all_pos.append(tuple(p))
                tt.append(local[v])
            loc_tris.append(tuple(tt))
        nv += len(local)
        strip, _ = xrnd.tris_to_strip(loc_tris)
        idx_off = len(indices)
        indices += struct.pack(f'<{len(strip)}H', *strip)
        indices += b'\0' * (-len(indices) % 4)
        idxent += struct.pack('<fIIII', 1.0, idx_off, xrnd.nondegenerate(strip), len(strip), 6)
        r, c = xrnd.bounding_sphere(all_pos[nv - len(local):nv])
        sm = bytearray(t_sub)
        struct.pack_into('<II', sm, 4, 0, k * 0x14)
        struct.pack_into('<4f', sm, 0x0C, r, *c)
        mask = sum(1 << vis_bit[s.lower()] for s in m.get('sets', []) if s.lower() in vis_bit)
        struct.pack_into('<II', sm, 0x2C, len(palettes), mask)   # palette offset, selection mask (0 = always)
        palettes += bytes([len(pal)] + pal)
        meshptrs += struct.pack('<I', k * 0x38)
        submesh += sm
        matptrs += struct.pack('<I', k * 0x0C)
        materials += struct.pack('<fII', 1.0, 1, k * 0x2C)
        pr = bytearray(t_prog)
        struct.pack_into('<I', pr, 0x14, len(cmds))
        programs += pr
        cm = bytearray(t_cmd)
        struct.pack_into('<I', cm, cmd_tex, textures.index(tex) * 0x40)
        word = struct.unpack_from('<I', cm, cmd_fmt)[0]
        struct.pack_into('<I', cm, cmd_fmt, (word & ~0x1F) | (0x10 if has_col else 0) | (1 + nweights))
        cmds += cm
    if nv > 0xFFFF:
        raise ValueError(f'{nv} vertices: more than 65535 in one vertex buffer')
    palettes += b'\0' * (-len(palettes) % 4)

    texobj = bytearray()
    for t in textures:
        to = bytearray(t_tex)
        struct.pack_into('<I', to, 0x10, name_off[t])
        texobj += to
    r, c = xrnd.bounding_sphere(all_pos)
    geom = bytearray(tb[xrnd.SLOT_GEOM])
    struct.pack_into('<I', geom, 0x1C, len(subs))
    struct.pack_into('<4f', geom, 0x50, r, *c)
    vbent = struct.pack('<5I', 0, nv, 0, 0x14, stride) + struct.pack('<5I', 12, nv, 0, 0xFFFFFFFF, 6)
    vbhdr = struct.pack('<3I', 0xFFFFFFFF, 0, 0xFFFFFFFF) + struct.pack('<3I', 0xFFFFFFFF, nv * stride, 0xFFFFFFFF)
    blocks = {xrnd.SLOT_GEOM: geom, xrnd.SLOT_MESHPTRS: meshptrs, xrnd.SLOT_SUBMESH: submesh,
              xrnd.SLOT_IDXENT: idxent, xrnd.SLOT_VBENT: vbent, xrnd.SLOT_VBHDR: vbhdr, SLOT_PAL: palettes,
              xrnd.SLOT_MATPTRS: matptrs, xrnd.SLOT_MATERIALS: materials, xrnd.SLOT_PROGRAMS: programs,
              xrnd.SLOT_CMD: cmds, xrnd.SLOT_TEXTURES: texobj, xrnd.SLOT_NAMES: names,
              xrnd.SLOT_INDICES: indices, xrnd.SLOT_VERTICES: vertices0 + vertices1}
    order = [xrnd.SLOT_GEOM, xrnd.SLOT_MESHPTRS, xrnd.SLOT_SUBMESH, xrnd.SLOT_IDXENT, xrnd.SLOT_VBENT,
             xrnd.SLOT_VBHDR, SLOT_PAL, xrnd.SLOT_MATPTRS, xrnd.SLOT_MATERIALS, xrnd.SLOT_PROGRAMS, xrnd.SLOT_CMD,
             xrnd.SLOT_TEXTURES, xrnd.SLOT_NAMES, xrnd.SLOT_INDICES, xrnd.SLOT_VERTICES]
    hdr = list(thdr)
    hdr[5:11] = [1, len(subs), 2, len(subs), len(textures), len(subs)]
    xr = xrnd.build_xrnd(hdr, blocks, order, reloc or (0,))

    # TXTR: 0x80 bytes name, +0x80 -1, +0x84 exporter runtime value, +0x88 width, +0x8C height, +0x90 3 ...;
    # the template entry is one of a texture stored in the template's .ptx when known
    _, tver, tnum, t_txtr = by[b'TXTR']
    elen = len(t_txtr) // max(1, tnum)
    tent = t_txtr[:elen]
    for i in range(tnum):
        e = t_txtr[i * elen:(i + 1) * elen]
        if stored and e[:e.index(b'\0')].decode('latin1').lower().removesuffix('.tga') in stored:
            tent = e
            break
    txtr = bytearray()
    for t in textures:
        e = bytearray(tent)
        nm = (t + '.tga').encode('latin1')[:0x7F]
        e[:0x80] = nm + b'\0' * (0x80 - len(nm))
        w, h = (sizes or {}).get(t, (0, 0))
        struct.pack_into('<III', e, 0x84, 0, w, h)
        txtr += e
    out = []
    for tag, ver, num, payload in chunks:
        if tag == b'TXTR':
            out.append((tag, ver, len(textures), txtr))
        elif tag == b'XRND':
            out.append((tag, xver, xnum, xr))
        elif tag == b'SKEL' and skel is not None:
            out.append((tag, ver, num, skel))
        else:
            out.append((tag, ver, num, payload))
    stats = {'submeshes': len(subs), 'vertices': nv, 'textures': textures,
             'max_palette': max(len(s[1]) for s in subs)}
    for m in meshes:
        m.pop('_bi', None), m.pop('_wb', None)
    return xrnd.write_chunks(out), stats


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('glb')
    ap.add_argument('template', help='retail character .pax whose skeleton the GLB is rigged to')
    ap.add_argument('out')
    ap.add_argument('--max-bones', type=int, default=40, help='bones per sub-mesh palette (retail max 45)')
    args = ap.parse_args()
    meshes = load_skinned(args.glb)
    data, st = build_character(open(args.template, 'rb').read(), meshes, args.max_bones)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    open(args.out, 'wb').write(data)
    print(f'wrote {args.out} ({len(data)} bytes): {st["submeshes"]} sub-meshes, {st["vertices"]} vertices, '
          f'largest palette {st["max_palette"]} bones, textures {st["textures"]}')


if __name__ == '__main__':
    main()

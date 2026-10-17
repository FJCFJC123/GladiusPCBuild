"""Gladius Xbox characters (.pax with SKEL + skinned XRND) - reader and GLB export.

    python tools/xchar.py bec_unpacked/data/characters/legionnaire/legionnaire.pax out.glb [--all-lods]

Skeleton (SKEL, 32 bytes per bone): float position[3], float rotation[4] (x, y, z, w; identity is stored as
w = -1), u16 (self << 8 | 2), u16 (parent << 8 | self). Positions / rotations are local to the parent; bone 0
('character') is the root. Bone names are the first entries of the NAME chunk.

Skinned XRND (see tools/xrnd.py): two vertex-buffer entries over the same vertices -
  stream 0, stride 28: position 12, packed normal 4, UV 8, weights u8[3] + pad
  stream 0, stride 32: position 12, packed normal 4, colour 4, UV 8, weights u8[3] + pad
  stream 1, stride 6:  int16[3] bone references (-1 = unused), each = 3 x palette slot (3 constant registers
                       per bone matrix)
Sub-mesh +0x2C = offset of its palette in block +44: u8 count, u8 bone[count]; sub-mesh +0x30 = selection-set
mask (bit n = SELS entry n: set_LOD0, set_faceEyes, ...). Materials: block +48 per-sub-mesh offsets into block
+4C (1.0f, program count, program offsets into +50); program +0x14 = command stream offset in +64; command op
0x01 (len 12): slot, texture object offset in +70 (0x40 each, +0x10 = name offset in +78).
"""
import argparse
import io
import json
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xrnd  # noqa: E402


# ------------------------------------------------------------------------------------------------ reading
def unpack_normal(p):
    x, y, z = p & 0x7FF, (p >> 11) & 0x7FF, (p >> 22) & 0x3FF
    x, y, z = x - 2048 if x >= 1024 else x, y - 2048 if y >= 1024 else y, z - 1024 if z >= 512 else z
    return x / 1023.0, y / 1023.0, z / 511.0


def cstr(buf, off):
    return buf[off:buf.index(b'\0', off)].decode('latin1')


def read_skeleton(chunks):
    names = [s.decode('latin1') for s in chunks[b'NAME'][2].split(b'\0')]
    _, n, sk = chunks[b'SKEL']
    bones = []
    for i in range(n):
        px, py, pz, qx, qy, qz, qw = struct.unpack_from('<7f', sk, i * 32)
        _, link = struct.unpack_from('<HH', sk, i * 32 + 28)
        bones.append({'name': names[i], 'parent': (link >> 8) if i else -1, 'pos': (px, py, pz),
                      'rot': (qx, qy, qz, qw)})
    return bones


def read_character(pax):
    """-> dict: bones, sets (SELS names), verts {pos, nrm, uv, joints[n,3], weights[n,3]}, submeshes
    [{tris, mask, palette, textures}]"""
    chunks = {t: (v, n, p) for t, v, n, p in xrnd.read_chunks(pax)}
    names = [s.decode('latin1') for s in chunks[b'SELS'][2].split(b'\0') if s]
    # STYP (u16 type, u16 SELS index per set): type 1 = visibility set - these take the sub-mesh mask bits, in
    # order (set_LOD0, set_faceEyes, ..., set_LOD1..4, set_shieldStrap); type 8 = tint set (no bit)
    _, nst, st = chunks[b'STYP']
    sets = [names[i] for t, i in (struct.unpack_from('<HH', st, k * 4) for k in range(nst)) if t == 1]
    bones = read_skeleton(chunks) if b'SKEL' in chunks else []
    _, hdr, B = xrnd.parse_xrnd(chunks[b'XRND'][2], chunks[b'XRND'][0])
    vbe = B[0x3C]
    entries = [struct.unpack_from('<IIIII', vbe, i * 0x14) for i in range(len(vbe) // 0x14)]
    nv, stride = entries[0][1], entries[0][4]
    if stride not in (28, 32):
        raise ValueError(f'not a skinned model (stride {stride})')
    V = B[0x6C]
    a = np.frombuffer(V, np.uint8, nv * stride).reshape(nv, stride)
    pos = a[:, 0:12].copy().view('<f4').reshape(nv, 3)
    nrm = np.array([unpack_normal(int(p)) for p in a[:, 12:16].copy().view('<u4').ravel()], np.float32)
    o = 16 + (4 if stride == 32 else 0)
    uv = a[:, o:o + 8].copy().view('<f4').reshape(nv, 2)
    wts = a[:, o + 8:o + 11].astype(np.float32) / 255.0
    refs = np.frombuffer(V, '<i2', nv * 3, nv * stride).reshape(nv, 3)
    sm, ie, idx, pal = B[0x34], B[0x38], B[0x68], B.get(0x44, b'')
    mats, mat, prog, cmds, tobj, names = B[0x48], B[0x4C], B[0x50], B[0x64], B[0x70], B[0x78]
    joints = np.zeros((nv, 3), np.int32)
    owner = np.full(nv, -1, np.int32)
    conflicts = 0
    subs = []
    for k in range(len(sm) // 0x38):
        ient, = struct.unpack_from('<I', sm, k * 0x38 + 8)
        poff, mask = struct.unpack_from('<II', sm, k * 0x38 + 0x2C)
        _, ioff, _, icnt, _ = struct.unpack_from('<fIIII', ie, ient)
        strip = struct.unpack_from(f'<{icnt}H', idx, ioff)
        palette = list(pal[poff + 1:poff + 1 + pal[poff]]) if pal else []
        # textures: material -> its programs -> command op 0x01 entries (slot, texture). The streams are render
        # state: a material that binds nothing keeps the previous draw's textures (resolved in draw order later)
        moff, = struct.unpack_from('<I', mats, k * 4)
        _, nprog = struct.unpack_from('<fI', mat, moff)
        tex = []
        for j in range(nprog):
            poff2, = struct.unpack_from('<I', mat, moff + 8 + j * 4)
            c, = struct.unpack_from('<I', prog, poff2 + 0x14)
            while c + 2 <= len(cmds) and cmds[c] != 0:
                op, ln = cmds[c], cmds[c + 1]
                if op == 0x01 and ln >= 12:
                    slot, to = struct.unpack_from('<II', cmds, c + 4)
                    noff, = struct.unpack_from('<I', tobj, to + 0x10)
                    tex.append((slot, cstr(names, noff)))
                c += max(ln, 4)
        tris = []
        for i in range(len(strip) - 2):
            t = (strip[i], strip[i + 1], strip[i + 2]) if i % 2 == 0 else (strip[i + 1], strip[i], strip[i + 2])
            if len(set(t)) == 3:
                tris.append(t)
        for v in set(strip):
            r = refs[v]
            j = [palette[x // 3] if x >= 0 and x // 3 < len(palette) else 0 for x in r]
            if owner[v] >= 0 and list(joints[v]) != j:
                conflicts += 1
            joints[v], owner[v] = j, k
        subs.append({'tris': tris, 'mask': mask, 'palette': palette, 'binds': tex})
    wsum = wts.sum(1, keepdims=True)
    wts = np.where(wsum > 0, wts / np.maximum(wsum, 1e-6), np.array([1.0, 0, 0], np.float32))
    return {'bones': bones, 'sets': sets, 'pos': pos, 'nrm': nrm, 'uv': uv, 'joints': joints, 'weights': wts,
            'submeshes': subs, 'conflicts': conflicts}


def read_ptx(data):
    """{texture name (no extension, lower case): RGBA uint8 array} - top mip of each DXT1 / DXT5 texture"""
    chunks = {t: (v, n, p) for t, v, n, p in xrnd.read_chunks(data)}
    out = {}
    _, n, pf = chunks[b'PFHD']
    nm = chunks[b'NMPT'][2]
    # entry data offsets are relative to the entry inside the file: locate PFHD's payload in the file
    pf_at = data.index(pf)
    for i in range(n):
        fmt, mips, _, w, h, size, off, noff = struct.unpack_from('<HHIHHIII', pf, i * 32)
        name = cstr(nm, noff).lower()
        name = name[:-4] if name.endswith('.tga') else name
        at = pf_at + i * 32 + off
        if off != 0xFFFFFFFF and fmt in (0x4100, 0x4300):    # -1: stored in a shared bundle, not here
            out[name] = decode_dxt(data[at:at + size], w, h, fmt == 0x4300)
    return out


def _565(c):
    return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1)


def decode_dxt(buf, w, h, dxt5):
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    bs = 16 if dxt5 else 8
    b = np.frombuffer(buf, np.uint8, bw * bh * bs).reshape(bw * bh, bs)
    col = b[:, 8:16] if dxt5 else b
    c0 = col[:, 0:2].copy().view('<u2').ravel().astype(np.int32)
    c1 = col[:, 2:4].copy().view('<u2').ravel().astype(np.int32)
    bits = col[:, 4:8].copy().view('<u4').ravel()
    p0, p1 = _565(c0), _565(c1)
    four = (c0 > c1) | dxt5
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)                                         # (N,4,3)
    ix = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3            # (N,16)
    rgb = np.take_along_axis(pal, ix[:, :, None].astype(np.int64).repeat(3, 2), 1)
    alpha = np.full((len(b), 16), 255, np.int32)
    if dxt5:
        a0, a1 = b[:, 0].astype(np.int32), b[:, 1].astype(np.int32)
        ab = np.zeros(len(b), np.uint64)
        for k in range(6):
            ab |= b[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
        ai = ((ab[:, None] >> (3 * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int32)
        eight = np.stack([a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)], 1)
        six = np.stack([a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] +
                       [np.zeros_like(a0), np.full_like(a0, 255)], 1)
        apal = np.where((a0 > a1)[:, None], eight, six)
        alpha = np.take_along_axis(apal, ai, 1)
    elif True:
        alpha = np.where((~four[:, None]) & (ix == 3), 0, 255)
    px = np.concatenate([rgb, alpha[:, :, None]], 2).astype(np.uint8)          # (N,16,4)
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w]


# ------------------------------------------------------------------------------------------------ GLB export
def _yup(v):                                    # Gladius z-up -> glTF y-up
    v = np.asarray(v, np.float32)
    return np.stack([v[..., 0], v[..., 2], -v[..., 1]], -1)


def _qmat(q):
    """rotation matrix of a Gladius quaternion (SKEL / .pan store the inverse rotation: conjugate first)"""
    x, y, z, w = -q[0], -q[1], -q[2], q[3]
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def bone_globals(bones):
    """global bind matrices (4x4, Gladius space)"""
    G = []
    for b in bones:
        L = np.eye(4)
        L[:3, :3] = _qmat(b['rot'])
        L[:3, 3] = b['pos']
        G.append(L if b['parent'] < 0 else G[b['parent']] @ L)
    return G


def drawn(ch, lod=0):
    """[(sub-mesh index, sub-mesh)] drawn at one LOD (None = all), in draw order, each with 'textures' = the
    texture bound per slot when it draws (bindings carry over from earlier draws)"""
    lod_bits = {int(s[7:]): i for i, s in enumerate(ch['sets']) if s.lower().startswith('set_lod')}
    all_lod = sum(1 << b for b in lod_bits.values())
    state, out = {}, []
    for k, s in enumerate(ch['submeshes']):
        if lod is not None and s['mask'] & all_lod and not s['mask'] >> lod_bits.get(lod, 0) & 1:
            continue
        for slot, name in s['binds']:
            state[slot] = name
        out.append((k, dict(s, textures=[state[x] for x in sorted(state)])))
    return out


def _yup_quat(q):
    """Gladius quaternion (inverse rotation, z-up) -> glTF rotation (x, y, z, w), y-up"""
    q = np.asarray(q, np.float64)
    x, y, z, w = -q[..., 0], -q[..., 1], -q[..., 2], q[..., 3]
    return np.stack([x, z, -y, w], -1)


def export_glb(ch, out, textures=None, all_lods=False, anims=()):
    subs = drawn(ch, None if all_lods else 0)
    pos, nrm = _yup(ch['pos']), _yup(ch['nrm'])
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-6)
    joints = np.zeros((len(pos), 4), np.uint16)
    joints[:, :3] = ch['joints']
    weights = np.zeros((len(pos), 4), np.float32)
    weights[:, :3] = ch['weights']

    blob = bytearray()
    views, accs = [], []

    def add(arr, target=None, comp=None, typ=None, minmax=False, normalized=False):
        arr = np.ascontiguousarray(arr)
        while len(blob) % 4:
            blob.append(0)
        views.append({'buffer': 0, 'byteOffset': len(blob), 'byteLength': arr.nbytes,
                      **({'target': target} if target else {})})
        blob.extend(arr.tobytes())
        a = {'bufferView': len(views) - 1, 'componentType': comp, 'count': len(arr), 'type': typ}
        if minmax:                              # always arrays, also for SCALAR accessors
            a['min'], a['max'] = np.atleast_1d(arr.min(0)).tolist(), np.atleast_1d(arr.max(0)).tolist()
        if normalized:
            a['normalized'] = True
        accs.append(a)
        return len(accs) - 1

    A_POS = add(pos.astype(np.float32), 34962, 5126, 'VEC3', True)
    A_NRM = add(nrm.astype(np.float32), 34962, 5126, 'VEC3')
    A_UV = add(ch['uv'].astype(np.float32), 34962, 5126, 'VEC2')
    A_J = add(joints, 34962, 5123, 'VEC4')
    A_W = add(weights, 34962, 5126, 'VEC4')

    # materials: one per diffuse texture (first texture that is not a reflection ramp)
    images, gl_tex, materials, mat_of = [], [], [], {}
    for k, s in subs:
        diffuse = next((t for t in s['textures'] if '_r.rm' not in t.lower()), s['textures'][0] if s['textures']
                       else None)
        key = diffuse or ''
        if key in mat_of:
            continue
        m = {'name': key or 'untextured', 'pbrMetallicRoughness': {'metallicFactor': 0.0, 'roughnessFactor': 0.9}}
        img = textures.get(key.lower()) if textures and key else None
        if img is not None:
            from PIL import Image
            png = io.BytesIO()
            Image.fromarray(img, 'RGBA').save(png, 'PNG')
            while len(blob) % 4:
                blob.append(0)
            views.append({'buffer': 0, 'byteOffset': len(blob), 'byteLength': png.tell()})
            blob.extend(png.getvalue())
            images.append({'name': key, 'mimeType': 'image/png', 'bufferView': len(views) - 1})
            gl_tex.append({'source': len(images) - 1})
            m['pbrMetallicRoughness']['baseColorTexture'] = {'index': len(gl_tex) - 1}
            # '.ra' textures carry a reflection mask in alpha (where the _R.rm ramp shines), not transparency
            if '.ra' not in key.lower() and img[..., 3].min() < 250:
                m['alphaMode'] = 'MASK'
        mat_of[key] = len(materials)
        materials.append(m)

    # winding: glTF front faces are counter-clockwise; pick the order that agrees with the vertex normals
    def oriented(tris):
        t = np.array(tris, np.uint32)
        if not len(t):
            return t
        p = pos[t]
        fn = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
        vn = nrm[t].sum(1)
        flip = (fn * vn).sum(1) < 0
        t[flip] = t[flip][:, [0, 2, 1]]
        return t

    prims = []
    for k, s in subs:
        t = oriented(s['tris'])
        if not len(t):
            continue
        diffuse = next((x for x in s['textures'] if '_r.rm' not in x.lower()), s['textures'][0] if s['textures']
                       else None)
        A_I = add(t.ravel(), 34963, 5125, 'SCALAR')
        prims.append({'attributes': {'POSITION': A_POS, 'NORMAL': A_NRM, 'TEXCOORD_0': A_UV, 'JOINTS_0': A_J,
                                     'WEIGHTS_0': A_W}, 'indices': A_I, 'material': mat_of[diffuse or ''],
                      'extras': {'submesh': k, 'sets': [n for i, n in enumerate(ch['sets']) if s['mask'] >> i & 1]}})

    # skeleton: one node per bone, local TRS; inverse bind matrices from the global bind pose (y-up)
    bones = ch['bones']
    C = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0], [0, 0, 0, 1]], np.float64)   # z-up -> y-up
    G = [C @ g @ np.linalg.inv(C) for g in bone_globals(bones)]
    ibm = np.array([np.linalg.inv(g).T.ravel() for g in G], np.float32)                     # column-major
    A_IBM = add(ibm, None, 5126, 'MAT4')
    nodes = []
    for i, b in enumerate(bones):
        nodes.append({'name': b['name'], 'translation': _yup(b['pos']).tolist(),
                      'rotation': _yup_quat(b['rot']).tolist(),
                      'children': [j for j, c in enumerate(bones) if c['parent'] == i]})
    # animations: {joint: {'pos': (times, xyz), 'rot': (times, xyzw)}} per clip (tools/xanim.py decode)
    node_of = {b['name']: i for i, b in enumerate(bones)}
    gl_anims = []
    for clip, tracks in anims:
        samplers, channels = [], []
        for joint, kinds in tracks.items():
            if joint not in node_of:
                continue
            for kind, (times, vals) in kinds.items():
                if len(times) < 1:
                    continue
                v = _yup(vals) if kind == 'pos' else _yup_quat(vals)
                ai = add(np.asarray(times, np.float32), None, 5126, 'SCALAR', True)
                ao = add(np.asarray(v, np.float32), None, 5126, 'VEC3' if kind == 'pos' else 'VEC4')
                samplers.append({'input': ai, 'output': ao, 'interpolation': 'LINEAR'})
                channels.append({'sampler': len(samplers) - 1,
                                 'target': {'node': node_of[joint],
                                            'path': 'translation' if kind == 'pos' else 'rotation'}})
        if channels:
            gl_anims.append({'name': clip, 'samplers': samplers, 'channels': channels})
    for n in nodes:
        if not n['children']:
            del n['children']
    mesh_node = len(nodes)
    nodes.append({'name': 'mesh', 'mesh': 0, 'skin': 0})
    roots = [i for i, b in enumerate(bones) if b['parent'] < 0] + [mesh_node]
    gltf = {'asset': {'version': '2.0', 'generator': 'GladiusPort tools/xchar.py'},
            'scene': 0, 'scenes': [{'nodes': roots}], 'nodes': nodes,
            'meshes': [{'name': 'character', 'primitives': prims}],
            'skins': [{'joints': list(range(len(bones))), 'inverseBindMatrices': A_IBM, 'skeleton': 0}],
            'materials': materials, 'accessors': accs, 'bufferViews': views,
            'buffers': [{'byteLength': len(blob)}]}
    if images:
        gltf['images'], gltf['textures'] = images, gl_tex
    if gl_anims:
        gltf['animations'] = gl_anims
    js = json.dumps(gltf, separators=(',', ':')).encode()
    js += b' ' * (-len(js) % 4)
    while len(blob) % 4:
        blob.append(0)
    with open(out, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob)))
        f.write(struct.pack('<II', len(js), 0x4E4F534A) + js)
        f.write(struct.pack('<II', len(blob), 0x004E4942) + bytes(blob))
    return len(prims), len(materials)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pax')
    ap.add_argument('out')
    ap.add_argument('--ptx', help='texture bundle (default: the .ptx beside the .pax)')
    ap.add_argument('--all-lods', action='store_true', help='also export the lower LOD sub-meshes')
    ap.add_argument('--anims', nargs='*', help='.pan files to add as clips (no files given: every .pan beside the .pax)')
    args = ap.parse_args()
    ch = read_character(open(args.pax, 'rb').read())
    ptx_path = args.ptx or os.path.splitext(args.pax)[0] + '.ptx'
    textures = read_ptx(open(ptx_path, 'rb').read()) if os.path.exists(ptx_path) else {}
    used = sorted({t.lower() for s in ch['submeshes'] for _, t in s['binds']})
    print(f'{args.pax}: {len(ch["bones"])} bones, {len(ch["pos"])} vertices, {len(ch["submeshes"])} sub-meshes, '
          f'{ch["conflicts"]} vertices shared between palettes')
    print(f'  textures used: {used}; in the .ptx: {sorted(textures)}')
    anims = []
    if args.anims is not None:
        import glob
        import xanim
        files = args.anims or sorted(glob.glob(os.path.join(os.path.dirname(args.pax) or '.', '*.pan')))
        stem = os.path.splitext(os.path.basename(args.pax))[0].lower()
        for f in files:
            name = os.path.splitext(os.path.basename(f))[0]
            name = name[len(stem) + 1:] if name.lower().startswith(stem + '_') else name
            try:
                anims.append((name, xanim.decode(xanim.read_pan(open(f, 'rb').read()))))
            except Exception as e:                      # noqa: BLE001 - report and keep going
                print(f'  skipped {f}: {e}')
    np_, nm = export_glb(ch, args.out, textures, args.all_lods, anims)
    print(f'wrote {args.out}: {np_} primitives, {nm} materials, {len(anims)} animations')


if __name__ == '__main__':
    main()

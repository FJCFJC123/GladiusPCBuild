"""GLB (glTF 2.0 binary) -> Gladius Xbox static prop .pax (tools/xrnd.py writer).

    python tools/glb2pax.py model.glb out.pax --template <retail prop .pax> [options]

The whole scene is baked into one rigid model: node transforms applied, glTF's Y-up turned into the game's
Z-up, one sub-mesh per material. Texture names come from the material (or --texture forces an existing game
texture); otherwise each material's diffuse image (or a flat colour) goes into <out>.ptx (tools/ptx.py).

Options:
  --texture NAME      use this texture for every material (e.g. an existing game texture, no .tga)
  --scale S           multiply positions (glTF metres; game units are about metres too)
  --fit SIZE          scale so the largest extent is SIZE
  --ground            move the model so it rests on z = 0 (props are placed by their origin)
  --center            centre the model on x = y = 0
  --flip-v            v = 1 - v
  --color R,G,B       vertex colour for models without COLOR_0 (0..1); usually left out (scene lighting looks right)
  --max-texture N     largest texture side, power of two (default 1024)
"""
import argparse
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xrnd  # noqa: E402

COMP = {5120: ('b', 1), 5121: ('B', 1), 5122: ('h', 2), 5123: ('H', 2), 5125: ('I', 4), 5126: ('f', 4)}
NCOMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}
NORM_DIV = {'b': 127.0, 'B': 255.0, 'h': 32767.0, 'H': 65535.0}


# ------------------------------------------------------------------------------------------------ glTF reading
def read_glb(path):
    d = open(path, 'rb').read()
    magic, ver, length = struct.unpack_from('<4sII', d, 0)
    if magic != b'glTF' or ver != 2:
        raise ValueError('not a glTF 2.0 binary')
    o, gltf, blob = 12, None, b''
    while o < length:
        clen, ctype = struct.unpack_from('<I4s', d, o)
        body = d[o + 8:o + 8 + clen]
        if ctype == b'JSON':
            gltf = json.loads(body)
        elif ctype == b'BIN\0':
            blob = body
        o += 8 + clen
    return gltf, blob


def accessor(gltf, blob, idx):
    a = gltf['accessors'][idx]
    if 'sparse' in a:
        raise ValueError('sparse accessors are not supported')
    fmt, size = COMP[a['componentType']]
    n = NCOMP[a['type']]
    count = a['count']
    if 'bufferView' not in a:
        return [tuple([0] * n)] * count
    bv = gltf['bufferViews'][a['bufferView']]
    base = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    stride = bv.get('byteStride', size * n)
    out = []
    for i in range(count):
        v = struct.unpack_from('<' + fmt * n, blob, base + i * stride)
        if a.get('normalized') and fmt in NORM_DIV:
            v = tuple(max(-1.0, x / NORM_DIV[fmt]) for x in v)
        out.append(v)
    return out


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def node_matrix(node):
    if 'matrix' in node:
        m = node['matrix']                                       # column-major
        return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
    tx, ty, tz = node.get('translation', (0, 0, 0))
    qx, qy, qz, qw = node.get('rotation', (0, 0, 0, 1))
    sx, sy, sz = node.get('scale', (1, 1, 1))
    r = [[1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)],
         [2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)],
         [2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)]]
    return [[r[0][0] * sx, r[0][1] * sy, r[0][2] * sz, tx],
            [r[1][0] * sx, r[1][1] * sy, r[1][2] * sz, ty],
            [r[2][0] * sx, r[2][1] * sy, r[2][2] * sz, tz],
            [0, 0, 0, 1]]


def walk(gltf, idx, parent, out, world=None):
    node = gltf['nodes'][idx]
    m = mat_mul(parent, node_matrix(node))
    if world is not None:
        world[idx] = m
    if 'mesh' in node:
        # a skinned mesh's vertices are in the bind pose: its node transform does not apply (with --pose-skins each part is
        # placed by its joints)
        out.append((node['mesh'], m if 'skin' not in node else [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]],
                    node.get('skin')))
    for c in node.get('children', []):
        walk(gltf, c, m, out, world)


def skin_matrices(gltf, blob, skin, world):
    """joint world matrix x inverse bind matrix for each joint of a skin (row-major 4x4)"""
    sk = gltf['skins'][skin]
    n = len(sk['joints'])
    ibm = accessor(gltf, blob, sk['inverseBindMatrices']) if 'inverseBindMatrices' in sk else None
    out = []
    for k, j in enumerate(sk['joints']):
        b = [[ibm[k][c * 4 + r] for c in range(4)] for r in range(4)] if ibm else             [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]  # glTF matrices are column-major
        out.append(mat_mul(world.get(j, [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]), b))
    return out


def xform(m, p, w):
    return tuple(m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3] * w for i in range(3))


def normalize(v):
    l = math.sqrt(sum(c * c for c in v)) or 1.0
    return tuple(c / l for c in v)


def y_up_to_z_up(v):
    return (v[0], -v[2], v[1])                                   # rotate +90 degrees about X (keeps handedness)


def safe_name(s, fallback):
    s = ''.join(c if c.isalnum() or c in '_-' else '_' for c in (s or '')).strip('_')
    return (s or fallback)[:31]


# ------------------------------------------------------------------------------------------------ conversion
def load_meshes(path, args):
    gltf, blob = read_glb(path)
    scene = gltf['scenes'][gltf.get('scene', 0)] if gltf.get('scenes') else {'nodes': list(range(len(gltf['nodes'])))}
    inst = []
    ident = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
    world = {}
    for n in scene['nodes']:
        walk(gltf, n, ident, inst, world)
    pose = getattr(args, 'pose_skins', False)
    skin_cache = {}
    groups = {}                                                  # material index -> {'verts': [], 'tris': []}
    mats = gltf.get('materials', [])
    for mesh_idx, m, skin in inst:
        SM = None
        if pose and skin is not None:
            if skin not in skin_cache:
                skin_cache[skin] = skin_matrices(gltf, blob, skin, world)
            SM = skin_cache[skin]
        for prim in gltf['meshes'][mesh_idx]['primitives']:
            if prim.get('mode', 4) != 4:
                print(f'  skipped a primitive with mode {prim.get("mode")} (only triangle lists)')
                continue
            a = prim['attributes']
            pos = accessor(gltf, blob, a['POSITION'])
            nrm = accessor(gltf, blob, a['NORMAL']) if 'NORMAL' in a else None
            uv = accessor(gltf, blob, a['TEXCOORD_0']) if 'TEXCOORD_0' in a else [(0.0, 0.0)] * len(pos)
            col = accessor(gltf, blob, a['COLOR_0']) if 'COLOR_0' in a else None
            J = accessor(gltf, blob, a['JOINTS_0']) if SM and 'JOINTS_0' in a else None
            W = accessor(gltf, blob, a['WEIGHTS_0']) if J and 'WEIGHTS_0' in a else None
            idx = [i[0] for i in accessor(gltf, blob, prim['indices'])] if 'indices' in prim else list(range(len(pos)))
            g = groups.setdefault(prim.get('material', -1), {'verts': [], 'tris': [], 'seen': {}})
            nm = [[m[i][j] for j in range(3)] for i in range(3)]  # normal matrix (no non-uniform scale assumed)
            base = len(g['verts'])
            local = []
            for i, p in enumerate(pos):
                if J:                                    # linear blend skinning in the rest pose of the joints
                    M = [[0.0] * 4 for _ in range(4)]
                    ws = W[i] if W else (1.0, 0, 0, 0)
                    tw = sum(ws) or 1.0
                    for jn, w in zip(J[i], ws):
                        if w:
                            S = SM[int(jn)]
                            for r in range(4):
                                for c in range(4):
                                    M[r][c] += S[r][c] * w / tw
                    mm = M
                else:
                    mm = m
                P = y_up_to_z_up(xform(mm, p, 1.0))
                nmm = [[mm[r][c] for c in range(3)] for r in range(3)] if J else nm
                N = y_up_to_z_up(normalize(tuple(sum(nmm[r][k] * nrm[i][k] for k in range(3)) for r in range(3)))) if nrm else None
                u, v = uv[i][0], uv[i][1]
                if args.flip_v:
                    v = 1.0 - v
                c = None
                if col:
                    c = tuple(col[i]) + ((1.0,) if len(col[i]) == 3 else ())
                local.append([P, N, (u, v), c])
            for t in range(0, len(idx) - 2, 3):
                a_, b_, c_ = idx[t], idx[t + 1], idx[t + 2]
                g['tris'].append((base + a_, base + b_, base + c_))
            g['verts'] += local
    # normals where the GLB has none: area-weighted face normals
    for g in groups.values():
        if any(v[1] is None for v in g['verts']):
            acc = [[0.0, 0.0, 0.0] for _ in g['verts']]
            for a_, b_, c_ in g['tris']:
                pa, pb, pc = g['verts'][a_][0], g['verts'][b_][0], g['verts'][c_][0]
                e1 = [pb[i] - pa[i] for i in range(3)]
                e2 = [pc[i] - pa[i] for i in range(3)]
                cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
                for k in (a_, b_, c_):
                    for i in range(3):
                        acc[k][i] += cr[i]
            for v, n in zip(g['verts'], acc):
                if v[1] is None:
                    v[1] = normalize(n)
    # placement: scale / fit / ground / centre
    allp = [v[0] for g in groups.values() for v in g['verts']]
    if not allp:
        raise ValueError('no triangle geometry in the GLB')
    lo = [min(p[i] for p in allp) for i in range(3)]
    hi = [max(p[i] for p in allp) for i in range(3)]
    s = args.scale
    if args.fit:
        s *= args.fit / max(hi[i] - lo[i] for i in range(3))
    off = [0.0, 0.0, 0.0]
    if args.center:
        off[0], off[1] = -(lo[0] + hi[0]) / 2 * s, -(lo[1] + hi[1]) / 2 * s
    if args.ground:
        off[2] = -lo[2] * s
    default_col = tuple(args.color) + (1.0,) if args.color else None
    meshes = []
    for mi, g in groups.items():
        mat = mats[mi] if 0 <= mi < len(mats) else {}
        # own textures get the model name as a prefix (texture names are global in the game); names are cut to 31
        # characters by shortening the prefix, and clashes get a number
        own = safe_name(str(mat.get('name') or texture_name(gltf, mat) or max(mi, 0)), str(max(mi, 0)))
        prefix = safe_name(args.model_name, 'model')[:max(4, 30 - len(own))]
        tex = args.texture or safe_name(f'{prefix}_{own}', f'{prefix}_{max(mi, 0)}')
        if not args.texture:
            used = {m['texture'] for m in meshes}
            k = 1
            while tex in used:
                tex = safe_name(f'{prefix[:24]}_{own[:4]}{k}', f'{prefix}_{k}')
                k += 1
        verts, remap = [], {}
        for v in g['verts']:
            P = tuple(v[0][i] * s + off[i] for i in range(3))
            c = v[3] or default_col
            key = (P, v[1], v[2], c)
            if key not in remap:                                 # merge identical vertices
                remap[key] = len(verts)
                verts.append((P, v[1], v[2]) + ((c,) if c else ()))
            v.append(remap[key])
        tris = [(g['verts'][a_][4], g['verts'][b_][4], g['verts'][c_][4]) for a_, b_, c_ in g['tris']]
        meshes.append({'texture': tex, 'verts': verts, 'tris': tris, 'material': mi})
    return gltf, blob, meshes, (s, lo, hi)


def diffuse_ref(mat):
    """(texture ref, colour factor) - metallic-roughness baseColor, or the spec-gloss extension's diffuse"""
    pbr = mat.get('pbrMetallicRoughness', {})
    sg = mat.get('extensions', {}).get('KHR_materials_pbrSpecularGlossiness', {})
    if 'baseColorTexture' in pbr or 'baseColorFactor' in pbr:
        return pbr.get('baseColorTexture'), pbr.get('baseColorFactor', [1, 1, 1, 1])
    if sg:
        return sg.get('diffuseTexture'), sg.get('diffuseFactor', [1, 1, 1, 1])
    return None, [1, 1, 1, 1]


def texture_name(gltf, mat):
    t, _ = diffuse_ref(mat)
    if not t:
        return None
    src = gltf['textures'][t['index']].get('source')
    if src is None:
        return None
    im = gltf['images'][src]
    return im.get('name') or os.path.splitext(os.path.basename(im.get('uri', '')))[0] or f'image{src}'


def image_bytes(gltf, blob, glb_path, t):
    im = gltf['images'][gltf['textures'][t['index']]['source']]
    if 'bufferView' in im:
        bv = gltf['bufferViews'][im['bufferView']]
        return blob[bv.get('byteOffset', 0):bv.get('byteOffset', 0) + bv['byteLength']]
    if 'uri' in im and not im['uri'].startswith('data:'):
        return open(os.path.join(os.path.dirname(glb_path), im['uri']), 'rb').read()
    return None


def build_textures(gltf, blob, glb_path, meshes, max_size):
    """[(name, RGBA array)] for the .ptx: each material's diffuse image (times its colour factor), or a flat
    colour when it has none"""
    import io
    import numpy as np
    from PIL import Image
    mats = gltf.get('materials', [])
    out, done = [], set()
    for m in meshes:
        if m['texture'] in done:
            continue
        done.add(m['texture'])
        mat = mats[m['material']] if 0 <= m['material'] < len(mats) else {}
        t, factor = diffuse_ref(mat)
        data = image_bytes(gltf, blob, glb_path, t) if t else None
        if data:
            im = Image.open(io.BytesIO(data)).convert('RGBA')
            w, h = im.size
            p2 = lambda v: 1 << max(2, min(max_size.bit_length() - 1, round(math.log2(max(v, 4)))))
            if (p2(w), p2(h)) != (w, h):
                im = im.resize((p2(w), p2(h)), Image.LANCZOS)
            arr = np.array(im, dtype=np.float32)
        else:
            arr = np.full((4, 4, 4), 255.0, np.float32)
        arr *= np.array(factor, np.float32)
        out.append((m['texture'], np.clip(np.round(arr), 0, 255).astype(np.uint8)))
        print(f'  texture {m["texture"]}: {arr.shape[1]}x{arr.shape[0]}' + ('' if data else ' (flat colour)'))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('glb')
    ap.add_argument('out')
    ap.add_argument('--template', required=True, help='retail stride-24 prop .pax (e.g. mesh/misc/rocks/rock_gen_01.pax)')
    ap.add_argument('--name', help='model name (default: output file name)')
    ap.add_argument('--texture')
    ap.add_argument('--scale', type=float, default=1.0)
    ap.add_argument('--fit', type=float)
    ap.add_argument('--ground', action='store_true')
    ap.add_argument('--center', action='store_true')
    ap.add_argument('--flip-v', action='store_true')
    ap.add_argument('--color', type=lambda s: [float(x) for x in s.split(',')])
    ap.add_argument('--max-texture', type=int, default=1024, help='largest texture side (power of two)')
    args = ap.parse_args()
    name = args.name or os.path.splitext(os.path.basename(args.out))[0]
    args.model_name = name
    gltf, blob, meshes, (s, lo, hi) = load_meshes(args.glb, args)
    nv = sum(len(m['verts']) for m in meshes)
    nt = sum(len(m['tris']) for m in meshes)
    print(f'{args.glb}: {len(meshes)} material(s), {nv} vertices, {nt} triangles, scale {s:.4g}, '
          f'size {[round((hi[i] - lo[i]) * s, 3) for i in range(3)]} (x, y, z-up)')
    for m in meshes:
        print(f'  {m["texture"]}: {len(m["verts"])} vertices, {len(m["tris"])} triangles')
    pax = xrnd.build_static_pax(open(args.template, 'rb').read(), meshes, name)
    open(args.out, 'wb').write(pax)
    print(f'wrote {args.out} ({len(pax)} bytes)')
    if not args.texture:                                         # own textures -> <out>.ptx (same base name)
        import ptx
        tex = build_textures(gltf, blob, args.glb, meshes, args.max_texture)
        ptx_path = os.path.splitext(args.out)[0] + '.ptx'
        data = ptx.build_ptx(tex)
        open(ptx_path, 'wb').write(data)
        print(f'wrote {ptx_path} ({len(data)} bytes)')


if __name__ == '__main__':
    main()

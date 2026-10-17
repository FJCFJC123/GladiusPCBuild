"""Merge a GLB part into an existing Gladius Xbox static prop (.pax + .ptx), keeping everything the prop has.

    python tools/paxmerge.py prop.pax part.glb [--out out.pax] [--at X Y Z] [--yaw DEG] [--scale S | --fit SIZE]
                             [--ground] [--center] [--flip-v] [--max-texture N]

The prop's XRND blocks stay as they are; the part is appended (tools/xrnd.py: every in-block pointer is relative to
the block it points into, so appending entries never moves existing data):
  - one new vertex buffer entry + D3D header (the prop's own stride: 24, or 28 with vertex colours - white unless the
    GLB has COLOR_0), its vertices at the end of the vertex block, one strip per material at the end of the index block
  - per GLB material: a sub-mesh record (copied from the prop's first one: sphere, entries, name), a material + its
    programs + command streams copied from the prop's first sub-mesh's material (same render states) with the
    texture bind (op 0x01 sub-op 0, +8) pointed at a new texture object, and the texture's name
  - list A (opaque) grows; list B (the transparent pass, drawn after A) keeps its entries after A's in the pointer
    blocks, its geometry offsets (+0x2C / +0x30) move with them
  - TXTR gets an entry per new texture, SELS the new sub-mesh names (before the texture names) and texture names
  - the .ptx keeps its entries byte for byte; the GLB's textures are encoded and appended (tools/ptx.py)
Positions: GLB metres, glTF Y-up -> Z-up (tools/glb2pax.py), then scale, yaw about z, and --at in the prop's own
model space (the editor passes the selection's centre).
"""
import argparse
import math
import os
import struct
import sys
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glb2pax  # noqa: E402
import ptx  # noqa: E402
import xrnd  # noqa: E402
from xrnd import (SLOT_GEOM, SLOT_MESHPTRS, SLOT_SUBMESH, SLOT_IDXENT, SLOT_VBENT, SLOT_VBHDR, SLOT_MATPTRS,  # noqa: E402
                  SLOT_MATERIALS, SLOT_PROGRAMS, SLOT_CMD, SLOT_TEXTURES, SLOT_NAMES, SLOT_INDICES, SLOT_VERTICES)

U32 = struct.Struct('<I')


def u32(b, o):
    return U32.unpack_from(b, o)[0]


def cmd_stream(cmds, off):
    """bytes of one command stream starting at off (through its op 0x00 terminator)"""
    i = off
    while i + 2 <= len(cmds) and cmds[i]:
        ln = cmds[i + 1]
        i += ln if ln >= 4 else 4
    return bytes(cmds[off:i + 4])


def obbt_add(payload, map_bytes, cnt_a_old, n_new, pts):
    """OBBT (levels): map = int16 entries (count field = its length in bytes; -n = node n, >= 0 = sub-mesh in list A
    then B order), then 0x40-byte nodes (3 rows axis xyz + half extent, centre, u16 first, u16 count). The new
    sub-meshes go into list A after its old entries, so list B's indices move up by n_new; the root node's entries are
    copied to the map's end with the new ones after them (its range must stay contiguous) and its box grows over the
    new geometry. -> (count, payload)"""
    nmap = map_bytes // 2
    m = list(struct.unpack_from(f'<{nmap}h', payload, 0))
    nodes = bytearray(payload[map_bytes:])
    m = [e + n_new if e >= cnt_a_old else e for e in m]
    first, count = struct.unpack_from('<HH', nodes, 0x3C)
    root = m[first:first + count]
    new_first = len(m)
    m += root + list(range(cnt_a_old, cnt_a_old + n_new))
    if len(m) > 0xFFFF or len(m) > 0x7FFF * 2:
        raise ValueError('OBBT map too long')
    struct.pack_into('<HH', nodes, 0x3C, new_first, len(root) + n_new)
    f = list(struct.unpack_from('<15f', nodes, 0))
    for k in range(3):                                   # grow the root box along its own axes
        ax, half = f[4 * k:4 * k + 3], f[4 * k + 3]
        c = sum(f[12 + i] * ax[i] for i in range(3))
        lo, hi = c - half, c + half
        for p in pts:
            d = sum(p[i] * ax[i] for i in range(3))
            lo, hi = min(lo, d), max(hi, d)
        f[4 * k + 3] = (hi - lo) / 2 + 0.01
        mid_shift = (lo + hi) / 2 - c
        for i in range(3):
            f[12 + i] += ax[i] * mid_shift
    struct.pack_into('<15f', nodes, 0, *f)
    mp = struct.pack(f'<{len(m)}h', *m)
    return len(mp), mp + bytes(nodes)


# ------------------------------------------------------------------------------------------------ clear box
def _unpack_n(p):
    x, y, z = p & 0x7FF, (p >> 11) & 0x7FF, (p >> 22) & 0x3FF
    x, y, z = x - 2048 if x >= 1024 else x, y - 2048 if y >= 1024 else y, z - 1024 if z >= 512 else z
    return x / 1023.0, y / 1023.0, z / 511.0


def _lerp_vertex(va, vb, t, stride):
    """two packed vertices -> the one at t along a -> b: position, normal (renormalised), colour per channel, UVs"""
    a, b = bytes(va), bytes(vb)
    pa, pb = struct.unpack_from('<3f', a, 0), struct.unpack_from('<3f', b, 0)
    out = bytearray(a)
    struct.pack_into('<3f', out, 0, *(pa[i] + (pb[i] - pa[i]) * t for i in range(3)))
    na, nb = _unpack_n(struct.unpack_from('<I', a, 12)[0]), _unpack_n(struct.unpack_from('<I', b, 12)[0])
    n = [na[i] + (nb[i] - na[i]) * t for i in range(3)]
    ln = math.sqrt(sum(x * x for x in n)) or 1.0
    struct.pack_into('<I', out, 12, xrnd.pack_normal(*(x / ln for x in n)))
    uv = 16
    if stride >= 28:
        ca, cb = a[16:20], b[16:20]
        out[16:20] = bytes(round(ca[i] + (cb[i] - ca[i]) * t) for i in range(4))
        uv = 20
    for o in range(uv, stride, 4):                       # every float after: UV0, UV1
        fa, fb = struct.unpack_from('<f', a, o)[0], struct.unpack_from('<f', b, o)[0]
        struct.pack_into('<f', out, o, fa + (fb - fa) * t)
    return bytes(out)


def footprint(meshes, box, cells=96, sub=4):
    """The new part's real footprint in the box's frame (x, y along the box's first two axes, z along the third): a
    grid of about `cells` across the longer side, each cell sampled at sub x sub points. A cell counts as covered only
    when EVERY sample lies under one of the part's triangles (projected) - partly covered cells along the outline keep
    their ground, so the trim never opens a gap the part does not fill; the part's rim overlaps the old ground there.
    Covered cells keep the lowest / highest part height over their samples. -> grid dict"""
    c, A, h = box[0:3], [box[3:6], box[6:9], box[9:12]], box[12:15]
    cs = max(2 * h[0], 2 * h[1]) / cells
    nx, ny = max(1, math.ceil(2 * h[0] / cs)), max(1, math.ceil(2 * h[1] / cs))
    sx, sy = nx * sub, ny * sub                          # sample grid
    ss = cs / sub
    szlo = [1e30] * (sx * sy)
    szhi = [-1e30] * (sx * sy)

    def loc(p):
        d = [p[i] - c[i] for i in range(3)]
        return [sum(d[i] * A[k][i] for i in range(3)) for k in range(3)]

    for m in meshes:
        L = [loc(v[0]) for v in m['verts']]
        for t in m['tris']:
            P = [L[i] for i in t]
            xs = [(p[0] + h[0]) / ss for p in P]         # in sample units
            ys = [(p[1] + h[1]) / ss for p in P]
            d = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if abs(d) < 1e-12:
                continue
            x0, x1 = max(0, int(math.floor(min(xs) - 0.5))), min(sx - 1, int(math.ceil(max(xs) - 0.5)))
            y0, y1 = max(0, int(math.floor(min(ys) - 0.5))), min(sy - 1, int(math.ceil(max(ys) - 0.5)))
            for iy in range(y0, y1 + 1):                 # sample points (centres) inside the triangle
                py = iy + 0.5
                for ix in range(x0, x1 + 1):
                    px = ix + 0.5
                    l0 = ((ys[1] - ys[2]) * (px - xs[2]) + (xs[2] - xs[1]) * (py - ys[2])) / d
                    l1 = ((ys[2] - ys[0]) * (px - xs[2]) + (xs[0] - xs[2]) * (py - ys[2])) / d
                    if l0 < -1e-6 or l1 < -1e-6 or l0 + l1 > 1 + 1e-6:
                        continue
                    z = P[0][2] * l0 + P[1][2] * l1 + P[2][2] * (1 - l0 - l1)
                    q = iy * sx + ix
                    if z < szlo[q]:
                        szlo[q] = z
                    if z > szhi[q]:
                        szhi[q] = z
    zlo, zhi = {}, {}
    for cy in range(ny):                                 # fully covered cells only
        for cx in range(nx):
            lo_, hi_, full = 1e30, -1e30, True
            for iy in range(cy * sub, cy * sub + sub):
                for ix in range(cx * sub, cx * sub + sub):
                    q = iy * sx + ix
                    if szlo[q] > szhi[q]:
                        full = False
                        break
                    lo_, hi_ = min(lo_, szlo[q]), max(hi_, szhi[q])
                if not full:
                    break
            if full:
                zlo[(cx, cy)], zhi[(cx, cy)] = lo_, hi_
    pad = max(0.3, 0.05 * 2 * h[2])
    if os.environ.get('PAXMERGE_DEBUG'):
        Ls = [loc(v[0]) for m in meshes for v in m['verts']]
        cov = sum(1 for q in range(sx * sy) if szlo[q] <= szhi[q])
        print(f'  footprint: part local x {min(p[0] for p in Ls):.2f}..{max(p[0] for p in Ls):.2f} '
              f'y {min(p[1] for p in Ls):.2f}..{max(p[1] for p in Ls):.2f} z {min(p[2] for p in Ls):.2f}..'
              f'{max(p[2] for p in Ls):.2f}, half {h}, samples covered {cov}/{sx * sy}, cells full {len(zlo)}/{nx * ny}')
    return {'c': c, 'A': A, 'h': h, 'cs': cs, 'nx': nx, 'ny': ny, 'zlo': zlo, 'zhi': zhi, 'pad': pad, 'loc': loc}


def clear_box(bl, hdr, box, meshes=None):
    """Existing faces against the new part (box = its oriented box: centre c, unit axes a0..a2, half extents h).
    With the part's meshes, the clear follows its real footprint: faces in the box are cut along the footprint grid's
    cell lines (new vertices interpolated: position, normal, colour, UVs) and a piece goes only where the part covers
    its cell and its height lies within the part's there (+ a margin) - open ground between a ruin's walls stays.
    Without meshes: everything inside the box goes, crossing faces cut at the box. New vertices go into the face's own
    vertex buffer (a full buffer's sub-mesh moves to one of its own first); the vertex block is rebuilt and every
    changed sub-mesh gets a new strip at the end of the index block. -> (pieces removed, faces cut, vertices added)"""
    c, A, h = box[0:3], [box[3:6], box[6:9], box[9:12]], box[12:15]
    planes = []                                          # (normal, offset): outside when dot(n, p) - d > 0
    for i in range(3):
        planes.append((A[i], sum(A[i][k] * c[k] for k in range(3)) + h[i]))
        planes.append(([-x for x in A[i]], -sum(A[i][k] * c[k] for k in range(3)) + h[i]))
    fp = footprint(meshes, box) if meshes else None
    n_sub, n_vb, n_ie = hdr[6], hdr[7], hdr[8]
    ents, hdrs, vtx = bl[SLOT_VBENT], bl[SLOT_VBHDR], bytes(bl[SLOT_VERTICES])
    vbs = []                                             # per vertex buffer entry: [stride, bytearray of its vertices]
    for e in range(n_vb):
        ho, cnt, st = u32(ents, e * 0x14), u32(ents, e * 0x14 + 4), u32(ents, e * 0x14 + 0x10)
        d = u32(hdrs, ho + 4)
        vbs.append([st, bytearray(vtx[d:d + cnt * st])])
    removed = cut = added = over = 0

    def pos(vb, i):
        return struct.unpack_from('<3f', vbs[vb][1], i * vbs[vb][0])

    def split(poly, n, d, vert, st):
        """convex polygon -> (part with dot(n, p) - d > 0, the rest); cut points interpolated"""
        pp = lambda q: struct.unpack_from('<3f', vert(q), 0)
        hi_, lo_ = [], []
        for a_, b_ in zip(poly, poly[1:] + poly[:1]):
            da = sum(n[q] * pp(a_)[q] for q in range(3)) - d
            db = sum(n[q] * pp(b_)[q] for q in range(3)) - d
            (hi_ if da > 0 else lo_).append(a_)
            if (da > 0) != (db > 0):
                m = (None, _lerp_vertex(vert(a_), vert(b_), da / (da - db), st))
                hi_.append(m)
                lo_.append(m)
        return hi_, lo_

    def drop_piece(poly, vert):                          # footprint mode: the part covers this piece?
        P = [struct.unpack_from('<3f', vert(q), 0) for q in poly]
        m = [sum(p[i] for p in P) / len(P) for i in range(3)]
        L = fp['loc'](m)
        ix = int(math.floor((L[0] + fp['h'][0]) / fp['cs']))
        iy = int(math.floor((L[1] + fp['h'][1]) / fp['cs']))
        k = (ix, iy)
        if k not in fp['zlo']:
            return False
        # a covered cell: anything at or above the part's lowest surface there would show through it; what lies below stays
        return L[2] >= fp['zlo'][k] - fp['pad']

    import paxsplit
    for k in range(n_sub):
        sm = bytes(bl[SLOT_SUBMESH][k * 0x38:k * 0x38 + 0x38])
        vbe, ie = u32(sm, 4), u32(sm, 8)
        vb = vbe // 0x14
        st = vbs[vb][0]
        _, io, _, ic, prim = struct.unpack_from('<fIIII', bl[SLOT_IDXENT], ie)
        if ic < 3:
            continue
        tris = paxsplit.strip_tris(list(struct.unpack_from(f'<{ic}H', bl[SLOT_INDICES], io)), prim)
        touched = []
        for t in tris:
            P = [pos(vb, i) for i in t]
            dist = [[sum(n[q] * p[q] for q in range(3)) - d for (n, d) in planes] for p in P]
            inbox = not any(all(dist[v][j] > 0 for v in range(3)) for j in range(6))
            if inbox and fp:                             # footprint mode: only faces over a covered cell can be cut
                L = [fp['loc'](p) for p in P]
                gx = [(p[0] + fp['h'][0]) / fp['cs'] for p in L]
                gy = [(p[1] + fp['h'][1]) / fp['cs'] for p in L]
                inbox = any((ix, iy) in fp['zlo'] and max(p[2] for p in L) >= fp['zlo'][(ix, iy)] - fp['pad']
                            for ix in range(int(math.floor(min(gx))), int(math.floor(max(gx))) + 1)
                            for iy in range(int(math.floor(min(gy))), int(math.floor(max(gy))) + 1))
            touched.append(inbox)
        if not any(touched):
            continue
        # a buffer without room for the cuts: the sub-mesh first moves into a vertex buffer of its own
        need = sum(touched) * (48 if fp else 8)
        if len(vbs[vb][1]) // st + need > 0xFFFF:
            used = sorted({i for t in tris for i in t})
            remap = {o: n_ for n_, o in enumerate(used)}
            data = bytearray(b''.join(bytes(vbs[vb][1][o * st:(o + 1) * st]) for o in used))
            new_e = len(vbs)
            ho = len(bl[SLOT_VBHDR])
            bl[SLOT_VBHDR] += bytes(bl[SLOT_VBHDR][u32(ents, vb * 0x14):u32(ents, vb * 0x14) + 0x0C])
            ne = bytearray(bl[SLOT_VBENT][vb * 0x14:vb * 0x14 + 0x14])
            struct.pack_into('<I', ne, 0, ho)
            bl[SLOT_VBENT] += ne
            ents = bl[SLOT_VBENT]
            vbs.append([st, data])
            hdr[7] += 1
            struct.pack_into('<I', bl[SLOT_SUBMESH], k * 0x38 + 4, new_e * 0x14)
            tris = [tuple(remap[i] for i in t) for t in tris]
            vb = new_e
            print(f'  clear: sub-mesh {k} moved to a vertex buffer of its own (its old one has no room for the cuts)')
        moved = vb != vbe // 0x14                        # a moved sub-mesh's remapped list is always written
        out, changed, cache = [], moved, {}
        vert = lambda q: bytes(vbs[vb][1][q[0] * st:(q[0] + 1) * st]) if q[0] is not None else q[1]
        for t, tch in zip(tris, touched):
            if not tch:
                out.append(t)
                continue
            poly = [(i, None) for i in t]
            kept = []
            if fp:                                       # pieces along the footprint grid
                L = [fp['loc'](pos(vb, i)) for i in t]
                gx = [(p[0] + fp['h'][0]) / fp['cs'] for p in L]
                gy = [(p[1] + fp['h'][1]) / fp['cs'] for p in L]
                cells = {(ix, iy) for ix in range(int(math.floor(min(gx))), int(math.floor(max(gx))) + 1)
                         for iy in range(int(math.floor(min(gy))), int(math.floor(max(gy))) + 1)}
                if not cells & set(fp['zlo']):           # nowhere near the part: as it is
                    out.append(t)
                    continue
                cols, rest = [], poly                    # split along x lines, then each column along y lines
                ax, ay = fp['A'][0], fp['A'][1]
                cx0 = sum(ax[q] * fp['c'][q] for q in range(3)) - fp['h'][0]
                cy0 = sum(ay[q] * fp['c'][q] for q in range(3)) - fp['h'][1]
                for ix in range(int(math.floor(min(gx))) + 1, int(math.floor(max(gx))) + 1):
                    right, left = split(rest, ax, cx0 + ix * fp['cs'], vert, st)
                    if len(left) >= 3:
                        cols.append(left)
                    rest = right
                    if len(rest) < 3:
                        break
                if len(rest) >= 3:
                    cols.append(rest)
                pieces = []
                for col in cols:
                    rest = col
                    for iy in range(int(math.floor(min(gy))) + 1, int(math.floor(max(gy))) + 1):
                        up, down = split(rest, ay, cy0 + iy * fp['cs'], vert, st)
                        if len(down) >= 3:
                            pieces.append(down)
                        rest = up
                        if len(rest) < 3:
                            break
                    if len(rest) >= 3:
                        pieces.append(rest)
                gone = [pc for pc in pieces if drop_piece(pc, vert)]
                if not gone:
                    out.append(t)
                    continue
                kept = [pc for pc in pieces if not drop_piece(pc, vert)]
                removed += len(gone)
            else:                                        # the whole box
                for (n, d) in planes:
                    if len(poly) < 3:
                        break
                    outside, inside = split(poly, n, d, vert, st)
                    if len(outside) >= 3:
                        kept.append(outside)
                    poly = inside
                if not kept:
                    removed += 1
            changed = True
            # vertex budget: shared cut points are stored once; a face that no longer fits is handled whole (dropped if the part
            # covers its centre, else kept)
            fresh = {q[1] for pl in kept for q in pl if q[0] is None and q[1] not in cache}
            if len(vbs[vb][1]) // st + len(fresh) > 0xFFFF:
                if fp and drop_piece([(i, None) for i in t], vert):
                    removed += 1
                else:
                    out.append(t)
                over += 1
                continue
            cut += 1
            for pl in kept:
                ids = []
                for q in pl:
                    if q[0] is None:
                        if q[1] not in cache:
                            cache[q[1]] = len(vbs[vb][1]) // st
                            vbs[vb][1] += q[1]
                            added += 1
                        ids.append(cache[q[1]])
                    else:
                        ids.append(q[0])
                for j in range(1, len(ids) - 1):         # fan (convex, winding kept)
                    out.append((ids[0], ids[j], ids[j + 1]))
        if not changed:
            continue
        if not out:                                      # everything gone: one degenerate triangle
            out_strip = [tris[0][0]] * 3 if tris else [0, 0, 0]
        else:
            out_strip, _ = xrnd.tris_to_strip(out)
        nio = len(bl[SLOT_INDICES]) + (-len(bl[SLOT_INDICES]) % 4)
        bl[SLOT_INDICES] += b'\0' * (nio - len(bl[SLOT_INDICES]))
        bl[SLOT_INDICES] += struct.pack(f'<{len(out_strip)}H', *out_strip)
        struct.pack_into('<fIIII', bl[SLOT_IDXENT], ie, 1.0, nio, xrnd.nondegenerate(out_strip), len(out_strip), 6)
    if not (removed or cut):
        return 0, 0, 0
    newv = bytearray()                                   # vertex block rebuilt, buffer after buffer
    for e, (st, data) in enumerate(vbs):
        newv += b'\0' * (-len(newv) % 4)
        struct.pack_into('<I', bl[SLOT_VBHDR], u32(ents, e * 0x14) + 4, len(newv))
        struct.pack_into('<I', bl[SLOT_VBENT], e * 0x14 + 4, len(data) // st)
        newv += data
    bl[SLOT_VERTICES] = newv
    if over:
        print(f'  clear: {over} faces handled whole (their vertex buffer is full)')
    return removed, cut, added


def merge(pax, ptx_data, meshes, textures, tag, box=None):
    """pax / ptx bytes + glb2pax meshes ({'texture', 'verts', 'tris'}) + [(name, RGBA)] -> (pax, ptx)"""
    chunks = xrnd.read_chunks(pax)
    xi = next(i for i, c in enumerate(chunks) if c[0] == b'XRND')
    xr_tag, xr_ver, xr_num, xr_pay = chunks[xi]
    reloc, hdr, bl = xrnd.parse_xrnd(xr_pay, xr_ver)
    order = sorted(bl, key=lambda s: hdr[s // 4])
    n_sub, n_vb, n_ie, n_tex, n_mat = hdr[6], hdr[7], hdr[8], hdr[9], hdr[10]
    # fixed-size blocks trimmed to their counts (a block may carry padding up to the next one)
    for s, size, n in ((SLOT_SUBMESH, 0x38, n_sub), (SLOT_IDXENT, 0x14, n_ie), (SLOT_VBENT, 0x14, n_vb),
                       (SLOT_VBHDR, 0x0C, n_vb), (SLOT_TEXTURES, 0x40, n_tex)):
        bl[s] = bytearray(bl[s][:size * n])
    for s in (SLOT_MATERIALS, SLOT_PROGRAMS, SLOT_CMD, SLOT_NAMES, SLOT_INDICES, SLOT_VERTICES):
        bl[s] = bytearray(bl[s])
    if box:                                              # the existing faces inside the part's box go first
        rem, cutn, addn = clear_box(bl, hdr, box, meshes)
        print(f'  clear (the part\'s footprint): {rem} pieces removed from {cutn} faces ({addn} vertices added)')
    geom = bytearray(bl[SLOT_GEOM])
    cnt_a, off_a, moff_a, cnt_b, off_b, moff_b = struct.unpack_from('<IIIIII', geom, 0x1C)
    cnt_b = cnt_b if cnt_b not in (0, 0xFFFFFFFF) and cnt_b < 0x10000 else 0
    meshptrs, matptrs = bytes(bl[SLOT_MESHPTRS]), bytes(bl[SLOT_MATPTRS])
    list_a = [u32(meshptrs, off_a + 4 * k) for k in range(cnt_a)]
    mats_a = [u32(matptrs, moff_a + 4 * k) for k in range(cnt_a)]
    list_b = [u32(meshptrs, off_b + 4 * k) for k in range(cnt_b)]
    mats_b = [u32(matptrs, moff_b + 4 * k) for k in range(cnt_b)]
    first_sub, first_mat = (list_a or list_b)[0], (mats_a or mats_b)[0]

    # the new vertex buffer takes the layout of the buffer the copied material draws: 24 position, normal, UV / 28 +
    # colour / 36 + colour + second UV
    t_vbe = u32(bl[SLOT_SUBMESH], first_sub + 4)
    vbent0 = bl[SLOT_VBENT][t_vbe:t_vbe + 0x14]
    stride = u32(vbent0, 0x10)
    if stride not in (24, 28, 36):
        raise ValueError(f'vertex stride {stride}: only static models (24 / 28 / 36) can be merged into')
    default_col = 0xFFFFFFFF                             # colour for GLB vertices without one: the buffer's average
    if stride >= 28:                                     # (levels bake their lighting into vertex colours)
        hd = u32(bl[SLOT_VBHDR], u32(vbent0, 0) + 4)
        cnt0 = u32(vbent0, 4)
        acc, n = [0, 0, 0, 0], 0
        for i in range(0, cnt0, max(1, cnt0 // 4000)):
            c = u32(bl[SLOT_VERTICES], hd + i * stride + 16)
            for k in range(4):
                acc[k] += (c >> (8 * k)) & 0xFF
            n += 1
        if n:
            default_col = sum((round(acc[k] / n) & 0xFF) << (8 * k) for k in range(4))
    t_sub = bytearray(bl[SLOT_SUBMESH][first_sub:first_sub + 0x38])
    nprog = u32(bl[SLOT_MATERIALS], first_mat + 4)
    t_progs = [u32(bl[SLOT_MATERIALS], first_mat + 8 + 4 * j) for j in range(nprog)]

    def add_name(s):
        off = len(bl[SLOT_NAMES])
        bl[SLOT_NAMES] += s.encode('latin1') + b'\0'
        return off

    # textures: new texture objects (copied from the first) named after the new textures
    tex_off = {}
    for name, _ in textures:                             # +0x10 = name offset (no extension), rest as the first
        to = bytearray(bl[SLOT_TEXTURES][:0x40] or bytes(0x40))
        struct.pack_into('<I', to, 0x10, add_name(name))
        tex_off[name] = len(bl[SLOT_TEXTURES])
        bl[SLOT_TEXTURES] += to

    # pieces: each material's triangles cut so a piece uses at most MAX_VB vertices (16-bit indices), packed into vertex
    # buffers of at most MAX_VB - one sub-mesh per piece
    MAX_VB = 0xFFFF
    pieces = []                                          # (mesh, verts, tris local to verts)
    for m in meshes:
        remap, verts, tris = {}, [], []
        for t in m['tris']:
            need = sum(1 for i in t if i not in remap)
            if len(verts) + need > MAX_VB:
                pieces.append((m, verts, tris))
                remap, verts, tris = {}, [], []
            loc = []
            for i in t:
                if i not in remap:
                    remap[i] = len(verts)
                    verts.append(m['verts'][i])
                loc.append(remap[i])
            tris.append(tuple(loc))
        if tris:
            pieces.append((m, verts, tris))

    def pack_vertex(vt):
        (x, y, z), (nx, ny, nz), (u, v) = vt[:3]
        if stride == 36:                                 # second UV = the first
            col = xrnd.pack_color(*vt[3]) if len(vt) > 3 else default_col
            return struct.pack('<fffIIffff', x, y, z, xrnd.pack_normal(nx, ny, nz), col, u, v, u, v)
        if stride == 28:
            col = xrnd.pack_color(*vt[3]) if len(vt) > 3 else default_col
            return struct.pack('<fffIIff', x, y, z, xrnd.pack_normal(nx, ny, nz), col, u, v)
        return struct.pack('<fffIff', x, y, z, xrnd.pack_normal(nx, ny, nz), u, v)

    vbs = []                                             # [vertex entry offset, vertex count]
    placed = []                                          # per piece: (vertex entry offset, base vertex)

    def open_vb():
        vb_data = len(bl[SLOT_VERTICES])
        vb_data += -vb_data % 4
        bl[SLOT_VERTICES] += b'\0' * (vb_data - len(bl[SLOT_VERTICES]))
        hdr_off = len(bl[SLOT_VBHDR])
        vh = bytearray(bl[SLOT_VBHDR][:0x0C])
        struct.pack_into('<II', vh, 0, 0xFFFFFFFF, vb_data)   # Common -1: the engine makes the D3D buffer
        bl[SLOT_VBHDR] += vh
        vbe_off = len(bl[SLOT_VBENT])
        ve = bytearray(vbent0)
        struct.pack_into('<II', ve, 0, hdr_off, 0)
        bl[SLOT_VBENT] += ve
        vbs.append([vbe_off, 0])

    for m, verts, tris in pieces:
        if not vbs or vbs[-1][1] + len(verts) > MAX_VB:
            open_vb()
        placed.append((vbs[-1][0], vbs[-1][1]))
        bl[SLOT_VERTICES] += b''.join(pack_vertex(vt) for vt in verts)
        vbs[-1][1] += len(verts)
        struct.pack_into('<I', bl[SLOT_VBENT], vbs[-1][0] + 4, vbs[-1][1])

    # per piece: indices, index entry, sub-mesh, material, programs, command streams
    new_subs, new_mats, sub_names = [], [], []
    for k, ((m, verts, tris), (vbe_off, vbase)) in enumerate(zip(pieces, placed)):
        strip, _ = xrnd.tris_to_strip([(a + vbase, b + vbase, c + vbase) for a, b, c in tris])
        io = len(bl[SLOT_INDICES])
        io += -io % 4
        bl[SLOT_INDICES] += b'\0' * (io - len(bl[SLOT_INDICES]))
        bl[SLOT_INDICES] += struct.pack(f'<{len(strip)}H', *strip)
        ie_off = len(bl[SLOT_IDXENT])
        bl[SLOT_IDXENT] += struct.pack('<fIIII', 1.0, io, xrnd.nondegenerate(strip), len(strip), 6)
        sm = bytearray(t_sub)
        r, c = xrnd.bounding_sphere([v[0] for v in verts])
        sub_name = f'{tag}_{k}'
        struct.pack_into('<II4f', sm, 4, vbe_off, ie_off, r, *c)
        struct.pack_into('<I', sm, 0x34, add_name(sub_name))
        sub_off = len(bl[SLOT_SUBMESH])
        bl[SLOT_SUBMESH] += sm
        new_subs.append(sub_off)
        sub_names.append(sub_name)
        prog_offs = []
        for po in t_progs:
            pr = bytearray(bl[SLOT_PROGRAMS][po:po + 0x2C])
            st = bytearray(cmd_stream(bl[SLOT_CMD], u32(pr, 0x14)))
            i = 0
            while i + 2 <= len(st) and st[i]:
                ln = st[i + 1]
                if st[i] == 0x01 and st[i + 2] == 0 and ln >= 12:
                    struct.pack_into('<I', st, i + 8, tex_off[m['texture']])
                i += ln if ln >= 4 else 4
            co = len(bl[SLOT_CMD])
            bl[SLOT_CMD] += st
            struct.pack_into('<I', pr, 0x14, co)
            prog_offs.append(len(bl[SLOT_PROGRAMS]))
            bl[SLOT_PROGRAMS] += pr
        mo = len(bl[SLOT_MATERIALS])
        bl[SLOT_MATERIALS] += struct.pack(f'<fI{len(prog_offs)}I', 1.0, len(prog_offs), *prog_offs)
        new_mats.append(mo)
    bl[SLOT_INDICES] += b'\0' * (-len(bl[SLOT_INDICES]) % 4)
    bl[SLOT_NAMES] += b'\0' * (-len(bl[SLOT_NAMES]) % 4)

    # pointer lists: A (old + new) then B; geometry counts, offsets and sphere
    list_a += new_subs
    mats_a += new_mats
    bl[SLOT_MESHPTRS] = struct.pack(f'<{len(list_a) + len(list_b)}I', *(list_a + list_b))
    bl[SLOT_MATPTRS] = struct.pack(f'<{len(mats_a) + len(mats_b)}I', *(mats_a + mats_b))
    struct.pack_into('<III', geom, 0x1C, len(list_a), 0, 0)
    if cnt_b:
        struct.pack_into('<III', geom, 0x28, cnt_b, 4 * len(list_a), 4 * len(mats_a))
    r0 = struct.unpack_from('<4f', geom, 0x50)
    pts = [v[0] for m in meshes for v in m['verts']]
    lo = [min([r0[1 + i] - r0[0]] + [p[i] for p in pts]) for i in range(3)]
    hi = [max([r0[1 + i] + r0[0]] + [p[i] for p in pts]) for i in range(3)]
    cen = [(lo[i] + hi[i]) / 2 for i in range(3)]
    rad = max([math.dist(cen, p) for p in pts] + [math.dist(cen, r0[1:]) + r0[0]])
    struct.pack_into('<4f', geom, 0x50, rad, *cen)
    bl[SLOT_GEOM] = geom
    hdr = list(hdr)
    hdr[6] += len(pieces)
    hdr[7] += len(vbs)
    hdr[8] += len(pieces)
    hdr[9] += len(textures)
    hdr[10] += len(pieces)
    new_xr = xrnd.build_xrnd(hdr, bl, order, reloc or (0,))

    out = []
    for t, v, n, p in chunks:
        if t == b'OBBT':                                 # a level: the culling tree must draw the new pieces
            out.append((t, v) + obbt_add(p, n, cnt_a, len(pieces), pts))
            continue
        if t == b'XRND':
            out.append((t, xr_ver, xr_num, new_xr))
        elif t == b'TXTR' and textures:
            entry_len = xrnd.TXTR_ENTRY                 # fixed 0x98 (the payload carries padding)
            e0 = p[:entry_len]
            add = bytearray()
            for name, img in textures:
                e = bytearray(e0)
                nm = (name + '.tga').encode('latin1')[:0x7F]
                e[:0x80] = nm + b'\0' * (0x80 - len(nm))
                # +0x80 -1, +0x84 saved runtime pointer (0), +0x88 width, +0x8C height, then the template's flags
                struct.pack_into('<III', e, 0x84, 0, img.shape[1], img.shape[0])
                add += e
            out.append((t, v, n + len(textures), bytes(p[:entry_len * n]) + bytes(add)))
        elif t == b'SELS':
            parts = bytes(p).split(b'\0')
            names = [s for s in parts if s]
            subs, texs = names[:n_sub], names[n_sub:]
            names = subs + [s.encode('latin1') for s in sub_names] + texs + [(nm + '.tga').encode('latin1') for nm, _ in textures]
            out.append((t, v, n, b''.join(s + b'\0' for s in names)))
        else:
            out.append((t, v, n, p))
    new_ptx = ptx.build_ptx_entries(ptx.read_ptx_entries(ptx_data) + [ptx.encode_entry(nm, img) for nm, img in textures])
    return xrnd.write_chunks(out), new_ptx


def write_preview(path, meshes, cx, cy, cz, k, max_edges=6000):
    """'GPRV', u32 edge count, then per edge 6 floats (x0 y0 z0 x1 y1 z1) - the part's edges after shaping, at its
    placement origin (floor at z = 0, centred), a big model thinned to max_edges evenly spread ones, plus its box"""
    edges = set()
    for mi, m in enumerate(meshes):
        for a, b, c in m['tris']:
            for e in ((a, b), (b, c), (c, a)):
                edges.add((mi,) + (e if e[0] < e[1] else (e[1], e[0])))
    edges = sorted(edges)
    step = max(1, len(edges) // max_edges)
    out = []
    pt = lambda m, i: tuple(((meshes[m]['verts'][i][0][j] - (cx, cy, cz)[j]) * k) for j in range(3))
    for e in edges[::step]:
        out.append(pt(e[0], e[1]) + pt(e[0], e[2]))
    g = [pt(mi, i) for mi, m in enumerate(meshes) for i in range(len(m['verts']))]
    lo = [min(p[j] for p in g) for j in range(3)]
    hi = [max(p[j] for p in g) for j in range(3)]
    for a, b in ((0, 1), (2, 3), (4, 5), (6, 7), (0, 2), (1, 3), (4, 6), (5, 7), (0, 4), (1, 5), (2, 6), (3, 7)):
        ca = [hi[j] if a >> j & 1 else lo[j] for j in range(3)]
        cb = [hi[j] if b >> j & 1 else lo[j] for j in range(3)]
        out.append(tuple(ca) + tuple(cb))
    with open(path, 'wb') as f:
        f.write(b'GPRV' + struct.pack('<I', len(out)))
        for e in out:
            f.write(struct.pack('<6f', *e))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pax')
    ap.add_argument('glb')
    ap.add_argument('--out', help='default: overwrite the prop (.pax and .ptx)')
    ap.add_argument('--ptx', help="the prop's .ptx (default: next to the .pax)")
    ap.add_argument('--at', type=float, nargs=3, default=(0.0, 0.0, 0.0), help="model-space position of the part's origin")
    ap.add_argument('--yaw', type=float, default=0.0, help='turn about z, degrees (after --up / --level)')
    ap.add_argument('--scale', type=float, default=1.0)
    ap.add_argument('--fit', type=float, help='scale so the largest extent is SIZE')
    ap.add_argument('--ground', action='store_true', help="the part's floor rests on --at (scans: the paving, not "
                    'the lowest point)')
    ap.add_argument('--center', action='store_true', help='centre the part on --at in x / y')
    ap.add_argument('--up', default='+z', choices=['+x', '-x', '+y', '-y', '+z', '-z'],
                    help='the GLB axis that is really up, after the usual Y-up -> Z-up turn (as tools/glb2pao.py)')
    ap.add_argument('--rot', help='x,y,z degrees after --up')
    ap.add_argument('--level', action='store_true', help='scans: find the floor / main wall and stand the part upright')
    ap.add_argument('--tilt', help='x,y degrees of lean correction, last')
    ap.add_argument('--flip-v', action='store_true')
    ap.add_argument('--max-texture', type=int, default=1024)
    ap.add_argument('--turn', type=float, default=0.0, help='degrees about the part\'s own vertical axis, after '
                    'centring (the editor\'s placement turn)')
    ap.add_argument('--lean', default='0,0', help='x,y degrees of tilt about the part\'s floor point, after centring '
                    '(the editor\'s placement tilt; applied x, then y, then --turn)')
    ap.add_argument('--clear-box', help='15 numbers c,a0,a1,a2,h (centre, three unit axes, half extents): existing faces '
                    'inside are removed, crossing ones cut at the box')
    ap.add_argument('--preview', help='write the shaped part as a wireframe here (no merge; pax is ignored): '
                    'what the editor draws while placing')
    args = ap.parse_args()
    out = args.out or args.pax
    base = os.path.splitext(os.path.basename(out))[0]
    tag = glb2pax.safe_name(os.path.splitext(os.path.basename(args.glb))[0], 'part')[:20]
    # orientation first, then size and placement in the corrected frame (same steps as tools/glb2pao.py)
    import glb2pao
    ga = types.SimpleNamespace(scale=1.0, fit=None, ground=False, center=False, flip_v=args.flip_v, color=None,
                               texture=None, model_name=glb2pax.safe_name(f'{base}_{tag}', 'part')[:24],
                               up=args.up, rot=args.rot, pose_skins=True)
    gltf, blob, meshes, _ = glb2pax.load_meshes(args.glb, ga)
    trust_up = args.up != '+z' or bool(args.rot)
    for step in ('axis', 'level', 'square', 'tilt'):
        R = glb2pao.axis_rotation(ga) if step == 'axis' else \
            (glb2pao.level_rotation(meshes, trust_up) if args.level else None) if step == 'level' else \
            (glb2pao.square_rotation(meshes, args.yaw) if args.yaw else None) if step == 'square' else \
            glb2pao.tilt_rotation(args.tilt) if args.tilt else None
        if R is not None:
            for m in meshes:
                m['verts'] = [(glb2pao.mul(R, v[0]), glb2pao.mul(R, v[1])) + tuple(v[2:]) for v in m['verts']]
    g = [v[0] for m in meshes for v in m['verts']]
    lo = [min(p[i] for p in g) for i in range(3)]
    hi = [max(p[i] for p in g) for i in range(3)]
    k = args.scale * (args.fit / max(hi[i] - lo[i] for i in range(3)) if args.fit else 1.0)
    cx = (lo[0] + hi[0]) / 2 if args.center else 0.0
    cy = (lo[1] + hi[1]) / 2 if args.center else 0.0
    cz = 0.0
    if args.ground:                                      # the floor: level faces' median height, else the lowest point
        fz = glb2pao.floor_height(meshes)
        cz = fz if fz is not None else lo[2]
    if args.preview:                                     # the shaped part at the origin (before --turn / --at)
        write_preview(args.preview, meshes, cx, cy, cz, k)
        print(f'  preview: {args.preview}')
        return
    tr = math.radians(args.turn)
    ct, st = math.cos(tr), math.sin(tr)
    lx, ly = (math.radians(float(v)) for v in args.lean.split(','))
    cx_, sx_, cy_, sy_ = math.cos(lx), math.sin(lx), math.cos(ly), math.sin(ly)

    def orient(x, y, z):                                 # tilt about x, then y, then turn about z (as the editor)
        y, z = y * cx_ - z * sx_, y * sx_ + z * cx_
        x, z = x * cy_ + z * sy_, -x * sy_ + z * cy_
        return x * ct - y * st, x * st + y * ct, z

    def place(p):
        x, y, z = orient((p[0] - cx) * k, (p[1] - cy) * k, (p[2] - cz) * k)
        return (x + args.at[0], y + args.at[1], z + args.at[2])

    def turn_n(n):
        return orient(*n)
    for m in meshes:
        m['verts'] = [(place(p), turn_n(n)) + tuple(rest) for (p, n, *rest) in m['verts']]
    print(f'  part: up {args.up}{" levelled" if args.level else ""}, size {[round((hi[i] - lo[i]) * k, 2) for i in range(3)]}'
          f', floor at {round((cz - lo[2]) * k, 2)} above its lowest point, placed at {list(args.at)}')
    textures = glb2pax.build_textures(gltf, blob, args.glb, meshes, args.max_texture)
    pax = open(args.pax, 'rb').read()
    ptx_path = args.ptx or os.path.splitext(args.pax)[0] + '.ptx'
    ptx_data = open(ptx_path, 'rb').read()
    box = [float(x) for x in args.clear_box.split(',')] if args.clear_box else None
    new_pax, new_ptx = merge(pax, ptx_data, meshes, textures, tag, box)
    open(out, 'wb').write(new_pax)
    out_ptx = os.path.splitext(out)[0] + '.ptx'
    open(out_ptx, 'wb').write(new_ptx)
    nv = sum(len(m['verts']) for m in meshes)
    nt = sum(len(m['tris']) for m in meshes)
    print(f'merged {args.glb}: {len(meshes)} sub-mesh(es), {nv} vertices, {nt} triangles, {len(textures)} texture(s) '
          f'-> {out} ({len(new_pax)} bytes), {out_ptx} ({len(new_ptx)} bytes)')


if __name__ == '__main__':
    main()

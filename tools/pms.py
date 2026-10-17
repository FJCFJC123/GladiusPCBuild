"""Gladius Xbox level collision (.pms, 'PMS2') - reader, heightfield writer and a copy of the engine's height check.

Layout (loader 0x1D2580, physics_Mesh::HeightCheck 0x1D38D0, per-triangle test 0x1D3620), little-endian:
    +0x00 'PMS2'   +0x04 file size   +0x08 nx   +0x0C ny (grid cells)   +0x10 cell size (float)
    +0x14 vertex count   +0x18 normal count   +0x1C triangle count   +0x20 cell record count   +0x24 list length
    +0x28 bounds min x, y, z   +0x34 bounds max x, y, z   +0x40 1 / cell size (the loader rewrites it)
    +0x44 vertices      float[3] each
          normals       float[3] each (unit; the height check divides by z)
          triangles     16 bytes: u16 v0, v1, v2, v2 alt (third vertex when flag bit 0 is set: either winding),
                        u16 normal, u16 bounds-min vertex, u16 bounds-max vertex (vertices holding the triangle's box
                        corners - appended when no real vertex is one), u16 flags (bit 2 = floor, required by the
                        height check; high nibble = collision category, ANDed with the scene's mask - retail 0x1004)
          cell records  32 bytes: u16 first list entry, u16 count, u16 box-min vertex, u16 box-max vertex, float[3]
                        centre of the box, float radius squared, 8 zero bytes
          list          u16 triangle indices, cell by cell
          cell table    u16[ny][nx]: cell record index + 1 (0 = empty); cell = floor((pos - min) / cell size)
          names         'set_collideFloor\\0' ... then a 0 byte (the loader stops at 0 or 0xFD)
Height check: the cell's triangles whose box holds x, y and whose flags pass the mask; z = v0.z + ((v0.y - y) * n.y +
(v0.x - x) * n.x) / n.z; a triangle counts when z <= the query height and x, y is inside it; the highest wins.
"""
import math
import struct


def read_pms(data):
    tag, size, nx, ny, cell, nv, nn, nt, nc, nl = struct.unpack_from('<4sIIIfIIIII', data, 0)
    if tag != b'PMS2':
        raise ValueError('not a PMS2 file')
    o = 0x44
    V = [struct.unpack_from('<3f', data, o + 12 * i) for i in range(nv)]
    o += 12 * nv
    N = [struct.unpack_from('<3f', data, o + 12 * i) for i in range(nn)]
    o += 12 * nn
    T = [struct.unpack_from('<8H', data, o + 16 * i) for i in range(nt)]
    o += 16 * nt
    C = [struct.unpack_from('<4H4f', data, o + 32 * i) for i in range(nc)]
    o += 32 * nc
    L = list(struct.unpack_from(f'<{nl}H', data, o))
    o += 2 * nl
    G = list(struct.unpack_from(f'<{nx * ny}H', data, o))
    o += 2 * nx * ny
    names = data[o:].split(b'\0')
    bounds = struct.unpack_from('<6f', data, 0x28)
    return {'nx': nx, 'ny': ny, 'cell': cell, 'V': V, 'N': N, 'T': T, 'C': C, 'L': L, 'G': G, 'bounds': bounds,
            'names': [n for n in names if n]}


def height_check(pms, x, y, z, mask=0):
    """the engine's physics_Mesh::HeightCheck: highest floor height at x, y not above z, or None"""
    ox, oy = pms['bounds'][0], pms['bounds'][1]
    cx = int(math.floor((x - ox) / pms['cell']))
    cy = int(math.floor((y - oy) / pms['cell']))
    if cx < 0 or cy < 0 or cx >= pms['nx'] or cy >= pms['ny']:
        return None
    g = pms['G'][cy * pms['nx'] + cx]
    if not g:
        return None
    first, count = pms['C'][g - 1][:2]
    best = None
    V, N = pms['V'], pms['N']
    for k in range(first, first + count):
        t = pms['T'][pms['L'][k]]
        flags = t[7]
        if mask and not (flags & mask):
            continue
        if not flags & 4:
            continue
        lo, hi = V[t[5]], V[t[6]]
        if lo[2] > z or lo[0] > x or x > hi[0] or lo[1] > y or y > hi[1]:
            continue
        n, a = N[t[4]], V[t[0]]
        h = a[2] + ((a[1] - y) * n[1] + (a[0] - x) * n[0]) / n[2]
        if h > z:
            continue
        b, c = V[t[1]], V[t[3] if flags & 1 else t[2]]
        e = lambda p, q: (p[0] - x) * (q[1] - y) - (q[0] - x) * (p[1] - y)
        s = [e(a, b), e(b, c), e(c, a)]
        if not (all(v >= -1e-4 for v in s) or (flags & 1 and all(v <= 1e-4 for v in s))):
            continue
        if best is None or h > best:
            best = h
    return best


def surface_heights(meshes, x0, y0, nx, ny, step, min_up=0.5, top=False):
    """lowest (or with top, highest) walkable surface - faces within acos(min_up) of level, either winding - at each
    grid point: -> [[z or None] * (nx + 1)] * (ny + 1). Lowest suits arenas: a scan's wall tops and the sky it
    mistook for surface sit above the floor."""
    H = [[None] * (nx + 1) for _ in range(ny + 1)]
    for m in meshes:
        V = m['verts']
        for a, b, c in m['tris']:
            pa, pb, pc = V[a][0], V[b][0], V[c][0]
            e1 = [pb[i] - pa[i] for i in range(3)]
            e2 = [pc[i] - pa[i] for i in range(3)]
            n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            ln = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
            if ln == 0 or abs(n[2]) / ln < min_up:
                continue
            i0 = max(0, int(math.ceil((min(pa[0], pb[0], pc[0]) - x0) / step)))
            i1 = min(nx, int(math.floor((max(pa[0], pb[0], pc[0]) - x0) / step)))
            j0 = max(0, int(math.ceil((min(pa[1], pb[1], pc[1]) - y0) / step)))
            j1 = min(ny, int(math.floor((max(pa[1], pb[1], pc[1]) - y0) / step)))
            den = (pb[1] - pc[1]) * (pa[0] - pc[0]) + (pc[0] - pb[0]) * (pa[1] - pc[1])
            if abs(den) < 1e-12:
                continue
            for j in range(j0, j1 + 1):
                y = y0 + j * step
                for i in range(i0, i1 + 1):
                    x = x0 + i * step
                    w0 = ((pb[1] - pc[1]) * (x - pc[0]) + (pc[0] - pb[0]) * (y - pc[1])) / den
                    w1 = ((pc[1] - pa[1]) * (x - pc[0]) + (pa[0] - pc[0]) * (y - pc[1])) / den
                    w2 = 1 - w0 - w1
                    if w0 < -1e-6 or w1 < -1e-6 or w2 < -1e-6:
                        continue
                    z = w0 * pa[2] + w1 * pb[2] + w2 * pc[2]
                    if H[j][i] is None or (z > H[j][i] if top else z < H[j][i]):
                        H[j][i] = z
    return H


def build_pms_heightfield(H, x0, y0, step, cell=4.0, flags=0x1004, fill=None):
    """H[j][i] = height at (x0 + i * step, y0 + j * step); None = no surface (fill, or the quad is left out).
    -> PMS2 bytes (two triangles per grid square, wound counter-clockwise seen from above)"""
    ny1, nx1 = len(H), len(H[0])
    verts, vidx = [], {}

    def vert(p):
        key = (round(p[0], 4), round(p[1], 4), round(p[2], 4))
        if key not in vidx:
            vidx[key] = len(verts)
            verts.append(key)
        return vidx[key]

    def hz(i, j):
        z = H[j][i]
        return fill if z is None else z

    tris = []                                           # (i0, i1, i2) vertex indices
    for j in range(ny1 - 1):
        for i in range(nx1 - 1):
            zs = [hz(i, j), hz(i + 1, j), hz(i, j + 1), hz(i + 1, j + 1)]
            if any(z is None for z in zs):
                continue
            p = [(x0 + (i + di) * step, y0 + (j + dj) * step, z) for (di, dj), z in
                 zip(((0, 0), (1, 0), (0, 1), (1, 1)), zs)]
            a, b, c, d = (vert(q) for q in p)
            tris += [(a, b, c), (c, b, d)]               # CCW from above (as the retail heightfield)
    normals, nidx, recs = [], {}, []
    for a, b, c in tris:
        pa, pb, pc = verts[a], verts[b], verts[c]
        e1 = [pb[k] - pa[k] for k in range(3)]
        e2 = [pc[k] - pa[k] for k in range(3)]
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        ln = math.sqrt(sum(v * v for v in n))
        n = tuple(round(v / ln, 6) for v in n)
        if n not in nidx:
            nidx[n] = len(normals)
            normals.append(n)
        lo = tuple(min(q[k] for q in (pa, pb, pc)) for k in range(3))
        hi = tuple(max(q[k] for q in (pa, pb, pc)) for k in range(3))
        recs.append([a, b, c, c, nidx[n], vert(lo), vert(hi), flags, lo, hi])
    if len(verts) > 0xFFFF or len(tris) > 0xFFFF:
        raise ValueError(f'collision too large for 16-bit indices ({len(verts)} vertices, {len(tris)} triangles): '
                         'use a larger step')
    bmin = [min(v[k] for v in verts) for k in range(3)]
    bmax = [max(v[k] for v in verts) for k in range(3)]
    nx = max(1, int(math.ceil((bmax[0] - bmin[0]) / cell)))
    ny = max(1, int(math.ceil((bmax[1] - bmin[1]) / cell)))
    lists = [[] for _ in range(nx * ny)]
    for t, r in enumerate(recs):
        lo, hi = r[8], r[9]
        cx0 = max(0, int(math.floor((lo[0] - bmin[0]) / cell)))
        cx1 = min(nx - 1, int(math.floor((hi[0] - bmin[0]) / cell)))
        cy0 = max(0, int(math.floor((lo[1] - bmin[1]) / cell)))
        cy1 = min(ny - 1, int(math.floor((hi[1] - bmin[1]) / cell)))
        for cy in range(cy0, cy1 + 1):
            for cx in range(cx0, cx1 + 1):
                lists[cy * nx + cx].append(t)
    cells, flat, G = [], [], [0] * (nx * ny)
    for k, lst in enumerate(lists):
        if not lst:
            continue
        lo = [min(recs[t][8][q] for t in lst) for q in range(3)]
        hi = [max(recs[t][9][q] for t in lst) for q in range(3)]
        c = [(lo[q] + hi[q]) / 2 for q in range(3)]
        r2 = sum(((hi[q] - lo[q]) / 2) ** 2 for q in range(3))
        cells.append((len(flat), len(lst), vert(tuple(lo)), vert(tuple(hi)), *c, r2))
        flat += lst
        G[k] = len(cells)
    if len(verts) > 0xFFFF or len(flat) > 0xFFFF:
        raise ValueError('collision too large for 16-bit indices: use a larger step')
    body = bytearray()
    for v in verts:
        body += struct.pack('<3f', *v)
    for n in normals:
        body += struct.pack('<3f', *n)
    for r in recs:
        body += struct.pack('<8H', *r[:8])
    for c in cells:
        body += struct.pack('<4H4f8x', *c)
    body += struct.pack(f'<{len(flat)}H', *flat)
    body += struct.pack(f'<{len(G)}H', *G)
    body += b'set_collideFloor\0\0'
    head = struct.pack('<4sIIIfIIIII6ff', b'PMS2', 0x44 + len(body), nx, ny, cell, len(verts), len(normals),
                       len(recs), len(cells), len(flat), *bmin, *bmax, 1.0 / cell)
    return head + bytes(body)

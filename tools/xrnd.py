"""XRND (Xbox render data) reader/writer for Gladius .pax models.

Static props: build_static_pax(template, meshes) writes a new .pax whose XRND holds one shared vertex buffer
(stride 24: position, packed normal, UV) and one sub-mesh + material per texture. The template (a retail
stride-24 prop such as mesh/misc/rocks/rock_gen_01.pax) supplies what is copied verbatim: the VERS and CPRT
chunks, the geometry/sub-mesh/program records and the material command stream (one textured, lit material).

    python tools/xrnd.py info model.pax
    python tools/xrnd.py cube template.pax out.pax texname     (test model: a 1 m cube)
"""
import math
import struct
import sys

HDR_LEN = 0x7C
# doeg header block slots (offset in the header) in the order the blocks are laid out by the writer
SLOT_GEOM, SLOT_MESHPTRS, SLOT_SUBMESH, SLOT_IDXENT, SLOT_VBENT, SLOT_VBHDR = 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40
SLOT_MATPTRS, SLOT_MATERIALS, SLOT_PROGRAMS, SLOT_CMD = 0x48, 0x4C, 0x50, 0x64
SLOT_TEXTURES, SLOT_NAMES, SLOT_INDICES, SLOT_VERTICES = 0x70, 0x78, 0x68, 0x6C
SLOTS = list(range(0x2C, 0x7C, 4))
TXTR_ENTRY = 0x98                                    # TXTR: name[0x80], -1, runtime, width, height, flags, 4 x 0


# ------------------------------------------------------------------------------------------------ chunks
def read_chunks(data):
    """[(tag, version, num, payload)] for a .pax/.pao file."""
    out, o = [], 0
    while o + 16 <= len(data):
        tag, size, ver, num = struct.unpack_from('<4sIII', data, o)
        if size < 16 or o + size > len(data):
            raise ValueError(f'bad chunk at {o:#x}')
        out.append((tag, ver, num, data[o + 16:o + size]))
        o += size
        if tag == b'END.':
            break
    return out


def write_chunks(chunks):
    out = bytearray()
    for tag, ver, num, payload in chunks:
        payload = bytes(payload) + b'\0' * (-len(payload) % 4)
        out += struct.pack('<4sIII', tag, 16 + len(payload), ver, num) + payload
    return bytes(out)


# ------------------------------------------------------------------------------------------------ doeg
def parse_xrnd(payload, version=1):
    """-> (reloc list, header dwords[31], {slot: block bytes})"""
    p = 0
    reloc = []
    if version == 1:
        n = struct.unpack_from('<I', payload, 0)[0]
        reloc = list(struct.unpack_from(f'<{n}I', payload, 4))
        p = 4 + n * 4
    hdr = list(struct.unpack_from('<31I', payload, p))
    if payload[p:p + 4] != b'doeg' or hdr[1] != HDR_LEN:
        raise ValueError('not a doeg block')
    base = p + HDR_LEN
    end = len(payload)
    offs = sorted((hdr[s // 4], s) for s in SLOTS if hdr[s // 4] != 0xFFFFFFFF)
    blocks = {}
    for i, (off, s) in enumerate(offs):
        nxt = offs[i + 1][0] if i + 1 < len(offs) else end - base
        blocks[s] = payload[base + off:base + nxt]
    return reloc, hdr, blocks


def build_xrnd(hdr, blocks, order, reloc=(0,)):
    """blocks laid out contiguously in `order`; header offsets, data size and tag filled in"""
    hdr = list(hdr)
    for s in SLOTS:
        hdr[s // 4] = 0xFFFFFFFF
    body = bytearray()
    for s in order:
        if s in blocks:
            hdr[s // 4] = len(body)
            body += blocks[s]
    hdr[0] = struct.unpack('<I', b'doeg')[0]
    hdr[1] = HDR_LEN
    hdr[3] = HDR_LEN + len(body)                         # data size: header + blocks
    head = struct.pack('<I', len(reloc)) + struct.pack(f'<{len(reloc)}I', *reloc)
    return head + struct.pack('<31I', *hdr) + bytes(body)


# ------------------------------------------------------------------------------------------------ geometry helpers
def pack_normal(nx, ny, nz):
    """11:11:10 signed (x, y: /1023, z: /511)"""
    def q(v, bits):
        m = (1 << (bits - 1)) - 1
        i = max(-m, min(m, int(round(v * m))))
        return i & ((1 << bits) - 1)
    return q(nx, 11) | (q(ny, 11) << 11) | (q(nz, 10) << 22)


def bounding_sphere(points):
    """centre of the bounding box, radius to the farthest point"""
    lo = [min(p[i] for p in points) for i in range(3)]
    hi = [max(p[i] for p in points) for i in range(3)]
    c = [(lo[i] + hi[i]) / 2 for i in range(3)]
    r = max(math.dist(c, p) for p in points)
    return r, c


def tris_to_strip(tris):
    """triangle list -> one D3D triangle strip (degenerate joins, winding kept); -> (indices, triangles)"""
    s = []
    for a, b, c in tris:
        if s:
            s += [s[-1], a, a]                           # degenerate join: every triangle across it repeats a vertex
            if (len(s) - 1) % 2:                         # odd strip positions are drawn flipped: start even
                s.append(a)
            s += [b, c]
        else:
            s += [a, b, c]
    return s, len(tris)


def nondegenerate(strip):
    return sum(1 for k in range(len(strip) - 2) if len({strip[k], strip[k + 1], strip[k + 2]}) == 3)


# ------------------------------------------------------------------------------------------------ static prop writer
def pack_color(r, g, b, a=1.0):
    """D3DCOLOR (A R G B in a little-endian dword) from 0..1 floats"""
    q = lambda v: max(0, min(255, int(round(v * 255))))
    return (q(a) << 24) | (q(r) << 16) | (q(g) << 8) | q(b)


def build_static_pax(template, meshes, model_name, colors=None):
    """template: bytes of a retail stride-24 prop .pax (one sub-mesh, one material).
    meshes: [{'texture': 'name' (no .tga), 'verts': [((x,y,z),(nx,ny,nz),(u,v)[,(r,g,b,a)]), ...],
              'tris': [(a,b,c), ...]}]  (Z up, indices local to each mesh).
    colors: write vertex colours (stride 28, vertex format bit 0x10). World-map props are shaded by them
    (retail colours average ~0.4-0.47). Default: on when any vertex has a colour (others get white).
    -> bytes of the new .pax"""
    if colors is None:
        colors = any(len(v) > 3 for m in meshes for v in m['verts'])
    chunks = read_chunks(template)
    by_tag = {t: (t, v, n, p) for t, v, n, p in chunks}
    xr_tag, xr_ver, xr_num, xr_pay = by_tag[b'XRND']
    reloc, thdr, tb = parse_xrnd(xr_pay, xr_ver)
    t_sub = tb[SLOT_SUBMESH][:0x38]
    t_prog = tb[SLOT_PROGRAMS][:0x2C]
    t_cmd = tb[SLOT_CMD]
    t_tex = tb[SLOT_TEXTURES][:0x40]
    t_txtr = by_tag[b'TXTR'][3]
    if struct.unpack_from('<I', tb[SLOT_VBENT], 16)[0] != 24:
        raise ValueError('template must be a stride-24 prop')
    # texture bind in the command stream: op 0x01 sub 0, pointer at +8 -> texture object
    # vertex format: op 0x09, format word at +4 (bit 0x10 = vertex colour)
    cmd_tex, cmd_fmt = [], []
    i = 0
    while i < len(t_cmd) and t_cmd[i]:
        op, ln = t_cmd[i], t_cmd[i + 1]
        if op == 0x01 and t_cmd[i + 2] == 0:
            cmd_tex.append(i + 8)
        if op == 0x09:
            cmd_fmt.append(i + 4)
        i += ln if ln else 4
    if len(cmd_tex) != 1 or len(cmd_fmt) != 1:
        raise ValueError(f'template material binds {len(cmd_tex)} textures / {len(cmd_fmt)} formats, expected 1')
    t_cmd = bytearray(t_cmd)
    fmt = struct.unpack_from('<I', t_cmd, cmd_fmt[0])[0]
    struct.pack_into('<I', t_cmd, cmd_fmt[0], fmt | 0x10 if colors else fmt & ~0x10)
    stride = 28 if colors else 24

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

    vertices, indices = bytearray(), bytearray()
    submesh, idxent, meshptrs, matptrs, materials, programs, cmds = (bytearray() for _ in range(7))
    all_pos = []
    vbase = 0
    for k, m in enumerate(meshes):
        pos = [v[0] for v in m['verts']]
        all_pos += pos
        for vt in m['verts']:
            (x, y, z), (nx, ny, nz), (u, v) = vt[:3]
            if colors:
                col = pack_color(*vt[3]) if len(vt) > 3 else 0xFFFFFFFF
                vertices += struct.pack('<fffIIff', x, y, z, pack_normal(nx, ny, nz), col, u, v)
            else:
                vertices += struct.pack('<fffIff', x, y, z, pack_normal(nx, ny, nz), u, v)
        strip, _ = tris_to_strip([(a + vbase, b + vbase, c + vbase) for a, b, c in m['tris']])
        if max(strip) > 0xFFFF:
            raise ValueError('more than 65535 vertices: split the model')
        idx_off = len(indices)
        indices += struct.pack(f'<{len(strip)}H', *strip)
        indices += b'\0' * (-len(indices) % 4)
        idxent += struct.pack('<fIIII', 1.0, idx_off, nondegenerate(strip), len(strip), 6)
        r, c = bounding_sphere(pos)
        sm = bytearray(t_sub)
        struct.pack_into('<II', sm, 4, 0, k * 0x14)         # shared vertex buffer entry, own index entry
        struct.pack_into('<4f', sm, 0x0C, r, *c)
        meshptrs += struct.pack('<I', k * 0x38)
        submesh += sm
        matptrs += struct.pack('<I', k * 0x0C)
        materials += struct.pack('<fII', 1.0, 1, k * 0x2C)
        pr = bytearray(t_prog)
        struct.pack_into('<I', pr, 0x14, len(cmds))
        programs += pr
        cm = bytearray(t_cmd)
        struct.pack_into('<I', cm, cmd_tex[0], textures.index(m['texture']) * 0x40)
        cmds += cm
        vbase += len(pos)
    if vbase > 0xFFFF:
        raise ValueError('more than 65535 vertices in one vertex buffer: split the model')

    texobj = bytearray()
    for t in textures:
        to = bytearray(t_tex)
        struct.pack_into('<I', to, 0x10, name_off[t])
        texobj += to
    r, c = bounding_sphere(all_pos)
    geom = bytearray(tb[SLOT_GEOM])
    struct.pack_into('<I', geom, 0x1C, len(meshes))
    struct.pack_into('<4f', geom, 0x50, r, *c)
    vbent = bytearray(tb[SLOT_VBENT])
    struct.pack_into('<I', vbent, 4, vbase)
    struct.pack_into('<I', vbent, 0x10, stride)
    blocks = {SLOT_GEOM: geom, SLOT_MESHPTRS: meshptrs, SLOT_SUBMESH: submesh, SLOT_IDXENT: idxent,
              SLOT_VBENT: vbent, SLOT_VBHDR: tb[SLOT_VBHDR], SLOT_MATPTRS: matptrs, SLOT_MATERIALS: materials,
              SLOT_PROGRAMS: programs, SLOT_CMD: cmds, SLOT_TEXTURES: texobj, SLOT_NAMES: names,
              SLOT_INDICES: indices, SLOT_VERTICES: vertices}
    order = [SLOT_GEOM, SLOT_MESHPTRS, SLOT_SUBMESH, SLOT_IDXENT, SLOT_VBENT, SLOT_VBHDR, SLOT_MATPTRS,
             SLOT_MATERIALS, SLOT_PROGRAMS, SLOT_CMD, SLOT_TEXTURES, SLOT_NAMES, SLOT_INDICES, SLOT_VERTICES]
    hdr = list(thdr)
    hdr[5:11] = [1, len(meshes), 1, len(meshes), len(textures), len(meshes)]
    xrnd = build_xrnd(hdr, blocks, order, reloc or (0,))

    # TXTR: one template entry per texture, name replaced (entry = name 0x80 + the template's fields)
    entry_len = TXTR_ENTRY                               # fixed 0x98 (the payload carries padding)
    txtr = bytearray()
    for t in textures:
        e = bytearray(t_txtr[:entry_len])
        nm = (t + '.tga').encode('latin1')[:0x7F]
        e[:0x80] = nm + b'\0' * (0x80 - len(nm))
        e[0x84:0x90] = b'\0' * 12                        # exporter's saved runtime values (pointer, size)
        txtr += e
    # SELS: one selection-set name per sub-mesh, then the texture file names
    sels = bytearray()
    for k, m in enumerate(meshes):
        sels += m.get('name', f'{model_name}_{model_name}' if len(meshes) == 1 else f'{model_name}_{k}').encode('latin1') + b'\0'
    for t in textures:
        sels += (t + '.tga').encode('latin1') + b'\0'
    out = []
    for tag, ver, num, payload in chunks:
        if tag == b'TXTR':
            out.append((tag, ver, len(textures), txtr))
        elif tag == b'SELS':
            out.append((tag, ver, 1, sels))
        elif tag == b'XRND':
            out.append((tag, xr_ver, xr_num, xrnd))
        else:
            out.append((tag, ver, num, payload))
    return write_chunks(out)


# ------------------------------------------------------------------------------------------------ levels (.pao)
# The geometry record lists its sub-meshes twice: list A (+0x1C count, +0x20 sub-mesh pointer list, +0x24 material
# pointer list - opaque) and list B (+0x28 / +0x2C / +0x30 - drawn after A: the transparent pass, e.g. Expanse
# Shore's seaweed / water). Entry i of a list pairs sub-mesh i with material i.
#
# OBBT (culling tree, walked from node 0): payload = the map (int16 entries; the chunk's count field = its length in
# BYTES) followed by the nodes, 0x40 bytes each: 3 rows of (axis x, y, z, half extent) + centre, then u16 first map
# entry, u16 count. A map entry -n descends into node n; a value >= 0 draws that sub-mesh (list A then list B order).

def level_positions(pao):
    """every vertex position of a .pao / .pax (all vertex buffers) -> [(x, y, z)]"""
    xr = [c for c in read_chunks(pao) if c[0] == b'XRND'][0]
    _, _, b = parse_xrnd(xr[3], xr[1])
    out = []
    for j in range(len(b[SLOT_VBENT]) // 0x14):
        hdr_off, count, _, _, stride = struct.unpack_from('<IIIiI', b[SLOT_VBENT], j * 0x14)
        data = struct.unpack_from('<I', b[SLOT_VBHDR], hdr_off + 4)[0]
        out += [struct.unpack_from('<3f', b[SLOT_VERTICES], data + i * stride) for i in range(count)]
    return out


def build_level_pao(template, arena, meshes, cell=16.0, colors=None, max_vb=0xFFFF):
    """A new level .pao from GLB meshes (same form as build_static_pax), for the arena whose retail .pao is `arena`.
    template: the stride-24 prop .pax whose records / material command stream are copied (one textured material).
    Each texture's triangles are cut into `cell` x `cell` columns (x, y) - one sub-mesh per texture per column, so
    the OBBT (root -> one box per column -> its sub-meshes) culls the level piece by piece. Sub-meshes are packed into
    vertex buffers of at most `max_vb` vertices (indices are 16-bit and local to their buffer; a sub-mesh picks its
    buffer with +4, the buffer's D3D header (Common -1, Data = byte offset in the vertex block, Lock -1) - as the
    retail world maps do). VERS / CPRT / SELS / STYP / PADD come from the arena, TXTR / OBBT / XRND are new.
    -> (pao bytes, stats dict)"""
    if colors is None:
        colors = any(len(v) > 3 for m in meshes for v in m['verts'])
    tch = {t: (t, v, n, p) for t, v, n, p in read_chunks(template)}
    xr_ver, xr_num = tch[b'XRND'][1], tch[b'XRND'][2]
    reloc, thdr, tb = parse_xrnd(tch[b'XRND'][3], xr_ver)
    if struct.unpack_from('<I', tb[SLOT_VBENT], 16)[0] != 24:
        raise ValueError('template must be a stride-24 prop')
    t_sub, t_prog, t_tex = tb[SLOT_SUBMESH][:0x38], tb[SLOT_PROGRAMS][:0x2C], tb[SLOT_TEXTURES][:0x40]
    t_cmd = bytearray(tb[SLOT_CMD])
    cmd_tex, cmd_fmt, i = [], [], 0
    while i < len(t_cmd) and t_cmd[i]:
        op, ln = t_cmd[i], t_cmd[i + 1]
        if op == 0x01 and t_cmd[i + 2] == 0:
            cmd_tex.append(i + 8)
        if op == 0x09:
            cmd_fmt.append(i + 4)
        i += ln if ln else 4
    if len(cmd_tex) != 1 or len(cmd_fmt) != 1:
        raise ValueError('template material must bind one texture')
    fmt = struct.unpack_from('<I', t_cmd, cmd_fmt[0])[0]
    struct.pack_into('<I', t_cmd, cmd_fmt[0], fmt | 0x10 if colors else fmt & ~0x10)
    stride = 28 if colors else 24

    textures = []
    for m in meshes:
        if m['texture'] not in textures:
            textures.append(m['texture'])

    # pieces: (column, texture, verts, tris local to verts), each within max_vb vertices
    pieces = []
    for m in meshes:
        V = m['verts']
        cols = {}
        for t in m['tris']:
            cx = (V[t[0]][0][0] + V[t[1]][0][0] + V[t[2]][0][0]) / 3
            cy = (V[t[0]][0][1] + V[t[1]][0][1] + V[t[2]][0][1]) / 3
            cols.setdefault((math.floor(cx / cell), math.floor(cy / cell)), []).append(t)
        for key, tris in cols.items():
            remap, vs, ts = {}, [], []
            for t in tris:
                if len(vs) + sum(1 for x in t if x not in remap) > max_vb:
                    pieces.append((key, m['texture'], vs, ts))
                    remap, vs, ts = {}, [], []
                for x in t:
                    if x not in remap:
                        remap[x] = len(vs)
                        vs.append(V[x])
                ts.append(tuple(remap[x] for x in t))
            if ts:
                pieces.append((key, m['texture'], vs, ts))
    pieces.sort(key=lambda p: (p[0], textures.index(p[1])))

    # vertex buffers
    vbs = []                                            # [bytearray of vertices, count]
    submesh, idxent, meshptrs, matptrs, indices = (bytearray() for _ in range(5))
    col_meshes = {}                                     # column -> [sub-mesh index]
    col_bounds = {}
    for k, (key, tex, vs, ts) in enumerate(pieces):
        if not vbs or vbs[-1][1] + len(vs) > max_vb:
            vbs.append([bytearray(), 0])
        vb = vbs[-1]
        base = vb[1]
        for vt in vs:
            (x, y, z), (nx, ny, nz), (u, v) = vt[:3]
            if colors:
                c = pack_color(*vt[3]) if len(vt) > 3 else 0xFFFFFFFF
                vb[0] += struct.pack('<fffIIff', x, y, z, pack_normal(nx, ny, nz), c, u, v)
            else:
                vb[0] += struct.pack('<fffIff', x, y, z, pack_normal(nx, ny, nz), u, v)
        vb[1] += len(vs)
        strip, _ = tris_to_strip([(a + base, b + base, c + base) for a, b, c in ts])
        off = len(indices)
        indices += struct.pack(f'<{len(strip)}H', *strip)
        indices += b'\0' * (-len(indices) % 4)
        idxent += struct.pack('<fIIII', 1.0, off, nondegenerate(strip), len(strip), 6)
        pos = [v[0] for v in vs]
        r, c = bounding_sphere(pos)
        sm = bytearray(t_sub)
        struct.pack_into('<II', sm, 4, (len(vbs) - 1) * 0x14, k * 0x14)
        struct.pack_into('<4f', sm, 0x0C, r, *c)
        submesh += sm
        meshptrs += struct.pack('<I', k * 0x38)
        matptrs += struct.pack('<I', textures.index(tex) * 0x0C)
        col_meshes.setdefault(key, []).append(k)
        lo = [min(p[i] for p in pos) for i in range(3)]
        hi = [max(p[i] for p in pos) for i in range(3)]
        if key in col_bounds:
            blo, bhi = col_bounds[key]
            lo = [min(lo[i], blo[i]) for i in range(3)]
            hi = [max(hi[i], bhi[i]) for i in range(3)]
        col_bounds[key] = (lo, hi)

    vertices, vbent, vbhdr = bytearray(), bytearray(), bytearray()
    for j, (data, count) in enumerate(vbs):
        vbhdr += struct.pack('<III', 0xFFFFFFFF, len(vertices), 0xFFFFFFFF)
        vbent += struct.pack('<IIIiI', j * 12, count, 0, -1, stride)
        vertices += data

    materials, programs, cmds, texobj, names = (bytearray() for _ in range(5))
    for j, t in enumerate(textures):
        materials += struct.pack('<fII', 1.0, 1, j * 0x2C)
        pr = bytearray(t_prog)
        struct.pack_into('<I', pr, 0x14, len(cmds))
        programs += pr
        cm = bytearray(t_cmd)
        struct.pack_into('<I', cm, cmd_tex[0], j * 0x40)
        cmds += cm
        to = bytearray(t_tex)
        struct.pack_into('<I', to, 0x10, len(names))
        texobj += to
        names += t.encode('latin1') + b'\0'
    names += b'\0' * (-len(names) % 4)

    lo = [min(b[0][i] for b in col_bounds.values()) for i in range(3)]
    hi = [max(b[1][i] for b in col_bounds.values()) for i in range(3)]
    r, c = bounding_sphere([lo, hi])
    geom = bytearray(tb[SLOT_GEOM])
    struct.pack_into('<iIIiII', geom, 0x1C, len(pieces), 0, 0, 0, 4 * len(pieces), 4 * len(pieces))
    struct.pack_into('<4f', geom, 0x50, r, *c)
    blocks = {SLOT_GEOM: geom, SLOT_MESHPTRS: meshptrs, SLOT_SUBMESH: submesh, SLOT_IDXENT: idxent,
              SLOT_VBENT: vbent, SLOT_VBHDR: vbhdr, SLOT_MATPTRS: matptrs, SLOT_MATERIALS: materials,
              SLOT_PROGRAMS: programs, SLOT_CMD: cmds, SLOT_TEXTURES: texobj, SLOT_NAMES: names,
              SLOT_INDICES: indices, SLOT_VERTICES: vertices}
    order = [SLOT_GEOM, SLOT_MESHPTRS, SLOT_SUBMESH, SLOT_IDXENT, SLOT_VBENT, SLOT_VBHDR, SLOT_MATPTRS,
             SLOT_MATERIALS, SLOT_PROGRAMS, SLOT_CMD, SLOT_TEXTURES, SLOT_NAMES, SLOT_INDICES, SLOT_VERTICES]
    hdr = list(thdr)
    hdr[5:11] = [1, len(pieces), len(vbs), len(pieces), len(textures), len(textures)]
    xrnd = build_xrnd(hdr, blocks, order, reloc or (0,))

    # OBBT: node 0 = the whole level -> node n = column n -> its sub-meshes
    def node(lo, hi, first, count):
        ce = [(lo[i] + hi[i]) / 2 for i in range(3)]
        ex = [max((hi[i] - lo[i]) / 2, 0.01) for i in range(3)]
        return struct.pack('<12f3fHH', 1, 0, 0, ex[0], 0, 1, 0, ex[1], 0, 0, 1, ex[2], *ce, first, count)
    keys = sorted(col_meshes)
    cmap = [-(n + 1) for n in range(len(keys))]
    nodes = node(lo, hi, 0, len(keys))
    for key in keys:
        nodes += node(*col_bounds[key], len(cmap), len(col_meshes[key]))
        cmap += col_meshes[key]
    if len(cmap) > 0x7FFF:
        raise ValueError('OBBT map too long')
    obbt = struct.pack(f'<{len(cmap)}h', *cmap) + nodes

    ach = read_chunks(arena)
    t_txtr = [c for c in ach if c[0] == b'TXTR'][0]
    entry_len = TXTR_ENTRY
    txtr = bytearray()
    for t in textures:
        e = bytearray(t_txtr[3][:entry_len])
        nm = (t + '.tga').encode('latin1')[:0x7F]
        e[:0x80] = nm + b'\0' * (0x80 - len(nm))
        e[0x84:0x90] = b'\0' * 12
        txtr += e
    out = []
    for tag, ver, num, payload in ach:
        if tag == b'TXTR':
            out.append((tag, ver, len(textures), txtr))
        elif tag == b'OBBT':
            out.append((tag, ver, 2 * len(cmap), obbt))
        elif tag == b'XRND':
            out.append((tag, xr_ver, xr_num, xrnd))
        else:
            out.append((tag, ver, num, payload))
    stats = {'submeshes': len(pieces), 'vbs': [n for _, n in vbs], 'textures': len(textures), 'columns': len(keys),
             'bounds': (lo, hi), 'stride': stride}
    return write_chunks(out), stats


def level_lists(blocks):
    """-> [(submesh offset, material offset, in_list_b)] in list A then list B order"""
    g = blocks[SLOT_GEOM]
    ca, pa, ma, cb, pb, mb = struct.unpack_from('<iIIiII', g, 0x1C)
    out = []
    for cnt, pl, ml, b in ((ca, pa, ma, False), (cb, pb, mb, True)):
        for i in range(max(cnt, 0)):
            sm = struct.unpack_from('<I', blocks[SLOT_MESHPTRS], pl + 4 * i)[0]
            mt = struct.unpack_from('<I', blocks[SLOT_MATPTRS], ml + 4 * i)[0]
            out.append((sm, mt, b))
    return out


def obbt_single_root(lo, hi, n_meshes):
    """OBBT payload with one node: an axis-aligned box lo..hi listing every sub-mesh -> (payload, count field)"""
    cmap = struct.pack(f'<{n_meshes}h', *range(n_meshes))
    c = [(lo[k] + hi[k]) / 2 for k in range(3)]
    e = [max((hi[k] - lo[k]) / 2, 0.01) for k in range(3)]
    node = struct.pack('<12f3fHH', 1, 0, 0, e[0], 0, 1, 0, e[1], 0, 0, 1, e[2], *c, 0, n_meshes)
    return cmap + node, len(cmap)


def rebuild_level(pao):
    """Round-trip test for the level writer: the geometry blocks of a retail .pao rebuilt from what they describe
    (index data, index entries, sub-mesh records with recomputed bounds, pointer lists, both geometry lists), the
    OBBT replaced by one root box. Materials, programs, command streams, textures, names and the vertex buffer are
    copied. -> new .pao bytes"""
    chunks = read_chunks(pao)
    xr = [c for c in chunks if c[0] == b'XRND'][0]
    reloc, hdr, b = parse_xrnd(xr[3], xr[1])
    entries = level_lists(b)
    if len(b[SLOT_VBENT]) != 0x14:
        raise ValueError('expected one vertex buffer')
    stride = struct.unpack_from('<I', b[SLOT_VBENT], 16)[0]
    vb = b[SLOT_VERTICES]
    nverts = len(vb) // stride
    pos = [struct.unpack_from('<3f', vb, i * stride) for i in range(nverts)]
    submesh, idxent, indices, meshptrs, matptrs = (bytearray() for _ in range(5))
    count_a = sum(1 for e in entries if not e[2])
    for k, (sm_off, mt_off, in_b) in enumerate(entries):
        old = b[SLOT_SUBMESH][sm_off:sm_off + 0x38]
        ie_off = struct.unpack_from('<I', old, 8)[0]
        _one, data_off, _nd, cnt, prim = struct.unpack_from('<fIIII', b[SLOT_IDXENT], ie_off)
        strip = list(struct.unpack_from(f'<{cnt}H', b[SLOT_INDICES], data_off))
        new_off = len(indices)
        indices += struct.pack(f'<{cnt}H', *strip)
        indices += b'\0' * (-len(indices) % 4)
        idxent += struct.pack('<fIIII', 1.0, new_off, nondegenerate(strip), cnt, prim)
        r, c = bounding_sphere([pos[i] for i in set(strip)])
        sm = bytearray(old)
        struct.pack_into('<II', sm, 4, 0, k * 0x14)
        struct.pack_into('<4f', sm, 0x0C, r, *c)
        meshptrs += struct.pack('<I', k * 0x38)
        submesh += sm
        matptrs += struct.pack('<I', mt_off)            # materials block copied: same offsets
    geom = bytearray(b[SLOT_GEOM])
    n_b = len(entries) - count_a
    struct.pack_into('<iIIiII', geom, 0x1C, count_a, 0, 0, n_b, 4 * count_a, 4 * count_a)
    lo = [min(p[k] for p in pos) for k in range(3)]
    hi = [max(p[k] for p in pos) for k in range(3)]
    r, c = bounding_sphere([lo, hi])
    struct.pack_into('<4f', geom, 0x50, r, *c)
    blocks = dict(b)
    blocks.update({SLOT_GEOM: geom, SLOT_MESHPTRS: meshptrs, SLOT_SUBMESH: submesh, SLOT_IDXENT: idxent,
                   SLOT_MATPTRS: matptrs, SLOT_INDICES: indices})
    order = [sl for _, sl in sorted((hdr[sl // 4], sl) for sl in b)]     # original block order
    hdr2 = list(hdr)
    hdr2[6] = hdr2[8] = len(entries)
    out_xrnd = build_xrnd(hdr2, blocks, order, reloc or (0,))
    obbt, obbt_count = obbt_single_root(lo, hi, len(entries))
    out = []
    for tag, ver, num, payload in chunks:
        if tag == b'XRND':
            out.append((tag, ver, num, out_xrnd))
        elif tag == b'OBBT':
            out.append((tag, ver, obbt_count, obbt))
        else:
            out.append((tag, ver, num, payload))
    return write_chunks(out)


# ------------------------------------------------------------------------------------------------ test model
def cube(texture, size=1.0, color=None):
    h = size / 2
    faces = [((1, 0, 0), (0, 1, 0), (0, 0, 1)), ((-1, 0, 0), (0, -1, 0), (0, 0, 1)), ((0, 1, 0), (-1, 0, 0), (0, 0, 1)),
             ((0, -1, 0), (1, 0, 0), (0, 0, 1)), ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (1, 0, 0), (0, -1, 0))]
    verts, tris = [], []
    for n, uax, vax in faces:
        b = len(verts)
        for u, v in ((0, 0), (1, 0), (1, 1), (0, 1)):
            p = tuple(n[i] * h + (u - 0.5) * size * uax[i] + (v - 0.5) * size * vax[i] + (h if i == 2 else 0)
                      for i in range(3))                 # sits on the ground (z from 0 to size)
            verts.append((p, n, (u, 1 - v)) + ((color,) if color else ()))
        tris += [(b, b + 1, b + 2), (b, b + 2, b + 3)]
    return [{'texture': texture, 'verts': verts, 'tris': tris}]


def info(path):
    for tag, ver, num, payload in read_chunks(open(path, 'rb').read()):
        line = f'{tag.decode("latin1")} ver {ver} num {num} {len(payload)} bytes'
        if tag == b'XRND':
            reloc, hdr, blocks = parse_xrnd(payload, ver)
            line += f'  counts {hdr[5:11]}  blocks ' + ' '.join(f'+{s:02X}:{len(b)}' for s, b in sorted(blocks.items()))
        print(line)


if __name__ == '__main__':
    if sys.argv[1] == 'info':
        info(sys.argv[2])
    elif sys.argv[1] == 'cube':
        tpl, out, tex = sys.argv[2], sys.argv[3], sys.argv[4]
        name = out.replace('\\', '/').rsplit('/', 1)[-1].rsplit('.', 1)[0]
        open(out, 'wb').write(build_static_pax(open(tpl, 'rb').read(), cube(tex), name))
        info(out)

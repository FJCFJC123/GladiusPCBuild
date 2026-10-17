"""GLB scene -> Gladius Xbox level .pao + .ptx (the visual mesh of an arena / world map) - XRND layout: tools/xrnd.py.

The new .pao replaces the arena's visuals only: its VERS / CPRT / SELS / STYP / PADD chunks are kept from the retail
arena .pao, the arena's collision (.pms) and grid (.grd) stay as they are. Textures go to <out>.ptx - the level
loads the .ptx of the same name, so write both over the arena's (as loose files in the game's data folder).

    python tools/glb2pao.py scene.glb bec_unpacked/data/levels/expanse/expanseshore/expanseshore.pao \
        game/data/levels/expanse/expanseshore/expanseshore.pao \
        --template bec_unpacked/data/mesh/misc/rocks/rock_gen_01.pax --fit 160 --at 32,32 --ground-z -0.4
"""
import argparse
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glb2pax  # noqa: E402
import xrnd  # noqa: E402


def mul(R, v):
    return tuple(R[i][0] * v[0] + R[i][1] * v[1] + R[i][2] * v[2] for i in range(3))


def rot_between(a, b):
    """rotation matrix turning unit vector a onto unit vector b (Rodrigues)"""
    v = (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    cs = sum(a[i] * b[i] for i in range(3))
    sn = math.sqrt(sum(x * x for x in v))
    if sn < 1e-9:                                       # parallel: none, opposite: half a turn about x
        return [[1, 0, 0], [0, 1, 0], [0, 0, 1]] if cs > 0 else [[1, 0, 0], [0, -1, 0], [0, 0, -1]]
    k = [x / sn for x in v]
    K = [[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]]
    K2 = [[sum(K[i][t] * K[t][j] for t in range(3)) for j in range(3)] for i in range(3)]
    return [[(1 if i == j else 0) + sn * K[i][j] + (1 - cs) * K2[i][j] for j in range(3)] for i in range(3)]


def axis_rotation(args):
    """--up (which GLB axis is really up), then --rot x,y,z degrees (applied x, then y, then z) -> matrix or None"""
    turns = {'+z': (0, 0, 0), '-z': (180, 0, 0), '+y': (90, 0, 0), '-y': (-90, 0, 0), '+x': (0, -90, 0),
             '-x': (0, 90, 0)}[args.up]
    if args.up != '+z' or args.rot:
        R = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
        extra = [float(x) for x in args.rot.split(',')] if args.rot else [0, 0, 0]
        for axis, deg in list(zip(range(3), turns)) + list(zip(range(3), extra)):
            c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
            A = [[1, 0, 0], [0, c, -s], [0, s, c]] if axis == 0 else \
                [[c, 0, s], [0, 1, 0], [-s, 0, c]] if axis == 1 else [[c, -s, 0], [s, c, 0], [0, 0, 1]]
            R = [[sum(A[i][t] * R[t][j] for t in range(3)) for j in range(3)] for i in range(3)]
        return R
    return None


def level_rotation(meshes, trust_up=False):
    """--level (after --up / --rot): the floor's normal = the area-weighted mean of the faces pointing up (within 60
    degrees, then refined within 30 of that); a large wall (the most face area in one direction, not the floor) is
    stood exactly upright; the result is turned to +z. Scans are often exported tilted - Sketchfab levels them in its
    viewer, not in the file."""
    faces = []
    for m in meshes:
        V = m['verts']
        for a, b, c in m['tris']:
            pa, pb, pc = V[a][0], V[b][0], V[c][0]
            e1 = [pb[i] - pa[i] for i in range(3)]
            e2 = [pc[i] - pa[i] for i in range(3)]
            faces.append((e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]))
    # which way is up: the side (z up or z down) with more face area within 60 degrees of it. Scans only have the
    # surfaces the cameras saw - from above - so an upside-down export shows its ground facing down.
    area = lambda f: math.sqrt(f[0] ** 2 + f[1] ** 2 + f[2] ** 2)
    def refine(n, limits, either_side=False):           # area-weighted mean of the faces within each cone of n
        for limit in limits:
            acc = [0.0, 0.0, 0.0]
            for f in faces:
                ln = area(f)
                d = (f[0] * n[0] + f[1] * n[1] + f[2] * n[2]) / ln if ln > 0 else 0
                if either_side and d < 0:               # double-sided scans: winding says nothing about the side
                    f, d = (-f[0], -f[1], -f[2]), -d
                if ln > 0 and d > limit:
                    for i in range(3):
                        acc[i] += f[i]                  # cross product length = 2 x area: area-weighted
            ln = math.sqrt(sum(x * x for x in acc))
            if ln == 0:
                return None
            n = tuple(x / ln for x in acc)
        return n
    if trust_up:
        n = refine((0.0, 0.0, 1.0), (0.5, 0.866), either_side=True)
        if n is None:
            return None
        tilt = math.degrees(math.acos(max(-1.0, min(1.0, n[2]))))
        print(f'  level: floor normal ({n[0]:.3f}, {n[1]:.3f}, {n[2]:.3f}) - tilted {tilt:.1f} degrees, levelled')
        return rot_between(n, (0.0, 0.0, 1.0))
    up = sum(area(f) for f in faces if f[2] > 0.5 * area(f) > 0)
    down = sum(area(f) for f in faces if -f[2] > 0.5 * area(f) > 0)
    n = (0.0, 0.0, 1.0) if up >= down else (0.0, 0.0, -1.0)
    if down > up:
        print(f'  level: more ground faces down than up ({down / max(up, 1e-9):.1f}x) - the scene is upside down')
    n = refine(n, (0.5, 0.866))
    if n is None:
        return None
    # the largest flat surface: a building scan's main wall stands it upright exactly (the ground of a ruin is mixed
    # with grass banks and rubble; its walls are planes). Seed = the 30-degree direction bin with the most area.
    bins = {}
    for f in faces:
        ln = area(f)
        if ln > 0:
            key = tuple(round(f[i] / ln * 2) for i in range(3))
            bins[key] = bins.get(key, 0.0) + ln
    seed = max(bins, key=bins.get)
    ln = math.sqrt(sum(x * x for x in seed)) or 1.0
    w = refine(tuple(x / ln for x in seed), (0.866, 0.94))
    if w is not None:
        d = sum(n[i] * w[i] for i in range(3))
        if abs(d) < 0.7:                                # a wall (not the floor itself): make it vertical
            up = [n[i] - d * w[i] for i in range(3)]
            ln = math.sqrt(sum(x * x for x in up))
            print(f'  level: main wall normal ({w[0]:.3f}, {w[1]:.3f}, {w[2]:.3f}), '
                  f'{math.degrees(math.acos(min(1.0, abs(d)))):.1f} degrees from the floor - stood upright')
            n = tuple(x / ln for x in up)
    tilt = math.degrees(math.acos(max(-1.0, min(1.0, n[2]))))
    print(f'  level: floor normal ({n[0]:.3f}, {n[1]:.3f}, {n[2]:.3f}) - tilted {tilt:.1f} degrees, turned upright')
    return rot_between(n, (0.0, 0.0, 1.0))


def face_list(meshes):
    """[(cross product (2 x area along the normal), centroid)] of every triangle"""
    out = []
    for m in meshes:
        V = m['verts']
        for a, b, c in m['tris']:
            pa, pb, pc = V[a][0], V[b][0], V[c][0]
            e1 = [pb[i] - pa[i] for i in range(3)]
            e2 = [pc[i] - pa[i] for i in range(3)]
            n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            out.append((n, ((pa[0] + pb[0] + pc[0]) / 3, (pa[1] + pb[1] + pc[1]) / 3, (pa[2] + pb[2] + pc[2]) / 3)))
    return out


def square_rotation(meshes, yaw):
    """turn about z so the walls (faces within ~11 degrees of vertical) run along the x / y axes - the battle grid's.
    Wall directions are folded to 90 degrees (4 x angle) so walls on all four sides agree; area-weighted. + yaw."""
    sx = sy = 0.0
    for n, _ in face_list(meshes):
        ln = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
        if ln > 0 and abs(n[2]) / ln < 0.2:
            a = 4 * math.atan2(n[1], n[0])
            sx += ln * math.cos(a)
            sy += ln * math.sin(a)
    turn = -math.degrees(math.atan2(sy, sx) / 4) if sx or sy else 0.0
    turn += yaw
    print(f'  square: walls {-turn + yaw:.1f} degrees off the grid axes - turned {turn:.1f} degrees about z')
    c, s = math.cos(math.radians(turn)), math.sin(math.radians(turn))
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


def tilt_rotation(tilt):
    """--tilt x,y: final lean correction in degrees about the x axis, then the y axis (+x tilts the top toward -y,
    +y tilts the top toward +x)"""
    tx, ty = (float(v) for v in tilt.split(','))
    cx, sx = math.cos(math.radians(tx)), math.sin(math.radians(tx))
    cy, sy = math.cos(math.radians(ty)), math.sin(math.radians(ty))
    rx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    print(f'  tilt: {tx:g} degrees about x, {ty:g} about y')
    return [[sum(ry[i][t] * rx[t][j] for t in range(3)) for j in range(3)] for i in range(3)]


def sky_meshes(meshes):
    """sky boxes / domes: a mesh whose box holds every other mesh's and is clearly wider than them (1.3x), with few
    triangles (a 12-triangle cube, a 420-triangle hemisphere) - it would roof the arena and spoil the floor / fit"""
    boxes = []
    for m in meshes:
        P = [v[0] for v in m['verts']]
        boxes.append(([min(p[i] for p in P) for i in range(3)], [max(p[i] for p in P) for i in range(3)])
                     if P else None)
    total = sum(len(m['tris']) for m in meshes)
    out = []
    for k, m in enumerate(meshes):
        if boxes[k] is None or len(m['tris']) > max(24, 0.35 * total):
            continue
        rest = [b for j, b in enumerate(boxes) if j != k and b is not None]
        if not rest:
            continue
        rl = [min(b[0][i] for b in rest) for i in range(3)]
        rh = [max(b[1][i] for b in rest) for i in range(3)]
        l, h = boxes[k]
        tol = [0.05 * (rh[i] - rl[i]) for i in range(3)]
        holds = all(l[i] <= rl[i] + tol[i] and h[i] >= rh[i] - tol[i] for i in range(3))
        wider = max(h[0] - l[0], h[1] - l[1]) >= 1.3 * max(rh[0] - rl[0], rh[1] - rl[1]) or len(m['tris']) <= 24
        if holds and wider:
            out.append(m)
    return out


def floor_height(meshes):
    """the paving: area-weighted median height of the near-level faces (within ~14 degrees, either winding)"""
    fl = []
    for n, ctr in face_list(meshes):
        ln = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
        if ln > 0 and abs(n[2]) / ln > 0.97:
            fl.append((ctr[2], ln))
    if not fl:
        return None
    fl.sort()
    half, acc = sum(w for _, w in fl) / 2, 0.0
    for z, w in fl:
        acc += w
        if acc >= half:
            return z
    return fl[-1][0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('glb')
    ap.add_argument('arena', help='the retail .pao of the arena being replaced')
    ap.add_argument('out', help='new .pao (the .ptx is written beside it)')
    ap.add_argument('--template', required=True, help='retail stride-24 prop .pax (e.g. mesh/misc/rocks/rock_gen_01.pax)')
    ap.add_argument('--name', help='texture name prefix (default: GLB file name)')
    ap.add_argument('--scale', type=float, default=1.0)
    ap.add_argument('--scale-arena', type=float, default=1.0, help='extra factor on the fit to the arena bounds')
    ap.add_argument('--manual', action='store_true', help='place with --fit / --at / --ground-z, not the arena')
    ap.add_argument('--fit', type=float, help='(manual) larger of the x / y sizes in units')
    ap.add_argument('--at', default='0,0', help='(manual) x,y of the scene centre (arena grid: cell n = 2n units)')
    ap.add_argument('--ground-z', type=float, default=0.0, help='(manual) z the scene floor is moved to')
    ap.add_argument('--floor-pct', type=float, help='(manual) height percentile taken as the floor (default: the '
                    'median height of the level faces)')
    ap.add_argument('--floor-z', type=float, help='(manual) the floor height in the GLB (after --up / --rot; levels '
                    'whose floor detection picks stands or a roof)')
    ap.add_argument('--keep-sky', action='store_true', help='keep sky boxes / domes (dropped by default)')
    ap.add_argument('--yaw', type=float, default=0.0, help='degrees about z after squaring to the grid (--level)')
    ap.add_argument('--spin', type=float, default=0.0,
                    help='degrees about the vertical through the scene centre, applied last (after --tilt)')
    ap.add_argument('--cut-above', type=float, help='drop triangles more than this many units over the floor (roofs)')
    ap.add_argument('--tilt', help='x,y degrees of lean correction, last (+x moves the top toward -y, +y toward +x)')
    ap.add_argument('--cell', type=float, default=16.0, help='culling column size in units')
    ap.add_argument('--up', default='+z', choices=['+x', '-x', '+y', '-y', '+z', '-z'],
                    help='the GLB axis that is really up, after the usual Y-up -> Z-up turn (scans exported with the '
                         'wrong up: look at the model from the top / side first)')
    ap.add_argument('--rot', help='x,y,z degrees, applied after --up')
    ap.add_argument('--level', action='store_true', help='experimental: estimate the floor / main wall and level them')
    ap.add_argument('--pms', help='also write collision (.pms) here - a heightfield of the walkable surfaces')
    ap.add_argument('--pms-arena', help="the arena's retail .pms: its area is covered too")
    ap.add_argument('--pms-step', type=float, default=1.0, help='collision grid spacing in units (retail: 2)')
    ap.add_argument('--pms-surface', default='bottom', choices=['bottom', 'top'],
                    help='which walkable surface per point: the lowest (floors under overhangs) or the highest')
    ap.add_argument('--grd', help="also write the level's battle grid (no-move layer) here")
    ap.add_argument('--grd-flat', type=float, default=0.75, help='largest height range inside a playable cell')
    ap.add_argument('--grd-starts', nargs='*', help="encounter start .grd files to check against the new grid")
    ap.add_argument('--grd-starts-out', help='remake the --grd-starts files for the new grid into this folder')
    ap.add_argument('--flip-v', action='store_true')
    ap.add_argument('--color', type=lambda s: [float(x) for x in s.split(',')])
    ap.add_argument('--max-texture', type=int, default=1024, help='largest texture side (power of two)')
    args = ap.parse_args()
    args.pose_skins = True                              # a level is static: skinned parts go where their bones put them
    args.model_name = args.name or glb2pax.safe_name(os.path.splitext(os.path.basename(args.glb))[0], 'level')
    args.texture = None
    args.center = True
    args.ground = True
    fit = args.fit
    args.fit = None                                     # glb2pax fits the largest of x, y, z; levels fit x / y
    t0 = time.time()
    gltf, blob, meshes, _ = glb2pax.load_meshes(args.glb, args)
    if not args.keep_sky:
        sky = sky_meshes(meshes)
        if sky:
            print(f'  dropped {len(sky)} sky box / dome mesh(es): {sum(len(m["tris"]) for m in sky)} triangles')
            meshes = [m for m in meshes if not any(m is s for s in sky)]
    raw_lo_z = None
    if args.floor_z is not None:                        # heights as the GLB has them after the up turn
        R0 = axis_rotation(args)
        raw_lo_z = min((mul(R0, v[0]) if R0 is not None else v[0])[2] for m in meshes for v in m['verts'])
    trust_up = args.up != '+z' or bool(args.rot)        # the user said which way is up: only take out the tilt
    for step in ('axis', 'level', 'square', 'tilt'):
        R = axis_rotation(args) if step == 'axis' else             (level_rotation(meshes, trust_up) if args.level else None) if step == 'level' else             (square_rotation(meshes, args.yaw) if args.level or args.yaw else None) if step == 'square' else             tilt_rotation(args.tilt) if args.tilt else None
        if R is not None:
            for m in meshes:
                m['verts'] = [(mul(R, v[0]), mul(R, v[1])) + tuple(v[2:]) for v in m['verts']]
    # centre on x / y, lowest point at z = 0
    g = [v[0] for m in meshes for v in m['verts']]
    glo = [min(p[i] for p in g) for i in range(3)]
    ghi = [max(p[i] for p in g) for i in range(3)]
    c = ((glo[0] + ghi[0]) / 2, (glo[1] + ghi[1]) / 2, glo[2])
    for m in meshes:
        m['verts'] = [((v[0][0] - c[0], v[0][1] - c[1], v[0][2] - c[2]),) + tuple(v[1:]) for v in m['verts']]
    if args.spin:
        # last: a pure turn about the vertical through the scene centre (--at) - squares the scene to the battle grid
        # without touching the levelling / tilt; the floor and heights stay as they are
        R = [[math.cos(math.radians(args.spin)), -math.sin(math.radians(args.spin)), 0],
             [math.sin(math.radians(args.spin)), math.cos(math.radians(args.spin)), 0], [0, 0, 1]]
        for m in meshes:
            m['verts'] = [(mul(R, v[0]), mul(R, v[1])) + tuple(v[2:]) for v in m['verts']]
        print(f'  spin: {args.spin:g} degrees about the vertical through the scene centre')
    arena = open(args.arena, 'rb').read()
    pct = lambda zs, q: sorted(zs)[int(q * (len(zs) - 1))]
    if args.manual:
        k = fit / max(ghi[0] - glo[0], ghi[1] - glo[1]) if fit else 1.0
        ax, ay = (float(v) for v in args.at.split(','))
        # the scene's floor = the median height of its level faces (the paving), else a low percentile of its heights
        fz = floor_height(meshes) if args.floor_pct is None else None
        if args.floor_z is not None:
            fz = args.floor_z - raw_lo_z                # scene heights start at its lowest point (without --level)
        if fz is None:
            fz = pct([v[0][2] for m in meshes for v in m['verts']], args.floor_pct or 0.05)
        print(f'  floor at scene height {fz:.2f} (of {ghi[2] - glo[2]:.2f}) -> z {args.ground_z}')
        az = args.ground_z - fz * k
    else:
        # as Protius' GLB -> .pao (glbpax.cpp): uniform scale to the arena's largest extent, centred on the arena,
        # floors matched by the 20th-percentile vertex height - so the new visuals sit on the arena's collision
        ap_ = xrnd.level_positions(arena)
        alo = [min(p[i] for p in ap_) for i in range(3)]
        ahi = [max(p[i] for p in ap_) for i in range(3)]
        g = [v[0] for m in meshes for v in m['verts']]
        glo = [min(p[i] for p in g) for i in range(3)]
        ghi = [max(p[i] for p in g) for i in range(3)]
        k = max(ahi[i] - alo[i] for i in range(3)) / max(ghi[i] - glo[i] for i in range(3)) * args.scale_arena
        ax, ay = (alo[0] + ahi[0]) / 2, (alo[1] + ahi[1]) / 2
        az = pct([p[2] for p in ap_], 0.2) - pct([p[2] for p in g], 0.2) * k
        print(f'  arena bounds {[round(v, 1) for v in alo]} .. {[round(v, 1) for v in ahi]}, scale {k:.4g}')
    for m in meshes:
        m['verts'] = [((v[0][0] * k + ax, v[0][1] * k + ay, v[0][2] * k + az),) + tuple(v[1:])
                      for v in m['verts']]
    if args.cut_above is not None:
        # interiors: cut the roof / upper galleries off at --cut-above over the floor so the battle camera sees in.
        # Triangles crossing the cut are CLIPPED (walls keep their lower part; new vertices interpolated along the
        # edges, shared between neighbours), triangles wholly above it are dropped, unused vertices removed
        top = (args.ground_z if args.manual else az) + args.cut_above

        def lerp(va, vb, t):
            out = []
            for ea, eb in zip(va, vb):
                if ea is None or eb is None or not isinstance(ea, (tuple, list)):
                    out.append(ea)
                else:
                    out.append(tuple(x + (y - x) * t for x, y in zip(ea, eb)))
            return tuple(out)

        kept_meshes, dropped, clipped = [], 0, 0
        for m in meshes:
            V = list(m['verts'])
            cut = {}

            def edge_vert(i, j):
                k = (min(i, j), max(i, j))
                if k not in cut:
                    za, zb = V[k[0]][0][2], V[k[1]][0][2]
                    t = (top - za) / (zb - za) if zb != za else 0.0
                    cut[k] = len(V)
                    V.append(lerp(V[k[0]], V[k[1]], t))
                return cut[k]

            tris = []
            for t in m['tris']:
                inside = [V[i][0][2] <= top for i in t]
                if all(inside):
                    tris.append(t)
                    continue
                if not any(inside):
                    dropped += 1
                    continue
                clipped += 1
                poly = []
                for q in range(3):                      # Sutherland-Hodgman against z <= top
                    i, j = t[q], t[(q + 1) % 3]
                    ii, ij = inside[q], inside[(q + 1) % 3]
                    if ii:
                        poly.append(i)
                    if ii != ij:
                        poly.append(edge_vert(i, j))
                for q in range(1, len(poly) - 1):
                    tris.append((poly[0], poly[q], poly[q + 1]))
            if not tris:
                continue
            remap, nvs = {}, []
            for t in tris:
                for i in t:
                    if i not in remap:
                        remap[i] = len(nvs)
                        nvs.append(V[i])
            m['verts'], m['tris'] = nvs, [tuple(remap[i] for i in t) for t in tris]
            kept_meshes.append(m)
        meshes[:] = kept_meshes
        print(f'  cut at z {top:.2f}: {clipped} triangles clipped, {dropped} above it dropped')
    nv = sum(len(m['verts']) for m in meshes)
    nt = sum(len(m['tris']) for m in meshes)
    print(f'{args.glb}: {len(meshes)} material(s), {nv} vertices, {nt} triangles ({time.time() - t0:.1f} s)')
    if args.pms or args.grd:
        # collision: a heightfield of the highest walkable surface (as the retail .pms - a 2-unit grid), over the
        # scene and the arena's old collision area; points with no surface get --ground-z
        import pms
        g = [v[0] for m in meshes for v in m['verts']]
        lo = [min(p[i] for p in g) for i in range(2)]
        hi = [max(p[i] for p in g) for i in range(2)]
        if args.pms_arena and os.path.exists(args.pms_arena):
            b = pms.read_pms(open(args.pms_arena, 'rb').read())['bounds']
            lo = [min(lo[0], b[0]), min(lo[1], b[1])]
            hi = [max(hi[0], b[3]), max(hi[1], b[4])]
        step = args.pms_step
        x0, y0 = math.floor(lo[0]), math.floor(lo[1])
        nx, ny = int(math.ceil((hi[0] - x0) / step)), int(math.ceil((hi[1] - y0) / step))
        H = pms.surface_heights(meshes, x0, y0, nx, ny, step, top=args.pms_surface == 'top')
        holes = sum(1 for row in H for z in row if z is None)
        if args.pms:
            data = pms.build_pms_heightfield(H, x0, y0, step, fill=args.ground_z)
            P = pms.read_pms(data)
            os.makedirs(os.path.dirname(os.path.abspath(args.pms)), exist_ok=True)
            open(args.pms, 'wb').write(data)
            print(f'wrote {args.pms} ({len(data)} bytes): {nx + 1}x{ny + 1} points every {step} units from '
                  f'({x0}, {y0}), {holes} without a surface (z {args.ground_z}), {len(P["T"])} triangles, '
                  f'{P["nx"]}x{P["ny"]} cells')
        if args.grd:
            # battle grid: playable where the whole cell has scanned ground and is fairly flat; MapCenter (the
            # battle camera's centre) at the middle of the playable cells
            import grd
            ok = grd.walkable_cells(H, x0, y0, step, flat=args.grd_flat)
            cells = [[0 if ok[y][x] else 1 for x in range(32)] for y in range(32)]
            free = [(x, y) for y in range(32) for x in range(32) if ok[y][x]]
            if not free:
                raise SystemExit('no playable cells: check placement (cells span world 0..64)')
            mx = sum(x for x, _ in free) / len(free)
            my = sum(y for _, y in free) / len(free)
            cx, cy = min(free, key=lambda c: (c[0] - mx) ** 2 + (c[1] - my) ** 2)
            cells[cy][cx] |= 2
            os.makedirs(os.path.dirname(os.path.abspath(args.grd)), exist_ok=True)
            open(args.grd, 'wb').write(grd.write_grd(['NoMoveNoCursor', 'MapCenter'], cells))
            print(f'wrote {args.grd}: {len(free)} playable cells of 30x30, MapCenter cell ({cx}, {cy})')
            for y in range(31, -1, -1):
                print('    ' + ''.join('C' if cells[y][x] & 2 else '#' if cells[y][x] & 1 else '.' for x in range(32)))
            if args.grd_starts_out:                     # encounter start grids remade for this level
                os.makedirs(args.grd_starts_out, exist_ok=True)
                main = grd.largest_region(ok)
                print(f'  largest connected playable region: {len(main)} cells')
                for path in args.grd_starts or []:
                    layers, sc = grd.relocate_starts(grd.read_grd(open(path, 'rb').read()), ok, (cx, cy))
                    dst = os.path.join(args.grd_starts_out, os.path.basename(path))
                    open(dst, 'wb').write(grd.write_grd(layers, sc))
                    teams = {n: sum(1 for y in range(32) for x in range(32) if sc[y][x] >> b & 1)
                             for b, n in enumerate(layers) if n.startswith('Start')}
                    print(f'  wrote {dst}: {teams}')
                    for y in range(31, -1, -1):
                        row = ''
                        for x in range(32):
                            t = [n[-1] for b, n in enumerate(layers) if n.startswith('Start') and sc[y][x] >> b & 1]
                            row += t[0] if t else 'C' if (x, y) == (cx, cy) else '.' if ok[y][x] else '#'
                        print('      ' + row)
            for path in [] if args.grd_starts_out else args.grd_starts or []:
                g = grd.read_grd(open(path, 'rb').read())
                bad = [(x, y, g['layers'][b]) for y in range(32) for x in range(32) for b in range(len(g['layers']))
                       if g['cells'][y][x] >> b & 1 and g['layers'][b].startswith('FStart') and not ok[y][x]]
                print(f'  {os.path.basename(path)}: ' + (f'{len(bad)} start cells blocked: {bad[:8]}' if bad
                                                         else 'all start cells playable'))
    pao, st = xrnd.build_level_pao(open(args.template, 'rb').read(), open(args.arena, 'rb').read(), meshes,
                                   cell=args.cell)
    lo, hi = st['bounds']
    print(f'  {st["submeshes"]} sub-meshes in {st["columns"]} columns, vertex buffers {st["vbs"]}, stride '
          f'{st["stride"]}, bounds {[round(v, 1) for v in lo]} .. {[round(v, 1) for v in hi]}')
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    open(args.out, 'wb').write(pao)
    print(f'wrote {args.out} ({len(pao)} bytes)')
    import ptx
    tex = glb2pax.build_textures(gltf, blob, args.glb, meshes, args.max_texture)
    ptx_path = os.path.splitext(args.out)[0] + '.ptx'
    data = ptx.build_ptx(tex)
    open(ptx_path, 'wb').write(data)
    print(f'wrote {ptx_path} ({len(data)} bytes, {time.time() - t0:.1f} s total)')


if __name__ == '__main__':
    main()

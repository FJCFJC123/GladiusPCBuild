"""Gladius battle grid layers (.grd) - reader / writer.

Layout (loader 0x56C40): u32 version 2, u32 layers used, 30 layer names of 32 bytes ('E' in unused slots), then
32 x 32 u32 cells stored row by row in y (file[y * 32 + x]; bit n = in layer n). The loader looks the layers up by
name and ORs engine flags into the battle grid 0x39C2C0[x * 32 + y] (transposed): NoMove 1, NoMoveNoCursor 3,
KnockbackOnly 4, Gate 8, NoLargeUnits 0x10, MapCenter 0x20, LineOfSightPass 0x40, NoTeleport 0x80. Other layer
names (FStart1.. in an encounter's own .grd) are read by the encounter setup. A cell is 2 units: cell c spans world
2c - 1 .. 2c + 1. An encounter lists several GRIDFILEs (the level's NoMove grid, then its start positions).
Names carry an 'F' prefix in the file (FNoMoveNoCursor, FMapCenter).
"""
import struct

SIZE = 32


def read_grd(data):
    ver, n = struct.unpack_from('<II', data, 0)
    names = [data[8 + 32 * i:8 + 32 * i + 32].split(b'\0')[0].decode('latin1') for i in range(30)]
    cells = struct.unpack_from('<1024I', data, 8 + 960)
    return {'version': ver, 'layers': names[:n], 'cells': [list(cells[y * SIZE:(y + 1) * SIZE]) for y in range(SIZE)]}


def write_grd(layers, cells):
    """layers: names without the F prefix, e.g. ['NoMoveNoCursor', 'MapCenter']; cells[y][x] = bit mask"""
    out = bytearray(struct.pack('<II', 2, len(layers)))
    for i in range(30):
        nm = ('F' + layers[i]).encode('latin1') if i < len(layers) else b'E'
        out += nm[:31] + b'\0' * (32 - len(nm[:31]))
    for y in range(SIZE):
        out += struct.pack(f'<{SIZE}I', *cells[y])
    return bytes(out)


def walkable_cells(H, x0, y0, step, flat=0.75):
    """cells whose whole 2 x 2 unit square has scanned ground (no None in the heightfield H, sampled every `step`
    from x0, y0) varying by at most `flat` -> [[bool] * 32] * 32 indexed [y][x]; the outer ring stays blocked"""
    ok = [[False] * SIZE for _ in range(SIZE)]
    for cy in range(1, SIZE - 1):
        for cx in range(1, SIZE - 1):
            zs = []
            x_lo, x_hi, y_lo, y_hi = 2 * cx - 1, 2 * cx + 1, 2 * cy - 1, 2 * cy + 1
            j0, j1 = int(round((y_lo - y0) / step)), int(round((y_hi - y0) / step))
            i0, i1 = int(round((x_lo - x0) / step)), int(round((x_hi - x0) / step))
            good = 0 <= j0 and j1 < len(H) and 0 <= i0 and i1 < len(H[0])
            if good:
                for j in range(j0, j1 + 1):
                    for i in range(i0, i1 + 1):
                        z = H[j][i]
                        if z is None:
                            good = False
                            break
                        zs.append(z)
                    if not good:
                        break
            ok[cy][cx] = good and bool(zs) and max(zs) - min(zs) <= flat
    return ok


def largest_region(ok):
    """the biggest 4-connected set of playable cells -> set of (x, y)"""
    seen, best = set(), set()
    for y in range(SIZE):
        for x in range(SIZE):
            if not ok[y][x] or (x, y) in seen:
                continue
            region, todo = set(), [(x, y)]
            seen.add((x, y))
            while todo:
                cx, cy = todo.pop()
                region.add((cx, cy))
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < SIZE and 0 <= ny < SIZE and ok[ny][nx] and (nx, ny) not in seen:
                        seen.add((nx, ny))
                        todo.append((nx, ny))
            if len(region) > len(best):
                best = region
    return best


def relocate_starts(g, ok, center):
    """an encounter start grid (read_grd) remade for a new level: every FStartN layer becomes one compact patch of
    as many cells on the level's largest playable region, the teams spread far apart (farthest-point seeds, open
    cells preferred); its own NoMove layers are emptied (the level grid has them) and MapCenter goes to `center`.
    -> (layers without the F prefix, cells)"""
    region = largest_region(ok)
    layers = [n[1:] if n.startswith('F') else n for n in g['layers']]
    want = {b: sum(1 for y in range(SIZE) for x in range(SIZE) if g['cells'][y][x] >> b & 1)
            for b, n in enumerate(layers) if n.startswith('Start')}
    cells = [[0] * SIZE for _ in range(SIZE)]
    if center is not None and 'MapCenter' in layers:
        cells[center[1]][center[0]] |= 1 << layers.index('MapCenter')
    open_ = lambda c: sum((c[0] + dx, c[1] + dy) in region for dx in (-1, 0, 1) for dy in (-1, 0, 1))
    core = [c for c in region if open_(c) == 9] or list(region)
    teams = sorted(want, key=lambda b: layers[b])
    seeds = []
    for b in teams:
        if not seeds:                                   # first team: the open cell farthest from the region's middle
            mx = sum(c[0] for c in region) / len(region)
            my = sum(c[1] for c in region) / len(region)
            seeds.append(max(core, key=lambda c: (c[0] - mx) ** 2 + (c[1] - my) ** 2))
        else:
            seeds.append(max(core, key=lambda c: min((c[0] - s[0]) ** 2 + (c[1] - s[1]) ** 2 for s in seeds)))
    used = set()
    for b, seed in zip(teams, seeds):
        patch, todo = [], [seed]                        # grow outward from the seed, nearest cells first
        cand = sorted((c for c in region if c not in used),
                      key=lambda c: (c[0] - seed[0]) ** 2 + (c[1] - seed[1]) ** 2)
        for c in cand:
            if len(patch) >= want[b]:
                break
            if not patch or any((c[0] + dx, c[1] + dy) in patch for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                patch.append(c)
        for x, y in patch:
            cells[y][x] |= 1 << b
            used.add((x, y))
    return layers, cells

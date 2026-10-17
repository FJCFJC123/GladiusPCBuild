"""Converted-character preview: runs the retarget (as tools/glbchars.py would for an entry of glbchars.json) and
renders the result in the Gladius bind pose - front (from -y, the way characters face) and side (from +x) views,
shaded by height; the Gladius head / neck / feet joints marked in red. For checking facing, crossed limbs, head pitch.
    python tools/glbcharview.py <id> [<id> ...] [--out folder]
"""
import argparse
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import retarget  # noqa: E402

BEC = os.path.join(os.path.dirname(TOOLS), 'bec_unpacked', 'data', 'characters')


def render(meshes, joints, path, S=420):
    T = np.concatenate([m['pos'][m['tris']] for m in meshes])
    lo, hi = T.reshape(-1, 3).min(0), T.reshape(-1, 3).max(0)
    span = max(hi - lo) * 1.02
    im = Image.new('RGB', (2 * S, S), (20, 20, 28))
    dr = ImageDraw.Draw(im)
    for view, (ix, iz_depth, ox, sign) in enumerate([(0, 1, 0, 1), (1, 0, S, -1)]):
        # front: x across, looking along +y (from -y): nearer = smaller y; side: y across, from +x
        order = np.argsort(-T[:, :, iz_depth].mean(1) * (1 if view == 0 else sign))
        cx = (lo[ix] + hi[ix]) / 2
        for k in order:
            t = T[k]
            pts = [(ox + S / 2 + (p[ix] - cx) / span * S, S - 4 - (p[2] - lo[2]) / span * S) for p in t]
            h = (t[:, 2].mean() - lo[2]) / max(1e-6, hi[2] - lo[2])
            n = np.cross(t[1] - t[0], t[2] - t[0])
            lit = abs(n[iz_depth]) / (np.linalg.norm(n) + 1e-9)
            dr.polygon(pts, fill=(int(60 + 150 * lit * h + 40), int(60 + 150 * lit * (1 - h)), int(70 + 90 * lit)))
        for name, p in joints.items():
            x, y = ox + S / 2 + (p[ix] - cx) / span * S, S - 4 - (p[2] - lo[2]) / span * S
            dr.ellipse((x - 3, y - 3, x + 3, y + 3), fill=(255, 40, 40))
    dr.text((6, 6), 'front (-y)', fill=(200, 200, 200))
    dr.text((S + 6, 6), 'side (+x; face to the left)', fill=(200, 200, 200))
    im.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ids', nargs='+')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(TOOLS), 'build', 'glbchars'))
    ap.add_argument('--src', default=os.path.join(os.path.expanduser('~'), 'Desktop', 'GLBChars'))
    args = ap.parse_args()
    cfg = json.load(open(os.path.join(TOOLS, 'glbchars.json'), encoding='utf-8'))
    every = cfg['characters'] + cfg.get('removed', [])
    for i in args.ids:
        e = next(x for x in every if x['id'] == i)
        cls = e['class'].lower()
        tpl = open(os.path.join(BEC, cls, cls + '.pax'), 'rb').read()
        a = e.get('args', [])
        retarget.REDUCE.update(drop=a[a.index('--drop-material') + 1] if '--drop-material' in a else None,
                               max_verts=int(a[a.index('--max-verts') + 1]) if '--max-verts' in a else None)
        retarget.SHAPE['height'] = float(a[a.index('--height') + 1]) if '--height' in a else None
        if e.get('preset_json'):
            preset, roles = os.path.join(TOOLS, e['preset_json']), None
        else:
            import automap
            g = os.path.join(args.src, e['glb'])
            preset = 'auto'
            roles = json.load(open(os.path.join(TOOLS, e['map']))) if e.get('map') else \
                automap.automap(*automap.rig_hierarchy(g))[0]
        meshes, _, _, info = retarget.retarget(os.path.join(args.src, e['glb']), tpl, preset, None, roles)
        low = min(m['pos'][:, 2].min() for m in meshes)
        if abs(low) > 0.005 and 'zero' in info['joints'] and not retarget.SHAPE.get('height'):
            pelvis = info['joints']['zero'][2]
            meshes, _, _, info = retarget.retarget(os.path.join(args.src, e['glb']), tpl, preset,
                                                   info['scale'] * pelvis / (pelvis - low), roles)
        path = os.path.join(args.out, i + '_view.png')
        render(meshes, {k: np.array(v) for k, v in info['joints'].items()}, path)
        print(f'{i}: {path} (height {info["height"]:.2f})')


if __name__ == '__main__':
    main()

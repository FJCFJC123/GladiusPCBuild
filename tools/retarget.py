"""Retarget a skinned GLB with its own skeleton onto a Gladius character (skeleton + animations of a retail class).

    python tools/retarget.py model.glb bec_unpacked/data/characters/legionnaire/legionnaire.pax \
        game/data/characters/legionnaire/legionnaire.pax --preset auto

The Gladius animations (.pan) drive the template's bones by name: rotations on every bone, positions only on a few
(pelvis 'zero', shoulder blades / mass, capes, weapon mounts, face) - so limbs and spine keep the lengths in the
character's own SKEL. The model therefore keeps its proportions:
  1. scale: uniform, so its pelvis sits at the template's pelvis height (the pelvis position track sets it)
  2. skeleton: the template's bones (directions, bind rotations) with the model's segment lengths
  3. pose: each model joint is turned so its bone points along the matching Gladius bone (an A-pose becomes the
     Gladius T-pose) and moved onto the new joint position; the mesh follows through its own skin weights
  4. skin: the model's weights move to the mapped Gladius bones (some joints split along a Gladius chain)
Output: the character .pax (tools/glb2char.py) and a .ptx beside it with the GLB's base colour textures and the
template's tint defaults.
"""
import argparse
import io
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glb2char  # noqa: E402
import glb2pax  # noqa: E402
import ptx  # noqa: E402
import xchar  # noqa: E402
import xrnd  # noqa: E402

UP, FWD = (0.0, 0.0, 1.0), (0.0, -1.0, 0.0)          # Gladius: z up, characters face -y


def _side(d, s):
    return {k.replace('_X', s): (v.replace('_X', s) if isinstance(v, str) else
                                 tuple(x.replace('_X', s) if isinstance(x, str) else x for x in v))
            for k, v in d.items()}


def preset_corvus():
    """Advanced-Skeleton style rig (Root_M, BackA_M, ..., Shoulder_L, Hip_L) -> Gladius humanoid"""
    skin = {'DeformationSystem': 'zero', 'Root_M': 'zero', 'BackA_M': 'bodyLumbar1', 'BackB_M': 'bodyLumbar2',
            'Chest_M': ('bodyThoracic', 'bodyThoracic2'), 'Neck_M': ('bodyNeck', 'bodyNeck2'), 'Head_M': 'faceSkull',
            'Breast_L': 'bodyThoracic2', 'Breast_R': 'bodyThoracic2'}
    # bone directions: model joint -> (model 'to' joint, Gladius bone, Gladius 'to' bone or a direction)
    dirs = {'Root_M': ('BackA_M', 'zero', 'bodyLumbar1'), 'BackA_M': ('BackB_M', 'bodyLumbar1', 'bodyLumbar2'),
            'BackB_M': ('Chest_M', 'bodyLumbar2', 'bodyThoracic'), 'Chest_M': ('Neck_M', 'bodyThoracic', 'bodyNeck'),
            'Neck_M': ('Head_M', 'bodyNeck', 'faceSkull')}
    # joints whose only guide is an *_end joint follow their parent rigidly (end joints mark a bone's local axis, not
    # where it points)
    follow = {'DeformationSystem': 'Root_M', 'Breast_L': 'Chest_M', 'Breast_R': 'Chest_M', 'Head_M': 'Neck_M'}
    # Gladius segments whose length comes from the model: (Gladius from, Gladius to, model from, model to)
    seg = [('zero', 'bodyLumbar1', 'Root_M', 'BackA_M'), ('bodyLumbar1', 'bodyLumbar2', 'BackA_M', 'BackB_M'),
           ('bodyLumbar2', 'bodyThoracic', 'BackB_M', 'Chest_M'), ('bodyThoracic', 'bodyNeck', 'Chest_M', 'Neck_M'),
           ('bodyNeck', 'faceSkull', 'Neck_M', 'Head_M')]
    for s in ('_L', '_R'):
        skin.update(_side({'Scapula_X': 'armShoulderBlade_X', 'Shoulder_X': 'armShoulder_X', 'Elbow_X': 'armElbow_X',
                           'Wrist_X': 'armWrist_X', 'IndexFinger1_X': 'fingerInRoot_X',
                           'IndexFinger2_X': 'fingerInMid_X', 'RingFinger1_X': 'fingerRingRoot_X',
                           'RingFinger2_X': 'fingerRingMid_X', 'ThumbFinger1_X': 'fingerThRoot_X',
                           'ThumbFinger2_X': 'fingerThMid_X', 'Hip_X': 'legHip_X', 'Knee_X': 'legKnee_X',
                           'Ankle_X': 'legAnkle_X', 'MiddleToe1_X': 'legBall_X'}, s))
        dirs.update(_side({
            'Scapula_X': ('Shoulder_X', 'armShoulderBlade_X', 'armShoulder_X'),
            'Shoulder_X': ('Elbow_X', 'armShoulder_X', 'armElbow_X'),
            'Elbow_X': ('Wrist_X', 'armElbow_X', 'armWrist_X'),
            'Wrist_X': ('RingFinger1_X', 'armWrist_X', 'fingerRingRoot_X'),
            'IndexFinger1_X': ('IndexFinger2_X', 'fingerInRoot_X', 'fingerInMid_X'),
            'RingFinger1_X': ('RingFinger2_X', 'fingerRingRoot_X', 'fingerRingMid_X'),
            'ThumbFinger1_X': ('ThumbFinger2_X', 'fingerThRoot_X', 'fingerThMid_X'),
            'Hip_X': ('Knee_X', 'legHip_X', 'legKnee_X'), 'Knee_X': ('Ankle_X', 'legKnee_X', 'legAnkle_X'),
            'Ankle_X': ('MiddleToe1_X', 'legAnkle_X', 'legBall_X')}, s))
        follow.update(_side({'IndexFinger2_X': 'IndexFinger1_X', 'RingFinger2_X': 'RingFinger1_X',
                             'ThumbFinger2_X': 'ThumbFinger1_X', 'MiddleToe1_X': 'Ankle_X'}, s))
        seg += [tuple(x.replace('_X', s) for x in t) for t in (
            ('armShoulderBlade_X', 'armShoulder_X', 'Scapula_X', 'Shoulder_X'),
            ('armShoulder_X', 'armElbow_X', 'Shoulder_X', 'Elbow_X'),
            ('armElbow_X', 'armWrist_X', 'Elbow_X', 'Wrist_X'),
            ('zero', 'legHip_X', 'Root_M', 'Hip_X'), ('legHip_X', 'legKnee_X', 'Hip_X', 'Knee_X'),
            ('legKnee_X', 'legAnkle_X', 'Knee_X', 'Ankle_X'), ('legAnkle_X', 'legBall_X', 'Ankle_X', 'MiddleToe1_X'))]
    return {'skin': skin, 'dirs': dirs, 'follow': follow, 'segments': seg, 'pelvis': ('Root_M', 'zero'),
            'feet': [('Ankle_L', 'MiddleToe1_L'), ('Ankle_R', 'MiddleToe1_R')]}


def preset_mixamo(pre='mixamorig:'):
    """Mixamo rig (mixamorig:Hips, Spine/Spine1/Spine2, Neck, Head, LeftShoulder/LeftArm/LeftForeArm/LeftHand, three
    segment fingers without a pinky, LeftUpLeg/LeftLeg/LeftFoot/LeftToeBase) -> Gladius humanoid"""
    m = lambda n: pre + n
    skin = {m('Hips'): 'zero', m('Spine'): 'bodyLumbar1', m('Spine1'): 'bodyLumbar2',
            m('Spine2'): ('bodyThoracic', 'bodyThoracic2'), m('Neck'): ('bodyNeck', 'bodyNeck2'), m('Head'): 'faceSkull'}
    dirs = {m('Hips'): (m('Spine'), 'zero', 'bodyLumbar1'), m('Spine'): (m('Spine1'), 'bodyLumbar1', 'bodyLumbar2'),
            m('Spine1'): (m('Spine2'), 'bodyLumbar2', 'bodyThoracic'), m('Spine2'): (m('Neck'), 'bodyThoracic', 'bodyNeck'),
            m('Neck'): (m('Head'), 'bodyNeck', 'faceSkull')}
    follow = {m('Head'): m('Neck')}
    seg = [('zero', 'bodyLumbar1', m('Hips'), m('Spine')), ('bodyLumbar1', 'bodyLumbar2', m('Spine'), m('Spine1')),
           ('bodyLumbar2', 'bodyThoracic', m('Spine1'), m('Spine2')), ('bodyThoracic', 'bodyNeck', m('Spine2'), m('Neck')),
           ('bodyNeck', 'faceSkull', m('Neck'), m('Head'))]
    fingers = {'Thumb': 'Th', 'Index': 'In', 'Middle': 'Mid', 'Ring': 'Ring'}
    for side, s in (('Left', '_L'), ('Right', '_R')):
        k = lambda n: m(side + n)
        skin.update({k('Shoulder'): 'armShoulderBlade' + s, k('Arm'): 'armShoulder' + s, k('ForeArm'): 'armElbow' + s,
                     k('Hand'): 'armWrist' + s, k('UpLeg'): 'legHip' + s, k('Leg'): 'legKnee' + s,
                     k('Foot'): 'legAnkle' + s, k('ToeBase'): 'legBall' + s})
        dirs.update({k('Shoulder'): (k('Arm'), 'armShoulderBlade' + s, 'armShoulder' + s),
                     k('Arm'): (k('ForeArm'), 'armShoulder' + s, 'armElbow' + s),
                     k('ForeArm'): (k('Hand'), 'armElbow' + s, 'armWrist' + s),
                     k('Hand'): (k('HandMiddle1'), 'armWrist' + s, 'fingerMidRoot' + s),
                     k('UpLeg'): (k('Leg'), 'legHip' + s, 'legKnee' + s),
                     k('Leg'): (k('Foot'), 'legKnee' + s, 'legAnkle' + s),
                     k('Foot'): (k('ToeBase'), 'legAnkle' + s, 'legBall' + s)})
        follow[k('ToeBase')] = k('Foot')
        for f, g in fingers.items():
            skin.update({k(f'Hand{f}1'): f'finger{g}Root{s}', k(f'Hand{f}2'): f'finger{g}Mid{s}',
                         k(f'Hand{f}3'): f'finger{g}Tip{s}'})
            dirs.update({k(f'Hand{f}1'): (k(f'Hand{f}2'), f'finger{g}Root{s}', f'finger{g}Mid{s}'),
                         k(f'Hand{f}2'): (k(f'Hand{f}3'), f'finger{g}Mid{s}', f'finger{g}Tip{s}')})
            follow[k(f'Hand{f}3')] = k(f'Hand{f}2')
        seg += [('armShoulderBlade' + s, 'armShoulder' + s, k('Shoulder'), k('Arm')),
                ('armShoulder' + s, 'armElbow' + s, k('Arm'), k('ForeArm')),
                ('armElbow' + s, 'armWrist' + s, k('ForeArm'), k('Hand')),
                ('zero', 'legHip' + s, m('Hips'), k('UpLeg')), ('legHip' + s, 'legKnee' + s, k('UpLeg'), k('Leg')),
                ('legKnee' + s, 'legAnkle' + s, k('Leg'), k('Foot')),
                ('legAnkle' + s, 'legBall' + s, k('Foot'), k('ToeBase'))]
    return {'skin': skin, 'dirs': dirs, 'follow': follow, 'segments': seg, 'pelvis': (m('Hips'), 'zero'),
            'feet': [(m('LeftFoot'), m('LeftToeBase')), (m('RightFoot'), m('RightToeBase'))]}


PRESETS = {'corvus': preset_corvus, 'mixamo': preset_mixamo}


def rot_between(a, b):
    a = np.asarray(a, float) / np.linalg.norm(a)
    b = np.asarray(b, float) / np.linalg.norm(b)
    v, c = np.cross(a, b), float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3) if c > 0 else np.diag([1.0, -1.0, -1.0])
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K * (1 / (1 + c))


def zup(v):
    v = np.asarray(v, float)
    return np.stack([v[..., 0], -v[..., 2], v[..., 1]], -1)


def mat_to_quat(R):
    """rotation matrices (..., 3, 3) -> unit quaternions (..., 4) as w, x, y, z"""
    R = np.asarray(R, float)
    q = np.empty(R.shape[:-2] + (4,))
    t = R[..., 0, 0] + R[..., 1, 1] + R[..., 2, 2]
    q[..., 0] = np.sqrt(np.maximum(0, 1 + t)) / 2
    q[..., 1] = np.sqrt(np.maximum(0, 1 + R[..., 0, 0] - R[..., 1, 1] - R[..., 2, 2])) / 2
    q[..., 2] = np.sqrt(np.maximum(0, 1 - R[..., 0, 0] + R[..., 1, 1] - R[..., 2, 2])) / 2
    q[..., 3] = np.sqrt(np.maximum(0, 1 - R[..., 0, 0] - R[..., 1, 1] + R[..., 2, 2])) / 2
    q[..., 1] = np.copysign(q[..., 1], R[..., 2, 1] - R[..., 1, 2])
    q[..., 2] = np.copysign(q[..., 2], R[..., 0, 2] - R[..., 2, 0])
    q[..., 3] = np.copysign(q[..., 3], R[..., 1, 0] - R[..., 0, 1])
    return q / np.linalg.norm(q, axis=-1, keepdims=True)


def qmul(a, b):
    w1, x1, y1, z1 = np.moveaxis(a, -1, 0)
    w2, x2, y2, z2 = np.moveaxis(b, -1, 0)
    return np.stack([w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2, w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
                     w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2, w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2], -1)


def dq_blend(w, Rs, Ts, idx, v, n):
    """dual-quaternion skinning of the one-time re-pose: blends the joint rotations themselves, so a joint turned far
    (A-pose -> T-pose shoulders) keeps its volume"""
    qr = mat_to_quat(Rs)                                 # per joint
    qd = 0.5 * qmul(np.concatenate([np.zeros(Ts.shape[:-1] + (1,)), Ts], -1), qr)
    br, bd = qr[idx], qd[idx]                            # (n, k, 4)
    sign = np.sign(np.einsum('nkq,nq->nk', br, br[:, 0]))
    sign[sign == 0] = 1
    ww = (w * sign)[..., None]
    r, d = (ww * br).sum(1), (ww * bd).sum(1)
    ln = np.linalg.norm(r, axis=1, keepdims=True)
    r, d = r / np.maximum(ln, 1e-12), d / np.maximum(ln, 1e-12)
    rw, rv = r[:, :1], r[:, 1:]
    rot = lambda x: x + 2 * np.cross(rv, np.cross(rv, x) + rw * x)
    t = 2 * qmul(d, r * np.array([1, -1, -1, -1]))[:, 1:]
    return rot(v) + t, rot(n)


def load_rig(path, from_nodes=False):
    """-> (joint names, bind positions (z-up), primitives with joint indices) from the GLB's first skin"""
    gltf, blob = glb2pax.read_glb(path)
    skin = gltf['skins'][0]
    names = [gltf['nodes'][j].get('name', f'joint{j}') for j in skin['joints']]
    ibm = glb2char.acc(gltf, blob, skin['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)
    bind = np.array([np.linalg.inv(m)[:3, 3] for m in ibm])
    # broken inverse binds (pelvis / thighs at the origin): take the rest pose from the node hierarchy instead. Only when
    # joints the bone map uses sit at the origin - some rigs keep IK targets there on purpose.
    S = None
    at_origin = sum(1 for b in bind[1:] if np.abs(b).max() < 1e-6)
    if from_nodes:
        parent = {c: i for i, n in enumerate(gltf['nodes']) for c in n.get('children', [])}
        W = {}

        def world(i):
            if i not in W:
                m = np.array(glb2pax.node_matrix(gltf['nodes'][i]), float)
                W[i] = world(parent[i]) @ m if i in parent else m
            return W[i]
        S = np.array([world(j) @ ibm[k] for k, j in enumerate(skin['joints'])])
        bind = np.array([world(j)[:3, 3] for j in skin['joints']])
        print(f'  inverse bind matrices put {at_origin} joints at the origin: rest pose taken from the node '
              f'hierarchy')
    prims = []
    for n in gltf['nodes']:
        if 'mesh' not in n or 'skin' not in n:
            continue
        for p in gltf['meshes'][n['mesh']]['primitives']:
            at = p['attributes']
            mat = gltf['materials'][p['material']] if 'material' in p else {}
            tex = glb2pax.texture_name(gltf, mat) or mat.get('name') or 'white'
            t, _ = glb2pax.diffuse_ref(mat)
            img = glb2pax.image_bytes(gltf, blob, path, t) if t else None
            pos, nrm = glb2char.acc(gltf, blob, at['POSITION']), glb2char.acc(gltf, blob, at['NORMAL'])
            jj, ww = glb2char.acc(gltf, blob, at['JOINTS_0']).astype(int), glb2char.acc(gltf, blob, at['WEIGHTS_0'])
            if S is not None:                           # rest pose: blend of world x inverse bind per vertex
                M = np.einsum('nk,nkij->nij', ww / np.maximum(ww.sum(1, keepdims=True), 1e-9), S[jj])
                pos = np.einsum('nij,nj->ni', M[:, :3, :3], pos) + M[:, :3, 3]
                nrm = np.einsum('nij,nj->ni', M[:, :3, :3], nrm)
                nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
            # the engine finds textures by name across every loaded .ptx: prefix generic names ("image0") with the class
            tname = glb2pax.safe_name(os.path.splitext(tex)[0], 'tex')
            if REDUCE.get('prefix'):
                tname = (REDUCE['prefix'] + '_' + tname)[:31]
            prims.append({'texture': tname, 'image': img,
                          'material': mat.get('name') or '',
                          'color': (mat.get('pbrMetallicRoughness') or {}).get('baseColorFactor') or
                          ((mat.get('extensions') or {}).get('KHR_materials_pbrSpecularGlossiness') or {}).get(
                              'diffuseFactor'),
                          'col': glb2char.acc(gltf, blob, at['COLOR_0']) if 'COLOR_0' in at else None,
                          'pos': zup(pos), 'nrm': zup(nrm),
                          'uv': glb2char.acc(gltf, blob, at['TEXCOORD_0']),
                          'j': jj, 'w': ww,
                          'tris': glb2char.acc(gltf, blob, p['indices']).astype(int).reshape(-1, 3)})
    return names, zup(bind), reduce_prims(prims, REDUCE.get('drop'), REDUCE.get('max_verts'))


REDUCE = {}                                             # main(): --drop-material / --max-verts
SHAPE = {}                                              # main(): --height


def reduce_prims(prims, drop=None, max_verts=None):
    """drop parts by material, then simplify every part by the same share until about max_verts vertices remain;
    the new vertices take normal, UV and skin of the nearest original vertex (positions from the simplifier)"""
    import re
    if drop:
        kept = [p for p in prims if not re.search(drop, p['material'], re.I)]
        print(f'  dropped {len(prims) - len(kept)} part(s) by material /{drop}/: '
              f'{sum(len(p["pos"]) for p in prims) - sum(len(p["pos"]) for p in kept)} vertices')
        prims = kept
    total = sum(len(p['pos']) for p in prims)
    if not max_verts or total <= max_verts:
        return prims
    import fast_simplification
    from scipy.spatial import cKDTree
    keep = max_verts / total
    out = []
    for p in prims:
        if len(p['tris']) < 64:                         # tiny parts (pupils, buckles): as they are
            out.append(p)
            continue
        pts, tris = fast_simplification.simplify(p['pos'].astype(np.float32), p['tris'].astype(np.int64),
                                                 target_reduction=1.0 - keep)
        if len(tris) == 0:
            continue
        _, near = cKDTree(p['pos']).query(pts)
        q = dict(p)
        if p.get('col') is not None:
            q['col'] = p['col'][near]
        q.update(pos=np.asarray(pts, float), nrm=p['nrm'][near], uv=p['uv'][near], j=p['j'][near], w=p['w'][near],
                 tris=np.asarray(tris, int))
        out.append(q)
    print(f'  simplified {total} -> {sum(len(p["pos"]) for p in out)} vertices '
          f'({sum(len(p["tris"]) for p in prims)} -> {sum(len(p["tris"]) for p in out)} triangles)')
    return out


def retarget(glb, template, preset, scale=None, roles=None):
    """preset: a PRESETS name, or 'auto' (tools/automap.py; roles = a roles dict to use instead of detecting)"""
    if preset == 'auto':
        import automap
        if roles is None:
            roles, _ = automap.automap(*automap.rig_hierarchy(glb))
        P = automap.roles_to_preset(roles)
    elif preset.lower().endswith('.json'):              # a full preset from a file (creatures)
        import json
        raw = json.load(open(preset, encoding='utf-8'))
        tup = lambda x: tuple(x) if isinstance(x, list) else x
        P = {'skin': {k: tup(v) for k, v in raw['skin'].items()}, 'dirs': {k: tup(v) for k, v in raw['dirs'].items()},
             'follow': raw.get('follow', {}), 'segments': [tuple(s) for s in raw.get('segments', [])],
             'pelvis': tuple(raw['pelvis']), 'feet': [tuple(f) for f in raw.get('feet', [])]}
    else:
        P = PRESETS[preset]()
    names, bind, prims = load_rig(glb)
    J = {n: i for i, n in enumerate(names)}
    used = set(P['dirs']) | {v[0] for v in P['dirs'].values()} | {P['pelvis'][0]}

    def fit(bind, prims):
        """how far the mesh sits from its own joints: median vertex -> main joint distance / skeleton size, per part"""
        size = max(np.ptp(bind[[J[n] for n in used if n in J]], axis=0).max(), 1e-9)
        return [float(np.median(np.linalg.norm(p['pos'] - bind[p['j'][np.arange(len(p['j'])), p['w'].argmax(1)]],
                                               axis=1))) / size for p in prims]
    origin = sum(1 for n in used if n in J and J[n] > 0 and np.abs(bind[J[n]]).max() < 1e-6) >= 2
    f_ibm = fit(bind, prims)
    if origin or np.median(f_ibm) > 0.5:
        # broken inverse binds (joints at the origin, or a mesh nowhere near its joints): use the node hierarchy's rest pose
        # when it fits better
        n2, b2, p2 = load_rig(glb, from_nodes=True)
        if origin or np.median(fit(b2, p2)) < np.median(f_ibm):
            names, bind, prims = n2, b2, p2
    # parts nowhere near their joints in either pose (spare pieces parked away from the model)
    far = [k for k, f in enumerate(fit(bind, prims)) if f > 5.0]   # cartoon heads / hands reach ~2
    if far and len(far) < len(prims):
        print(f'  left out {len(far)} part(s) far from their joints: '
              f'{sorted({prims[k]["material"] or prims[k]["texture"] for k in far})}')
        prims = [p for k, p in enumerate(prims) if k not in far]
    # stand the model up: some exports lie along another axis, and the head / neck keep their modelled turn
    head = next((m for m, g in P['skin'].items() if g == 'faceSkull' and m in J), None)
    if head and P['pelvis'][0] in J:
        up_m = bind[J[head]] - bind[J[P['pelvis'][0]]]
        if np.linalg.norm(up_m) > 0 and np.dot(up_m, UP) / np.linalg.norm(up_m) < 0.8:
            Rup = rot_between(up_m, np.array(UP))
            bind = bind @ Rup.T
            for p in prims:
                p['pos'], p['nrm'] = p['pos'] @ Rup.T, p['nrm'] @ Rup.T
            print(f'  model lies along {np.round(up_m / np.linalg.norm(up_m), 2)}: stood up')
    # facing: Gladius characters face -y; a model facing +y is turned about z. Decided by the shoulders, then the hands
    # (left hand at +x when facing -y); toe bones only without hands (creatures), since IK rigs can point them anywhere.
    side = {}
    for gl in (('armShoulder_L', 'armShoulder_R'), ('armShoulderBlade_L', 'armShoulderBlade_R'),
               ('armWrist_L', 'armWrist_R')):
        got = {g: m for m, g in P['skin'].items() if g in gl and m in J}
        if len(got) == 2:
            side = (got[gl[0]], got[gl[1]])
            break
    toes = [bind[J[t]] - bind[J[f]] for f, t in P.get('feet', []) if f in J and t in J]
    if side:
        backwards = np.cross(bind[J[side[0]]] - bind[J[side[1]]], UP)[1] > 0
    else:
        backwards = bool(toes) and np.mean([d[1] for d in toes]) > 0
    if backwards:
        flip = np.array([-1.0, -1.0, 1.0])
        bind = bind * flip
        for p in prims:
            p['pos'], p['nrm'] = p['pos'] * flip, p['nrm'] * flip
    chunks = {t: (v, n, p) for t, v, n, p in xrnd.read_chunks(template)}
    bones = xchar.read_skeleton(chunks)
    B = {b['name']: i for i, b in enumerate(bones)}
    G = np.array([g[:3, 3] for g in xchar.bone_globals(bones)])
    # bones this class lacks (the Ogre has no pinkies): skin a pinky onto the ring finger, other missing ones onto the
    # wrist; their pose / length entries are dropped

    def have(gname):
        if gname in B:
            return gname
        if 'Pink' in gname and gname.replace('Pink', 'Ring') in B:
            return gname.replace('Pink', 'Ring')
        side = gname[-2:] if gname[-2:] in ('_L', '_R') else ''
        return 'armWrist' + side if side and 'finger' in gname and 'armWrist' + side in B else 'zero'

    P = dict(P)
    P['follow'] = dict(P['follow'])
    P['skin'] = {k: tuple(have(x) for x in v) if isinstance(v, tuple) else have(v) for k, v in P['skin'].items()}
    P['dirs'] = {k: v for k, v in P['dirs'].items() if v[1] in B and (not isinstance(v[2], str) or v[2] in B)}
    P['segments'] = [sg for sg in P['segments'] if sg[0] in B and sg[1] in B]
    # a follow whose source lost its pose entry would copy an unposed joint: drop it, so the joint follows its nearest
    # posed ancestor
    posed = set(P['dirs']) | set(P.get('orient', {}))
    changed = True
    while changed:
        changed = False
        for k, src in list(P['follow'].items()):
            if src not in posed and src not in P['follow']:
                del P['follow'][k]
                changed = True
    P['follow'] = dict(P['follow'])
    # 1. scale
    pm, pg = P['pelvis']
    # pelvis height over the soles, not the origin (some rigs have the pelvis at the origin)
    sole = min(p['pos'][:, 2].min() for p in prims)
    s = scale or G[B[pg]][2] / max(bind[J[pm]][2] - sole, 1e-9)

    C = bind * s
    # 2. skeleton: the template's local offsets (in the parent's frame), scaled per segment to the model's lengths
    k = np.ones(len(bones))
    for gf, gt, mf, mt in P['segments']:
        if SHAPE.get('height') and (gt.startswith('leg') or gf.startswith('leg')):
            continue                                    # chibi: Gladius leg lengths
        lg = np.linalg.norm(G[B[gt]] - G[B[gf]])
        lm = np.linalg.norm(C[J[mt]] - C[J[mf]])
        i = B[gt]
        while i != B[gf] and i >= 0:                  # every bone on the path gf -> gt
            k[i] = lm / lg
            i = bones[i]['parent']
    new_bones = [dict(b, pos=tuple(np.asarray(b['pos']) * k[i])) for i, b in enumerate(bones)]
    N = np.array([g[:3, 3] for g in xchar.bone_globals(new_bones)])
    # 3. pose: a rigid transform per model joint (rotation about the joint, joint moved onto its Gladius bone)
    R = {n: np.eye(3) for n in names}
    T = {n: np.zeros(3) for n in names}
    for mj, (mto, gb, gto) in P['dirs'].items():
        if mj not in J:
            continue
        dm = C[J[mto]] - C[J[mj]] if mto in J else np.array(UP)
        dg = (N[B[gto]] - N[B[gb]]) if isinstance(gto, str) else np.array(gto)
        R[mj] = rot_between(dm, dg)
        T[mj] = N[B[gb]] - R[mj] @ C[J[mj]]
    for mj, src in P['follow'].items():
        if mj in J and src in J:
            R[mj], T[mj] = R[src], T[src]
    for mj, (src, gb) in P.get('orient', {}).items():  # another joint's turn (None: no turn), on a Gladius bone
        if mj in J and (src is None or src in J):
            R[mj] = np.eye(3) if src is None else R[src]
            T[mj] = N[B[gb]] - R[mj] @ C[J[mj]]
    # unlisted joints (ends, extras) follow their nearest listed ancestor in the GLB order of the skin
    gl, _ = glb2pax.read_glb(glb)
    nodes = gl['nodes']
    parent = {c: i for i, n in enumerate(nodes) for c in n.get('children', [])}
    node_of = {n.get('name'): i for i, n in enumerate(nodes)}
    listed = set(P['dirs']) | set(P['follow']) | set(P.get('orient', {}))

    def ancestor(name):
        i = node_of.get(name)
        while i is not None:
            nm = nodes[i].get('name')
            if nm in listed:
                return nm
            i = parent.get(i)
        return None

    for n in names:
        if n not in listed:
            a = ancestor(n)
            if a:
                R[n], T[n] = R[a], T[a]
    Rs = np.array([R[n] for n in names])
    Ts = np.array([T[n] for n in names])

    def skin_target(n):
        while n is not None and n not in P['skin']:
            n = ancestor_parent(n)
        return P['skin'].get(n, 'zero') if n else 'zero'

    def ancestor_parent(name):
        i = parent.get(node_of.get(name))
        return nodes[i].get('name') if i is not None else None

    meshes = []
    for p in prims:
        v = p['pos'] * s
        w = p['w'] / np.maximum(p['w'].sum(1, keepdims=True), 1e-9)
        pv, pn = dq_blend(w, Rs, Ts, p['j'], v, p['nrm'])
        pn /= np.maximum(np.linalg.norm(pn, axis=1, keepdims=True), 1e-9)
        # 4. skin: model joint -> Gladius bone (a chain target splits the weight along the chain by position)
        jn = np.empty((len(v), 8), object)
        wn = np.zeros((len(v), 8))
        for vi in range(len(v)):
            acc = {}
            for kk in range(4):
                if w[vi, kk] <= 0:
                    continue
                t = skin_target(names[p['j'][vi, kk]])
                if isinstance(t, tuple):
                    a, b = N[B[t[0]]], N[B[t[1]]]
                    f = float(np.clip(np.dot(pv[vi] - a, b - a) / max(np.dot(b - a, b - a), 1e-9), 0, 1))
                    acc[t[0]] = acc.get(t[0], 0) + w[vi, kk] * (1 - f)
                    acc[t[1]] = acc.get(t[1], 0) + w[vi, kk] * f
                else:
                    acc[t] = acc.get(t, 0) + w[vi, kk]
            items = sorted(acc.items(), key=lambda x: -x[1])[:8]
            for kk, (bn, ww) in enumerate(items):
                jn[vi, kk], wn[vi, kk] = bn, ww
            for kk in range(len(items), 8):
                jn[vi, kk] = 'zero'
        meshes.append({'texture': p['texture'], 'pos': pv, 'nrm': pn, 'uv': p['uv'], 'joints': jn, 'weights': wn,
                       'col': p.get('col'),
                       'tris': p['tris'], 'sets': []})
    # the new SKEL: template records with the new local translations
    skel = bytearray(chunks[b'SKEL'][2])
    for i, b in enumerate(new_bones):
        struct.pack_into('<3f', skel, i * 32, *b['pos'])
    info = {'scale': s, 'height': float(max(m['pos'][:, 2].max() for m in meshes)),
            'joints': {n: np.round(N[B[n]], 3).tolist() for n in ('zero', 'faceSkull', 'armWrist_L', 'legAnkle_L')
                       if n in B}}
    if SHAPE.get('height') and not scale:
        # chibi models: with Gladius leg lengths the pelvis stays at its height and everything above scales with s - one
        # more pass sets s so the top lands at the wanted height
        top = max(m['pos'][:, 2].max() for m in meshes)
        gp = G[B[pg]][2]
        if top > gp + 1e-3:
            return retarget(glb, template, preset, s * (SHAPE['height'] - gp) / (top - gp), roles)
    return meshes, bytes(skel), prims, info


def tints_of(template_ptx):
    """the template's tint defaults [(set name, colour)] from its .ptx DTNT chunk"""
    ch = {t: (n, p) for t, v, n, p in xrnd.read_chunks(template_ptx)}
    if b'DTNT' not in ch:
        return []
    names = [s.decode('latin1') for s in ch[b'NAME'][1].split(b'\0')]
    n, d = ch[b'DTNT']
    return [(names[struct.unpack_from('<H', d, i * 6)[0]], struct.unpack_from('<I', d, i * 6 + 2)[0]) for i in range(n)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('glb')
    ap.add_argument('template', help='retail character .pax (its .ptx beside it supplies the tint defaults)')
    ap.add_argument('out', help='new .pax (the .ptx is written beside it)')
    ap.add_argument('--preset', default='auto', choices=['auto'] + sorted(PRESETS),
                    help='auto: detect the rig (tools/automap.py); or a hand-written preset')
    ap.add_argument('--map', help='roles JSON for --preset auto (edit one written by --save-map to fix the mapping)')
    ap.add_argument('--save-map', help='write the roles used by --preset auto to this JSON file')
    ap.add_argument('--scale', type=float, help='uniform scale (default: match the pelvis height)')
    ap.add_argument('--max-texture', type=int, default=1024)
    ap.add_argument('--max-bones', type=int, default=40)
    ap.add_argument('--drop-material', help='regex: leave out the parts whose material matches (strand hair cards, '
                    'mouth interiors) - a character is ONE vertex buffer of at most 65535 vertices')
    ap.add_argument('--max-verts', type=int, help='simplify the parts (quadric decimation, each by the same share) '
                    'until about this many vertices are left')
    ap.add_argument('--height', type=float, help='chibi / cartoon models: total height in units (a gladiator is ~2) '
                    'with Gladius leg lengths, instead of matching the pelvis height (huge heads on short legs)')
    ap.add_argument('--preset-json', help='a full preset (skin / dirs / follow / segments / pelvis / feet) for rigs '
                    'that are not humanoid, e.g. six legs onto the Scarab')
    args = ap.parse_args()
    REDUCE['drop'] = args.drop_material
    REDUCE['max_verts'] = args.max_verts
    REDUCE['prefix'] = os.path.splitext(os.path.basename(args.out))[0].lower()   # unique texture names per class
    SHAPE['height'] = args.height
    tpl = open(args.template, 'rb').read()
    roles = None
    if args.preset_json:
        args.preset = args.preset_json
    elif args.preset == 'auto':
        import json
        import automap
        if args.map:
            roles = json.load(open(args.map))
        else:
            roles, notes = automap.automap(*automap.rig_hierarchy(args.glb))
            for x in notes:
                print('  automap note:', x)
        print('roles: ' + ', '.join(f'{k}={v}' for k, v in roles.items()))
        if args.save_map:
            json.dump(roles, open(args.save_map, 'w'), indent=1)
            print(f'wrote {args.save_map}')
    meshes, skel, prims, info = retarget(args.glb, tpl, args.preset, args.scale, roles)
    if args.scale is None and not SHAPE.get('height'):
        # straightened legs reach lower than the model's own pose: rescale once so the soles stand on z = 0
        low = min(m['pos'][:, 2].min() for m in meshes)
        pelvis = info['joints']['zero'][2]
        if abs(low) > 0.005:
            meshes, skel, prims, info = retarget(args.glb, tpl, args.preset, info['scale'] * pelvis / (pelvis - low),
                                                 roles)
    from PIL import Image
    tex, seen = [], set()
    for p in prims:
        if p['texture'] in seen:
            continue
        seen.add(p['texture'])
        if not p['image']:
            # a colour-only material: an 8x8 swatch of its base colour (every texture the model names must be in its .ptx, or
            # the game aborts)
            c = np.clip(np.array(p.get('color') or [1, 1, 1, 1], float), 0, 1)
            c[:3] = c[:3] ** (1 / 2.2)                  # glTF factors are linear
            tex.append((p['texture'], np.tile((c * 255).round().astype(np.uint8), (8, 8, 1))))
            continue
        im = Image.open(io.BytesIO(p['image'])).convert('RGBA')
        side = lambda v: 1 << max(2, min(args.max_texture.bit_length() - 1, round(np.log2(max(v, 4)))))
        if (side(im.width), side(im.height)) != im.size:
            im = im.resize((side(im.width), side(im.height)), Image.LANCZOS)
        tex.append((p['texture'], np.array(im)))
    tptx = os.path.splitext(args.template)[0] + '.ptx'
    tptx_data = open(tptx, 'rb').read() if os.path.exists(tptx) else None
    data, st = glb2char.build_character(open(args.template, 'rb').read(), meshes, args.max_bones, skel, tptx_data,
                                        {n: (i.shape[1], i.shape[0]) for n, i in tex})
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    open(args.out, 'wb').write(data)
    print(f'scale {info["scale"]:.3f}, height {info["height"]:.2f}, joints {info["joints"]}')
    print(f'wrote {args.out} ({len(data)} bytes): {st["submeshes"]} sub-meshes, {st["vertices"]} vertices, '
          f'largest palette {st["max_palette"]}, textures {st["textures"]}')
    tints = tints_of(tptx_data) if tptx_data else []
    blob = ptx.build_ptx(tex, tints)
    # the class's colour variants (<name>_var01.ptx ...) get the same textures: a unit may load any of them
    stem, ext = os.path.splitext(args.out)
    tstem = os.path.splitext(args.template)[0]
    outs = [stem + '.ptx'] + [stem + v + '.ptx' for v in ('_var01', '_var02', '_var03', '_var04', '_var05')
                              if os.path.exists(tstem + v + '.ptx')]
    for o in outs:
        open(o, 'wb').write(blob)
    print(f'wrote {[os.path.basename(o) for o in outs]}: {[(n, i.shape[1]) for n, i in tex]}, '
          f'tints {[t for t, _ in tints]}')


if __name__ == '__main__':
    main()

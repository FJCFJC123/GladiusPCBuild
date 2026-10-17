"""Automatic humanoid rig mapping for tools/retarget.py - any common naming convention onto the Gladius skeleton.

    python tools/automap.py model.glb                    (prints the roles it found)
    python tools/automap.py model.glb --save roles.json  (edit, then: retarget.py ... --map roles.json)

Conventions covered by the name rules: Mixamo (mixamorig:LeftForeArm), Unreal (lowerarm_l), Character Creator
(CC_Base_L_Forearm), 3ds Max Biped (Bip01 L Forearm), Valve (ValveBiped.Bip01_L_Forearm), Rigify (DEF-forearm.L),
VRoid / VRM (J_Bip_L_LowerArm), Daz Genesis (lForearmBend), Advanced Skeleton (Elbow_L) and similar.

Names only pick the anchors (head, hands, feet, a few keywords); the hierarchy does the rest:
  hips      the joint where the spine and both legs meet (lowest common ancestor of head and feet)
  spine     the joints between hips and head ('neck' joints become the neck); spread over the Gladius
            lumbar / thoracic / neck bones
  arm       the chain from the spine to the hand: [clavicle], upper arm, forearm (twist / roll joints skipped)
  leg       the chain from the hips to the foot: thigh, shin; the toe is the foot's main child
  fingers   thumb / index / middle / ring / pinky by name, the last three joints of each chain
Every other joint (ends, eyes, jaw, hair, twist bones, props) follows its nearest mapped ancestor.

Roles file (JSON): {"hips": name, "spine": [names], "neck": [names], "head": name, "clavicle_L": name,
"upperarm_L": name, "forearm_L": name, "hand_L": name, "thigh_L": name, "calf_L": name, "foot_L": name,
"toe_L": name, "thumb_L": [3 names], "index_L": [...], "middle_L", "ring_L", "pinky_L", ... the same for _R};
missing or null roles are left out.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PREFIXES = ['valvebiped.', 'mixamorig', 'cc_base_', 'bip001', 'bip01', 'def-', 'org-', 'mch-', 'j_bip_c_', 'j_bip_',
            'j_sec_', 'b_', 'bn_', 'jnt_', 'joint_', 'bone_', 'skel_', 'rig_', 'armature_']
FINGERS = {'thumb': 'Th', 'index': 'In', 'middle': 'Mid', 'ring': 'Ring', 'pinky': 'Pink'}
FINGER_WORDS = {'thumb': ('thumb',), 'index': ('index', 'pointer'), 'middle': ('middle', 'mid'),
                'ring': ('ring',), 'pinky': ('pinky', 'little', 'small', 'pink')}


def split_name(name):
    """-> (side 'L' / 'R' / None, normalised base name)"""
    n = name.split(':')[-1].split('|')[-1]
    side = None
    m = re.match(r'^([lr])([A-Z].*)$', n)                          # Daz: lShldrBend, rForearmBend
    if m and not n.lower().startswith(('left', 'right')):
        side, n = m.group(1).upper(), m.group(2)
    s = n.lower()
    for p in PREFIXES:
        if s.startswith(p):
            s = s[len(p):]
    s = s.strip(' _.-')
    if side is None:
        # the side word / letter as its own token (group 1 is removed): Left..., ..._l, ... L ..., .L, l_...
        for pat, sd in ((r'^(left)', 'L'), (r'^(right)', 'R'), (r'(left)$', 'L'), (r'(right)$', 'R'),
                        (r'[ _.\-](left)(?=[ _.\-])', 'L'), (r'[ _.\-](right)(?=[ _.\-])', 'R'),
                        (r'^(l)(?=[ _.\-\d])', 'L'), (r'^(r)(?=[ _.\-\d])', 'R'),
                        (r'[ _.\-](l)$', 'L'), (r'[ _.\-](r)$', 'R'),
                        (r'[ _.\-](l)(?=[ _.\-])', 'L'), (r'[ _.\-](r)(?=[ _.\-])', 'R')):
            mm = re.search(pat, s)
            if mm:
                side = sd
                s = s[:mm.start(1)] + s[mm.end(1):]
                break
    base = re.sub(r'[ _.\-]+', '', s)
    if not re.search(r'(finger|thumb|index|middle|ring|pinky|little|toe)', base):
        base = re.sub(r'\d+$', '', base)
    return side, base


def is_end(base):
    return bool(re.search(r'(nub|headtop|top)$', base)) or (base.endswith('end') and not base.endswith('bend'))


def finger_of(base):
    """'thumb' / 'index' / ... for a finger joint name, also Biped / Valve numbering (Finger0 = thumb .. 4 = pinky)"""
    m = re.match(r'^finger(\d)', base)
    if m and int(m.group(1)) < 5:
        return ['thumb', 'index', 'middle', 'ring', 'pinky'][int(m.group(1))]
    for f, words in FINGER_WORDS.items():
        if any(w in base for w in words) and not (f == 'middle' and any(x in base for x in ('index', 'ring', 'thumb'))):
            return f
    return None


def has(base, *words):
    return any(w in base for w in words)


def automap(names, parents, positions=None, weighted=None):
    """names: joint names; parents: index of each joint's parent joint (-1 = none); positions: (n, 3) z-up bind
    positions (optional, breaks ties); weighted: set of joint indices that carry skin weights (optional)
    -> (roles dict, notes list)"""
    n = len(names)
    info = [split_name(x) for x in names]
    kids = {i: [j for j in range(n) if parents[j] == i] for i in range(n)}
    notes = []

    def chain_up(i):
        out = []
        while i >= 0:
            out.append(i)
            i = parents[i]
        return out

    def find(pred, side=None):
        c = [i for i in range(n) if pred(info[i][1]) and (side is None or info[i][0] == side)
             and not is_end(info[i][1])]
        # prefer weighted joints, then the one highest in the hierarchy
        c.sort(key=lambda i: (weighted is not None and i not in weighted, len(chain_up(i))))
        return c[0] if c else None

    head = find(lambda b: b in ('head',) or (b.startswith('head') and not has(b, 'top', 'end', 'nub')))
    hands = {s: find(lambda b: has(b, 'hand', 'wrist') and not has(b, 'thumb', 'index', 'middle', 'ring', 'pinky',
                                                                    'finger', 'twist', 'ik', 'pole'), s)
             for s in 'LR'}
    feet = {s: find(lambda b: has(b, 'foot', 'ankle') and not has(b, 'toe', 'ball', 'twist', 'ik', 'pole', 'heel'), s)
            for s in 'LR'}
    if head is None and None not in hands.values():
        # no 'head' name (Rigify: DEF-spine.006): follow the unsided chain up from where both arms branch off
        a = [chain_up(hands['L']), chain_up(hands['R'])]
        j = next((i for i in a[0] if i in a[1]), None)
        while j is not None:
            nxt = [c for c in kids[j] if info[c][0] is None and not is_end(info[c][1])
                   and c not in a[0] and c not in a[1]]
            if not nxt:
                break
            j = max(nxt, key=lambda c: sum(1 for x in range(n) if c in chain_up(x)))
        head = j
        notes.append(f'head found by structure: {names[head]}' if head is not None else 'no head found')
    if head is None or None in hands.values() or None in feet.values():
        raise ValueError(f'could not find head / hands / feet by name (head {head}, hands {hands}, feet {feet}) - '
                         f'give a roles file with --map')

    # hips: lowest common ancestor of the head and both feet
    anc = [chain_up(x) for x in (head, feet['L'], feet['R'])]
    common = [i for i in anc[0] if i in anc[1] and i in anc[2]]
    hips = common[0]
    roles = {'hips': names[hips], 'head': names[head]}

    def path(a, b):
        """joints strictly between ancestor a and descendant b"""
        up = chain_up(b)
        if a not in up:
            return []
        return list(reversed(up[1:up.index(a)]))

    skip = lambda i: has(info[i][1], 'twist', 'roll', 'pole') or info[i][1].startswith('ik')
    # spine + neck between hips and head
    mid = [i for i in path(hips, head) if not has(info[i][1], 'twist')]
    neck = [i for i in mid if has(info[i][1], 'neck')]
    spine = [i for i in mid if i not in neck]
    if not neck and len(spine) > 1:                       # no 'neck' name: the last joint below the head
        neck, spine = [spine[-1]], spine[:-1]
    roles['spine'] = [names[i] for i in spine]
    roles['neck'] = [names[i] for i in neck]
    chest = spine[-1] if spine else hips

    for s in 'LR':
        # arm: from where it leaves the spine / neck chain to the hand
        up = chain_up(hands[s])
        stem = [i for i in up if i in [hips] + spine + neck]
        attach = stem[0] if stem else chest
        arm = [i for i in path(attach, hands[s]) if not skip(i)]
        # forearm by name (else the last joint before the hand); the upper arm is the joint before it and anything
        # before that is the clavicle ('Shoulder' is a clavicle in Mixamo but an upper arm in Advanced Skeleton)
        fore = [i for i in arm if has(info[i][1], 'forearm', 'lowerarm', 'elbow', 'lowarm', 'radius', 'ulna')]
        fi = arm.index(fore[0]) if fore else len(arm) - 1
        if fi < 1:
            notes.append(f'arm {s}: chain {[names[i] for i in arm]} too short')
            continue
        ui = next((k for k in range(fi) if has(info[arm[k]][1], 'upperarm', 'uparm', 'shldr', 'humerus')
                   or info[arm[k]][1] == 'arm'), fi - 1)        # first named one (Rigify splits it in segments)
        f, u, clav = arm[fi], arm[ui], arm[:ui]
        if clav:
            roles[f'clavicle_{s}'] = names[clav[-1]]
        roles[f'upperarm_{s}'] = names[u]
        roles[f'forearm_{s}'] = names[f]
        roles[f'hand_{s}'] = names[hands[s]]
        # leg: hips -> foot
        # leg: the shin by name (else the last joint before the foot); the thigh is the joint before it
        leg = [i for i in path(hips, feet[s]) if not skip(i)]
        shin = [i for i in leg if has(info[i][1], 'calf', 'shin', 'knee', 'lowerleg', 'lowleg', 'tibia') or
                info[i][1] == 'leg']
        ci = leg.index(shin[0]) if shin else len(leg) - 1
        if ci >= 1:
            ti = next((k for k in range(ci) if has(info[leg[k]][1], 'thigh', 'upleg', 'upperleg', 'femur')), ci - 1)
            roles[f'thigh_{s}'], roles[f'calf_{s}'] = names[leg[ti]], names[leg[ci]]
        else:
            notes.append(f'leg {s}: chain {[names[i] for i in leg]} too short')
        roles[f'foot_{s}'] = names[feet[s]]
        toe = [c for c in kids[feet[s]] if not is_end(info[c][1])]
        toe.sort(key=lambda c: (not has(info[c][1], 'toe', 'ball'), -len(kids[c])))
        if toe:
            roles[f'toe_{s}'] = names[toe[0]]
        # fingers: chains under the hand, by name
        under = [j for j in range(n) if hands[s] in chain_up(j)[1:]]
        for f_role in FINGER_WORDS:
            ch = [j for j in under if finger_of(info[j][1]) == f_role and not is_end(info[j][1])
                  and not has(info[j][1], 'metacarpal', 'meta')]
            # unweighted leaves are end markers even without an 'end' in the name (Mixamo ...Thumb4)
            ch = [j for j in ch if kids[j] or weighted is None or j in weighted]
            ch.sort(key=lambda j: len(chain_up(j)))
            if ch:
                roles[f'{f_role}_{s}'] = [names[j] for j in ch[:3]]
    return roles, notes


def roles_to_preset(roles):
    """roles -> the retarget preset structure (skin targets, bone directions, followers, length segments)"""
    r = lambda k: roles.get(k)
    skin, dirs, follow, seg = {}, {}, {}, []
    hips, head = r('hips'), r('head')
    spine, neck = list(r('spine') or []), list(r('neck') or [])
    skin[hips] = 'zero'
    # spine: the last spine joint is the chest (thoracic); the ones below spread over lumbar 1 / 2
    lumbar = ['bodyLumbar1', 'bodyLumbar2']
    chain_m, chain_g = [hips], ['zero']
    if spine:
        low = spine[:-1]
        for k, j in enumerate(low):
            g = lumbar[min(len(lumbar) - 1, k * len(lumbar) // max(1, len(low)))]
            skin[j] = g
            if g not in chain_g:
                chain_m.append(j)
                chain_g.append(g)
            else:
                follow[j] = chain_m[-1]
        skin[spine[-1]] = ('bodyThoracic', 'bodyThoracic2')
        chain_m.append(spine[-1])
        chain_g.append('bodyThoracic')
    if neck:
        skin[neck[0]] = ('bodyNeck', 'bodyNeck2') if len(neck) == 1 else 'bodyNeck'
        for j in neck[1:]:
            skin[j] = 'bodyNeck2'
            follow[j] = neck[0]
        chain_m.append(neck[0])
        chain_g.append('bodyNeck')
    skin[head] = 'faceSkull'
    chain_m.append(head)
    chain_g.append('faceSkull')
    for a in range(len(chain_m) - 1):
        dirs[chain_m[a]] = (chain_m[a + 1], chain_g[a], chain_g[a + 1])
        seg.append((chain_g[a], chain_g[a + 1], chain_m[a], chain_m[a + 1]))
    # the head keeps the facing it was modelled with (no turn) and sits on the Gladius skull; copying the neck's or
    # chest's turn tipped heads too far
    orient = {head: (None, 'faceSkull')}
    if neck:                                            # same for the neck: some models hang the whole helmet on it
        orient[neck[0]] = (None, 'bodyNeck')            # helmets skinned entirely to the neck would tip with it
    feet = []
    for s in ('L', 'R'):
        clav, up, fo, ha = r(f'clavicle_{s}'), r(f'upperarm_{s}'), r(f'forearm_{s}'), r(f'hand_{s}')
        if up and fo and ha:
            arm_m = ([clav] if clav else []) + [up, fo, ha]
            arm_g = (['armShoulderBlade_' + s] if clav else []) + ['armShoulder_' + s, 'armElbow_' + s, 'armWrist_' + s]
            for j, g in zip(arm_m, arm_g):
                skin[j] = g
            for a in range(len(arm_m) - 1):
                dirs[arm_m[a]] = (arm_m[a + 1], arm_g[a], arm_g[a + 1])
                seg.append((arm_g[a], arm_g[a + 1], arm_m[a], arm_m[a + 1]))
            mid = r(f'middle_{s}') or r(f'index_{s}') or r(f'ring_{s}')
            if mid:
                key = 'Mid' if r(f'middle_{s}') else 'In' if r(f'index_{s}') else 'Ring'
                dirs[ha] = (mid[0], 'armWrist_' + s, f'finger{key}Root_' + s)
            else:
                follow[ha] = fo
        th, ca, ft, to = r(f'thigh_{s}'), r(f'calf_{s}'), r(f'foot_{s}'), r(f'toe_{s}')
        if th and ca and ft:
            leg_m, leg_g = [th, ca, ft] + ([to] if to else []), ['legHip_' + s, 'legKnee_' + s, 'legAnkle_' + s] + (
                ['legBall_' + s] if to else [])
            for j, g in zip(leg_m, leg_g):
                skin[j] = g
            seg.append(('zero', 'legHip_' + s, hips, th))
            for a in range(len(leg_m) - 1):
                dirs[leg_m[a]] = (leg_m[a + 1], leg_g[a], leg_g[a + 1])
                seg.append((leg_g[a], leg_g[a + 1], leg_m[a], leg_m[a + 1]))
            if to:
                follow[to] = ft
                feet.append((ft, to))
        elif th and ca:                                 # a two-joint leg (a peg leg): hip and knee only
            skin[th], skin[ca] = 'legHip_' + s, 'legKnee_' + s
            seg.append(('zero', 'legHip_' + s, hips, th))
            dirs[th] = (ca, 'legHip_' + s, 'legKnee_' + s)
            seg.append(('legHip_' + s, 'legKnee_' + s, th, ca))
            follow[ca] = th
        for f_role, g in FINGERS.items():
            ch = r(f'{f_role}_{s}') or []
            gl = [f'finger{g}Root_{s}', f'finger{g}Mid_{s}', f'finger{g}Tip_{s}'][:len(ch)]
            for j, gb in zip(ch, gl):
                skin[j] = gb
            for a in range(len(ch) - 1):
                dirs[ch[a]] = (ch[a + 1], gl[a], gl[a + 1])
            if ch:
                follow[ch[-1]] = ch[-2] if len(ch) > 1 else ha
    for k in list(follow):
        dirs.pop(k, None)
    for j in orient:
        dirs.pop(j, None)
    return {'skin': skin, 'dirs': dirs, 'follow': follow, 'segments': seg, 'pelvis': (hips, 'zero'), 'feet': feet,
            'orient': orient}


def rig_hierarchy(glb):
    """-> (joint names, parent joint index per joint, z-up bind positions, weighted joint set) of the first skin"""
    import numpy as np
    import glb2pax
    import retarget
    names, bind, prims = retarget.load_rig(glb)
    gltf, _ = glb2pax.read_glb(glb)
    skin = gltf['skins'][0]
    jn = skin['joints']
    node_parent = {c: i for i, nd in enumerate(gltf['nodes']) for c in nd.get('children', [])}
    parents = []
    for j in jn:
        p = node_parent.get(j)
        while p is not None and p not in jn:
            p = node_parent.get(p)
        parents.append(jn.index(p) if p is not None else -1)
    weighted = set()
    for p in prims:
        weighted |= set(np.unique(p['j'][p['w'] > 0]).tolist())
    return names, parents, bind, weighted


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('glb')
    ap.add_argument('--save', help='write the roles to this JSON file')
    args = ap.parse_args()
    names, parents, bind, weighted = rig_hierarchy(args.glb)
    roles, notes = automap(names, parents, bind, weighted)
    for k, v in roles.items():
        print(f'  {k:12s} {v}')
    for x in notes:
        print('  note:', x)
    if args.save:
        json.dump(roles, open(args.save, 'w'), indent=1)
        print(f'wrote {args.save}')


if __name__ == '__main__':
    main()

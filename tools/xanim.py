"""Gladius Xbox animations (.pan) - reader / DCT track decoder.

    python tools/xanim.py bec_unpacked/data/characters/legionnaire/legionnaire_idle.pan      (prints a summary)

Chunks: VERS, CPRT, HEDR (u16 joint count, int16 root translation / 1024, float length in seconds), NAME (joint names,
zero separated), BLNM (event names), MASK (one byte per NAME entry: 0x40 = has a DCT position track, 0x80 = has a
DCT rotation track; tracks are numbered in NAME order), BLTK (u32 key count), BKTM (float key times), BOOL (u32
event bits per key), DCPT / DCRT (8 bytes per track: u8 rate, u8 sqrt quality, u8 pre-roll, u8 post-rotation,
u32 data bytes), DCPD / DCRD (the tracks' data, back to back).

A DCT track is a bit stream (read least significant bit first) of blocks; a block holds 8 samples of x, y, z as 3 x 8
DCT coefficients (per component: the even coefficients 0, 2, 4, 6 then the odd 1, 3, 5, 7). Each coefficient is a
variable-length code: '00' = 0; '01' + 1 bit -> 1 value bit, '01' + 1 -> 2; '10' + 0 -> 3, '10' + 1 -> 4; '11', n
ones, a zero -> 5 + n value bits. A value v of b bits is negative when v < 2^(b-1): v - 2^b + 1. The first
coefficient of each component is a delta on the previous block's. Samples = IDCT(coefficients * quantisation
dividers) * 128^2 / (int scale * quality^2); int scale = 1024 (positions in metres) or 4096 (rotations).
Rotation samples are quaternion x, y, z (w = sqrt(1 - |xyz|^2)) followed by a fixed post-rotation that is undone.
The track runs at 30 * (256 >> rate) / 256 keys per second; sample i is key i - pre-roll.
"""
import math
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xrnd  # noqa: E402

M_XPOS, M_XROT = 0x40, 0x80
QUANT = [1.6, 1.1, 1.0, 1.6, 2.4, 4.0, 5.1, 6.1]


def _idct_matrix():
    """8 samples from 8 coefficients in file order (even 0,2,4,6 then odd 1,3,5,7), dividers included"""
    M = np.zeros((8, 8))
    for pos, k in enumerate([0, 2, 4, 6, 1, 3, 5, 7]):
        ck = math.sqrt(1 / 8) if k == 0 else 0.5
        for s in range(8):
            M[s, pos] = ck * QUANT[k] * math.cos(math.pi * (2 * s + 1) * k / 16)
    return M


IDCT = _idct_matrix()


class Bits:
    """least-significant-bit-first reader over a byte string"""

    def __init__(self, data, pos=0):
        self.v = int.from_bytes(data, 'little')
        self.n = len(data) * 8
        self.pos = pos

    def get(self, k):
        r = (self.v >> self.pos) & ((1 << k) - 1)
        self.pos += k
        return r

    def code(self):
        if not self.get(1):
            if not self.get(1):
                return 0
            vb = 2 if self.get(1) else 1
        elif not self.get(1):
            vb = 4 if self.get(1) else 3
        else:
            vb = 5
            while self.get(1):
                vb += 1
        v = self.get(vb)
        return v - (1 << vb) + 1 if v < (1 << (vb - 1)) else v


def decode_samples(data, rate, quality, rotation, min_samples):
    """all samples of one track -> (n, 3) array (positions in metres / quaternion xyz before the post-rotation)"""
    scale = 128.0 * 128.0 / ((4096 if rotation else 1024) * quality * quality)
    b = Bits(data)
    dc = np.zeros(3)
    out = []
    while len(out) < min_samples and b.pos < b.n:
        c = np.array([b.code() for _ in range(24)], float).reshape(3, 8)
        c[:, 0] += dc
        dc = c[:, 0].copy()
        out.extend((IDCT @ (c * scale).T).tolist())          # (8 samples, 3 components)
    return np.array(out[:min_samples])


def unrotate(q, post):
    x, y, z, w = q
    if post == 0:
        return (x, y, z, w)
    if post == 1:
        return (w, z, -y, -x)
    if post == 2:
        return (-z, w, x, -y)
    if post == 3:
        return (y, -x, w, -z)
    r = {4: (x + w, y + z, z - y, w - x), 5: (x - w, y - z, z + y, w + x), 6: (x - z, y + w, z + x, w - y),
         7: (x + z, y - w, z - x, w + y), 8: (x + y, y - x, z + w, w - z), 9: (x - y, y + x, z - w, w + z)}[post]
    s = math.sin(math.pi / 4)
    return tuple(v * s for v in r)


def read_pan(data):
    ch = {}
    for t, v, n, p in xrnd.read_chunks(data):
        ch[t] = (n, p)
    count, trans, length = struct.unpack_from('<Hhf', ch[b'HEDR'][1], 0)
    names = [s.decode('latin1') for s in ch[b'NAME'][1].split(b'\0')][:ch[b'MASK'][0]]
    mask = ch[b'MASK'][1][:len(names)]
    an = {'length': length, 'translation': trans / 1024.0, 'joints': names, 'mask': list(mask),
          'events': [s.decode('latin1') for s in ch[b'BLNM'][1].split(b'\0') if s] if b'BLNM' in ch else [],
          'pos': [], 'rot': []}
    for kind, tab, dat, bit in (('pos', b'DCPT', b'DCPD', M_XPOS), ('rot', b'DCRT', b'DCRD', M_XROT)):
        if tab not in ch:
            continue
        n, t = ch[tab]
        blob = ch[dat][1]
        joints = [nm for nm, m in zip(names, mask) if m & bit]
        off = 0
        for i in range(n):
            rate, quality, preroll, post, size = struct.unpack_from('<BBBBI', t, i * 8)
            an[kind].append({'joint': joints[i] if i < len(joints) else None, 'rate': rate, 'quality': quality,
                             'preroll': preroll, 'post': post, 'data': blob[off:off + size]})
            off += size
    return an


def sample_track(tr, length, rotation):
    """-> (times in seconds, values (n, 3) positions or (n, 4) quaternions x, y, z, w) covering 0 .. length"""
    fps = 30.0 * (256 >> tr['rate']) / 256.0
    nkeys = int(math.ceil(length * fps - 1e-4)) + 1
    s = decode_samples(tr['data'], tr['rate'], tr['quality'], rotation, nkeys + tr['preroll'])
    s = s[tr['preroll']:tr['preroll'] + nkeys]
    times = np.minimum(np.arange(len(s)) / fps, length)
    if not rotation:
        return times, s
    q = []
    for x, y, z in s:
        w = math.sqrt(max(0.0, 1.0 - (x * x + y * y + z * z)))
        q.append(unrotate((x, y, z, w), tr['post']))
    q = np.array(q)
    q /= np.maximum(np.linalg.norm(q, axis=1, keepdims=True), 1e-9)
    return times, q


def decode(an):
    """{joint: {'pos': (times, xyz), 'rot': (times, xyzw)}}"""
    out = {}
    for kind in ('pos', 'rot'):
        for tr in an[kind]:
            if tr['joint']:
                out.setdefault(tr['joint'], {})[kind] = sample_track(tr, an['length'], kind == 'rot')
    return out


if __name__ == '__main__':
    an = read_pan(open(sys.argv[1], 'rb').read())
    tracks = decode(an)
    print(f'{sys.argv[1]}: {an["length"]:.3f} s, {len(an["joints"])} joints, {len(an["pos"])} position tracks, '
          f'{len(an["rot"])} rotation tracks, events {an["events"]}')
    for j in list(tracks)[:6]:
        for kind, (t, v) in tracks[j].items():
            print(f'  {j:20s} {kind}: {len(t)} keys, first {np.round(v[0], 3)}, last {np.round(v[-1], 3)}')

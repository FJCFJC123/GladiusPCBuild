"""Xbox .ptx texture bundles for Gladius (writer + DXT1/DXT5 encoder).

Layout (retail, 2329 files surveyed): PTTP (v3) | NAME names, zero-padded | NMPT names padded to 32 |
PFHD one 32-byte entry per texture | PADD | PTDT texture data | END.  The PTDT data starts on a 32-byte file
boundary (PFHD carries the padding). Entry: u16 format (0x4100 DXT1, 0x4300 DXT5), u16 mip count, u32 0,
u16 width, u16 height, u32 data size (padded to 32), u32 data offset RELATIVE TO THE ENTRY, u32 name offset
in NMPT, u32 0, u32 0. Mip levels follow each other, linear (not swizzled) DXT blocks.

    python tools/ptx.py out.ptx image.png [image2.png ...]    (texture names = file names without extension)
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xrnd  # noqa: E402  (chunk writer)

FMT_DXT1, FMT_DXT5 = 0x4100, 0x4300


# ------------------------------------------------------------------------------------------------ DXT encoder
def _blocks(img):
    """HxWxC uint8 (H, W multiples of 4) -> (N, 16, C) float32 in row-major block order"""
    h, w, c = img.shape
    b = img.reshape(h // 4, 4, w // 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(-1, 16, c)
    return b.astype(np.float32)


def _to565(rgb):
    r = np.clip(np.round(rgb[..., 0] * 31 / 255), 0, 31).astype(np.uint32)
    g = np.clip(np.round(rgb[..., 1] * 63 / 255), 0, 63).astype(np.uint32)
    b = np.clip(np.round(rgb[..., 2] * 31 / 255), 0, 31).astype(np.uint32)
    return (r << 11) | (g << 5) | b


def _from565(c):
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.float32)


def _color_block(rgb):
    """(N,16,3) -> (N,) u16 c0, u16 c1, u32 indices: 4-colour mode (c0 > c1)"""
    mean = rgb.mean(1, keepdims=True)
    d = rgb - mean
    cov = np.einsum('nki,nkj->nij', d, d)
    axis = np.ones((len(rgb), 3), np.float32)
    for _ in range(4):                                          # power iteration: principal axis per block
        axis = np.einsum('nij,nj->ni', cov, axis)
        axis /= np.linalg.norm(axis, axis=1, keepdims=True) + 1e-9
    proj = np.einsum('nki,ni->nk', d, axis)
    lo = rgb[np.arange(len(rgb)), proj.argmin(1)]
    hi = rgb[np.arange(len(rgb)), proj.argmax(1)]
    c0, c1 = _to565(hi), _to565(lo)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    p0, p1 = _from565(c0), _from565(c1)
    pal = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)          # (N,4,3)
    dist = ((rgb[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)             # (N,16,4)
    idx = dist.argmin(-1).astype(np.uint32)
    same = c0 == c1                                             # flat block: c0 == c1 means 3-colour mode
    idx[same] = 0
    bits = np.zeros(len(rgb), np.uint64)
    for k in range(16):
        bits |= idx[:, k].astype(np.uint64) << np.uint64(2 * k)
    return c0, c1, bits


def _alpha_block(a):
    """(N,16) -> bytes per block (8): a0 > a1 eight-value mode"""
    a0 = a.max(1)
    a1 = a.min(1)
    flat = a0 == a1
    a0 = np.where(flat, np.minimum(a0 + 1, 255), a0)
    a1 = np.where(flat & (a0 == 255), a1 - 1, a1)
    a1 = np.where(a0 == a1, a0 - 1, a1)
    t = np.arange(8, dtype=np.float32)
    w = np.array([0, 7, 1, 2, 3, 4, 5, 6], np.float32)          # index -> weight of a1 (0 = a0, 1 = a1, 2.. = mixes)
    vals = ((7 - w) * a0[:, None] + w * a1[:, None]) / 7         # (N,8)
    idx = np.abs(a[:, :, None] - vals[:, None, :]).argmin(-1).astype(np.uint64)
    bits = np.zeros(len(a), np.uint64)
    for k in range(16):
        bits |= idx[:, k] << np.uint64(3 * k)
    return a0.astype(np.uint8), a1.astype(np.uint8), bits


def encode(img, fmt):
    """HxWx4 uint8 -> DXT1/DXT5 bytes (padded up to a multiple of 4 pixels)"""
    h, w = img.shape[:2]
    ph, pw = -h % 4, -w % 4
    if ph or pw:
        img = np.pad(img, ((0, ph), (0, pw), (0, 0)), mode='edge')
    b = _blocks(img)
    c0, c1, bits = _color_block(b[:, :, :3])
    color = np.zeros((len(b), 8), np.uint8)
    color[:, 0:2] = c0.astype('<u2').view(np.uint8).reshape(-1, 2)
    color[:, 2:4] = c1.astype('<u2').view(np.uint8).reshape(-1, 2)
    color[:, 4:8] = bits.astype('<u4').view(np.uint8).reshape(-1, 4)
    if fmt == FMT_DXT1:
        return color.tobytes()
    a0, a1, abits = _alpha_block(b[:, :, 3])
    alpha = np.zeros((len(b), 8), np.uint8)
    alpha[:, 0], alpha[:, 1] = a0, a1
    alpha[:, 2:8] = abits.astype('<u8').view(np.uint8).reshape(-1, 8)[:, :6]
    return np.concatenate([alpha, color], 1).tobytes()


def mip_chain(img):
    """box-filtered levels down to 1x1"""
    levels = [img]
    while img.shape[0] > 1 or img.shape[1] > 1:
        h, w = max(1, img.shape[0] // 2), max(1, img.shape[1] // 2)
        f = img.astype(np.float32)
        if img.shape[0] > 1:
            f = (f[0::2] + f[1::2]) / 2
        if img.shape[1] > 1:
            f = (f[:, 0::2] + f[:, 1::2]) / 2
        img = np.clip(np.round(f), 0, 255).astype(np.uint8)
        levels.append(img)
    return levels


# ------------------------------------------------------------------------------------------------ container
def encode_entry(name, img):
    """(name without .tga, HxWx4 uint8 RGBA) -> entry (name, format, mips, width, height, data padded to 32).
    DXT5 when any alpha < 255."""
    fmt = FMT_DXT5 if (img[..., 3] < 255).any() else FMT_DXT1
    levels = mip_chain(img)
    d = b''.join(encode(l, fmt) for l in levels)
    d += b'\0' * (-len(d) % 32)
    return name, fmt, len(levels), img.shape[1], img.shape[0], d


def read_ptx_entries(data):
    """.ptx bytes -> [entry] as encode_entry makes them, the stored data byte for byte.
    Variants handled: format codes 0x4100 DXT1, 0x4200 DXT3, 0x4300 DXT5 and the Xbox 0x2000 DXT1 / 0x2200 DXT5; a
    model without textures of its own has an empty PFHD and no NAME / NMPT; NMPT names can run together without a
    terminator ("a.tgab.tga") - a name ends at its zero or right after ".tga"."""
    chunks, o = {}, 0
    while o + 16 <= len(data):
        tag, size, ver, num = struct.unpack_from('<4sIII', data, o)
        chunks[tag] = (o + 16, num, size)
        if size < 16 or tag == b'END.':
            break
        o += size
    if b'PFHD' not in chunks or chunks[b'PFHD'][1] == 0:
        return []
    nmpt = chunks[b'NMPT'][0] if b'NMPT' in chunks else None
    pfhd, n, _ = chunks[b'PFHD']
    out = []
    for i in range(n):
        e = pfhd + 32 * i
        fmt, mips, _, w, h, size, rel, noff = struct.unpack_from('<HHIHHIII', data, e)
        name = f'tex_{i}'
        if nmpt is not None and nmpt + noff < len(data):
            raw = data[nmpt + noff:data.find(b'\0', nmpt + noff)]
            k = raw.lower().find(b'.tga')
            name = (raw[:k] if k >= 0 else raw).decode('latin1')
        if rel == 0xFFFFFFFF or e + rel + size > len(data):     # shared texture (eye, skygold_R ...): entry only
            out.append((name, fmt, mips, w, h, None, bytes(data[e:e + 32])))
            continue
        out.append((name, fmt, mips, w, h, bytes(data[e + rel:e + rel + size])))
    return out


def build_ptx(textures, tints=()):
    """textures: [(name without .tga, HxWx4 uint8 RGBA)] -> .ptx bytes. DXT5 when any alpha < 255.
    tints: character tint defaults [(set name e.g. 'set_tintSkin', u32 colour)]: their names follow the texture names
    in NAME and a DTNT chunk (6 bytes each: u16 NAME index, u32 colour) follows NAME, as in retail characters."""
    return build_ptx_entries([encode_entry(name, img) for name, img in textures], tints)


def build_ptx_entries(entries, tints=()):
    """[(name, format, mips, width, height, data padded to 32)] -> .ptx bytes. data None = a shared texture's entry
    (no data in this file, data offset -1): (..., None, the original 32-byte entry) is written back as it was."""
    names = [t[0] + '.tga' for t in entries]
    blob = b'\0'.join(n.encode('latin1') for n in names) + b'\0'
    name_off, p = [], 0
    for n in names:
        name_off.append(p)
        p += len(n) + 1
    all_names = blob + b''.join(t.encode('latin1') + b'\0' for t, _ in tints)
    name_payload = all_names + b'\0' + b'\0' * (-(len(all_names) + 1) % 4)  # retail: one extra zero, then pad to 4
    dtnt = b''.join(struct.pack('<HI', len(names) + i, c) for i, (_, c) in enumerate(tints))
    dtnt += b'\0' * (-len(dtnt) % 4)
    dtnt_chunk = 16 + len(dtnt) if tints else 0
    nmpt_payload = blob + b'\0' * (-len(blob) % 32)
    datas = [e[5] + b'\0' * (-len(e[5]) % 32) if e[5] is not None else b'' for e in entries]
    metas = [(e[1], e[2], e[3], e[4], len(d)) for e, d in zip(entries, datas)]
    n = len(entries)
    # file offsets: PTTP 16 | NAME 16+len | NMPT 16+len | PFHD 16+32n+pad | PADD 16 | PTDT 16 | data (32-aligned)
    pfhd_payload_off = 16 + (16 + len(name_payload)) + dtnt_chunk + (16 + len(nmpt_payload)) + 16
    pad = -(pfhd_payload_off + 32 * n + 16 + 16) % 32
    ptdt_payload_off = pfhd_payload_off + 32 * n + pad + 16 + 16
    pfhd = bytearray()
    rel = 0
    for i, (fmt, mips, w, h, size) in enumerate(metas):
        entry_off = pfhd_payload_off + 32 * i
        if entries[i][5] is None:                        # shared: the original entry, its name offset updated
            raw = bytearray(entries[i][6])
            struct.pack_into('<I', raw, 20, name_off[i])
            pfhd += raw
            continue
        pfhd += struct.pack('<HHIHHIIIII', fmt, mips, 0, w, h, size, ptdt_payload_off + rel - entry_off,
                            name_off[i], 0, 0)
        rel += size
    pfhd += b'\0' * pad
    chunks = [(b'PTTP', 3, 1, b''), (b'NAME', 0, 1, name_payload)]
    if tints:
        chunks.append((b'DTNT', 0, len(tints), dtnt))
    chunks += [(b'NMPT', 0, 0, nmpt_payload), (b'PFHD', 1, n, bytes(pfhd)), (b'PADD', 1, 1, b''),
               (b'PTDT', 1, 1, b''.join(datas)), (b'END.', 1, 1, b'')]
    return xrnd.write_chunks(chunks)


def load_image(path, max_size=2048, pow2=True):
    from PIL import Image
    im = Image.open(path).convert('RGBA')
    w, h = im.size
    if pow2:
        p2 = lambda v: 1 << max(2, min(max_size.bit_length() - 1, round(np.log2(max(v, 4)))))
        nw, nh = p2(w), p2(h)
        if (nw, nh) != (w, h):
            im = im.resize((nw, nh), Image.LANCZOS)
    return np.array(im, dtype=np.uint8)


if __name__ == '__main__':
    out = sys.argv[1]
    tex = [(os.path.splitext(os.path.basename(p))[0], load_image(p)) for p in sys.argv[2:]]
    data = build_ptx(tex)
    open(out, 'wb').write(data)
    print(f'wrote {out} ({len(data)} bytes): ' + ', '.join(f'{n} {i.shape[1]}x{i.shape[0]}' for n, i in tex))

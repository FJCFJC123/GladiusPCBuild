"""Title image of an XBE -> Windows icon images (pure Python: the packaged builder has no Pillow / numpy).

The XBE section $$XTIMAGE holds an XPR0 resource: 'XPR0', u32 total size, u32 header size, then a D3D texture
header (Common, Data, Lock, Format, Size) whose pixels start at the header size. Gladius: 128 x 128 DXT1
(Format 0x07710C29: format byte 0x0C = DXT1, USize/VSize log2 = 7). Compressed Xbox textures are not swizzled.

Usage: python xbeicon.py default.xbe out.ico     (xbe2pe.py embeds the same images as the exe's icon)
"""
import struct, sys, zlib

ICON_SIZES = (16, 24, 32, 48, 64, 128)


def title_image(x):
    """(width, height, RGBA bytes) of the XBE's title image, or None."""
    sec = next((s for s in x.sections if s[0] == '$$XTIMAGE'), None)
    if not sec:
        return None
    b = x.data[sec[3]:sec[3] + sec[4]]
    if b[:4] != b'XPR0':
        return None
    total, hdr = struct.unpack_from('<II', b, 4)
    fmt = struct.unpack_from('<I', b, 12 + 12)[0]
    kind = (fmt >> 8) & 0xFF
    w, h = 1 << ((fmt >> 20) & 0xF), 1 << ((fmt >> 24) & 0xF)
    if kind == 0x0C:                                     # DXT1
        return w, h, dxt1_decode(b[hdr:hdr + w * h // 2], w, h)
    if kind == 0x0E:                                     # DXT3 (not used by Gladius; alpha ignored)
        return w, h, dxt_colour_blocks(b[hdr:hdr + w * h], w, h, 16, 8)
    return None


def _rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def dxt1_decode(data, w, h):
    return dxt_colour_blocks(data, w, h, 8, 0, dxt1=True)


def dxt_colour_blocks(data, w, h, block_bytes, colour_at, dxt1=False):
    out = bytearray(w * h * 4)
    i = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            c0, c1, bits = struct.unpack_from('<HHI', data, i + colour_at)
            i += block_bytes
            p0, p1 = _rgb565(c0), _rgb565(c1)
            if c0 > c1 or not dxt1:
                pal = [p0 + (255,), p1 + (255,),
                       tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
                       tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
            else:
                pal = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
            for py in range(4):
                for px in range(4):
                    o = ((by + py) * w + bx + px) * 4
                    out[o:o + 4] = bytes(pal[(bits >> (2 * (py * 4 + px))) & 3])
    return bytes(out)


def resize(rgba, w, h, nw, nh):
    """Area-average resample (downscaling); alpha-weighted so transparent texels do not darken edges."""
    out = bytearray(nw * nh * 4)
    for y in range(nh):
        y0, y1 = y * h / nh, (y + 1) * h / nh
        for x in range(nw):
            x0, x1 = x * w / nw, (x + 1) * w / nw
            acc = [0.0, 0.0, 0.0, 0.0]
            wsum = 0.0
            for sy in range(int(y0), min(h, int(y1 + 0.999))):
                fy = min(y1, sy + 1) - max(y0, sy)
                for sx in range(int(x0), min(w, int(x1 + 0.999))):
                    f = fy * (min(x1, sx + 1) - max(x0, sx))
                    if f <= 0:
                        continue
                    o = (sy * w + sx) * 4
                    a = rgba[o + 3] / 255.0
                    acc[0] += rgba[o] * a * f
                    acc[1] += rgba[o + 1] * a * f
                    acc[2] += rgba[o + 2] * a * f
                    acc[3] += a * f
                    wsum += f
            o = (y * nw + x) * 4
            if acc[3] > 0:
                out[o:o + 3] = bytes(min(255, int(c / acc[3] + 0.5)) for c in acc[:3])
            out[o + 3] = min(255, int(255 * acc[3] / wsum + 0.5)) if wsum else 0
    return bytes(out)


def png(rgba, w, h):
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def icon_images(x):
    """[(size, PNG bytes)] for ICON_SIZES from the title image, or [] when the XBE has none."""
    t = title_image(x)
    if not t:
        return []
    w, h, rgba = t
    return [(s, png(rgba if s == w else resize(rgba, w, h, s, s), s, s)) for s in ICON_SIZES if s <= w]


def ico_file(images):
    """.ico file bytes (PNG-compressed entries, Windows Vista+)."""
    d = struct.pack('<HHH', 0, 1, len(images))
    off = 6 + 16 * len(images)
    body = b''
    for s, p in images:
        d += struct.pack('<BBBBHHII', s & 0xFF, s & 0xFF, 0, 0, 1, 32, len(p), off + len(body))
        body += p
    return d + body


if __name__ == '__main__':
    from xbe import Xbe
    imgs = icon_images(Xbe(sys.argv[1]))
    if not imgs:
        sys.exit('no title image in ' + sys.argv[1])
    open(sys.argv[2], 'wb').write(ico_file(imgs))
    print(f'{sys.argv[2]}: sizes {[s for s, _ in imgs]}')

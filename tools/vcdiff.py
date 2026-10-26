"""VCDIFF (RFC 3284) decoder for xdelta3 patches - what GladiusPort Builder's Mods tab applies.

    python tools/vcdiff.py <source file> <patch.xdelta> <output file>

Supports the default code table, source / target windows and xdelta3's per-window Adler-32 checksum. Not supported:
xdelta3's secondary compressors (DJW / FGK / LZMA): make patches with "xdelta3 -e -S none ...".
"""
import mmap
import os
import struct
import sys
import zlib

VCD_DECOMPRESS, VCD_CODETABLE, VCD_APPHEADER = 1, 2, 4
VCD_SOURCE, VCD_TARGET, VCD_ADLER32 = 1, 2, 4
NOOP, ADD, RUN, COPY = 0, 1, 2, 3


class PatchError(Exception):
    pass


def _code_table():
    """RFC 3284 5.6 default instruction table: 256 x ((type1, size1, mode1), (type2, size2, mode2))"""
    t = [((RUN, 0, 0), (NOOP, 0, 0))]
    t += [((ADD, s, 0), (NOOP, 0, 0)) for s in range(0, 18)]
    for m in range(9):
        t += [((COPY, 0, m), (NOOP, 0, 0))] + [((COPY, s, m), (NOOP, 0, 0)) for s in range(4, 19)]
    for m in range(6):
        t += [((ADD, a, 0), (COPY, c, m)) for a in range(1, 5) for c in range(4, 7)]
    for m in range(6, 9):
        t += [((ADD, a, 0), (COPY, 4, m)) for a in range(1, 5)]
    t += [((COPY, 4, m), (ADD, 1, 0)) for m in range(9)]
    assert len(t) == 256
    return t


CODES = _code_table()


def _int(b, p):
    v = 0
    while True:
        c = b[p]
        p += 1
        v = (v << 7) | (c & 0x7F)
        if not c & 0x80:
            return v, p


def adler32(d):
    return zlib.adler32(d) & 0xFFFFFFFF


def apply(src_path, patch_path, out_path, progress=None):
    """Decode <patch_path> against <src_path> into <out_path> (written via a temp file, replaced on success)."""
    patch = open(patch_path, 'rb').read()
    if patch[:4] != b'\xd6\xc3\xc4\x00':
        raise PatchError(f'{patch_path}: not a VCDIFF / xdelta3 patch')
    ind, p = patch[4], 5
    if ind & VCD_DECOMPRESS:
        raise PatchError(f'{patch_path}: uses xdelta3 secondary compression (remake it with -S none)')
    if ind & VCD_CODETABLE:
        raise PatchError(f'{patch_path}: custom code table not supported')
    if ind & VCD_APPHEADER:
        n, p = _int(patch, p)
        p += n
    tmp = out_path + '.part'
    sf = open(src_path, 'rb')
    size = os.path.getsize(src_path)
    src = mmap.mmap(sf.fileno(), 0, access=mmap.ACCESS_READ) if size else b''
    try:
        with open(tmp, 'w+b') as out:
            written = 0
            while p < len(patch):
                wind = patch[p]
                p += 1
                seg = b''
                if wind & (VCD_SOURCE | VCD_TARGET):
                    slen, p = _int(patch, p)
                    spos, p = _int(patch, p)
                    if wind & VCD_SOURCE:
                        if spos + slen > size:
                            raise PatchError('patch reads past the end of the source file - wrong source file?')
                        seg = src[spos:spos + slen]
                    else:
                        out.seek(spos)
                        seg = out.read(slen)
                        out.seek(written)
                _, p = _int(patch, p)                      # length of the delta encoding
                tlen, p = _int(patch, p)
                dind = patch[p]
                p += 1
                if dind:
                    raise PatchError(f'{patch_path}: compressed sections not supported (remake it with -S none)')
                dlen, p = _int(patch, p)
                ilen, p = _int(patch, p)
                alen, p = _int(patch, p)
                cksum = None
                if wind & VCD_ADLER32:
                    cksum = struct.unpack_from('>I', patch, p)[0]
                    p += 4
                data, inst, addr = p, p + dlen, p + dlen + ilen
                p = addr + alen
                tgt = _window(patch, data, inst, addr, p, seg, tlen)
                if cksum is not None and adler32(tgt) != cksum:
                    raise PatchError('checksum mismatch - the file being patched is not the one the patch expects')
                out.write(tgt)
                written += len(tgt)
                if progress:
                    progress(p, len(patch))
    except BaseException:
        if os.path.exists(tmp):
            os.remove(tmp)
        raise
    finally:
        if size:
            src.close()
        sf.close()
    os.replace(tmp, out_path)


def _window(patch, dp, ip, ap, end_addr, seg, tlen):
    near, same, nxt = [0] * 4, [0] * (3 * 256), 0
    slen = len(seg)
    t = bytearray()
    iend = ap

    def copy(size, mode):
        nonlocal ap, nxt
        here = slen + len(t)
        if mode == 0:
            a, ap = _int(patch, ap)
        elif mode == 1:
            v, ap = _int(patch, ap)
            a = here - v
        elif mode < 6:
            v, ap = _int(patch, ap)
            a = near[mode - 2] + v
        else:
            a = same[(mode - 6) * 256 + patch[ap]]
            ap += 1
        near[nxt] = a
        nxt = (nxt + 1) % 4
        same[a % (3 * 256)] = a
        if a + size <= slen:                                # all from the source segment
            t.extend(seg[a:a + size])
            return
        if a >= slen and a + size <= here:                  # all from target data already decoded
            t.extend(t[a - slen:a - slen + size])
            return
        for i in range(size):                               # target data, may overlap what it writes
            x = a + i
            t.append(seg[x] if x < slen else t[x - slen])

    while ip < iend:
        code = patch[ip]
        ip += 1
        for typ, size, mode in CODES[code]:
            if typ == NOOP:
                continue
            if size == 0:
                size, ip = _int(patch, ip)
            if typ == ADD:
                t.extend(patch[dp:dp + size])
                dp += size
            elif typ == RUN:
                t.extend(bytes([patch[dp]]) * size)
                dp += 1
            else:
                copy(size, mode)
    if len(t) != tlen or ap != end_addr:
        raise PatchError('corrupt patch window')
    return bytes(t)


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    apply(*sys.argv[1:])
    print('ok')

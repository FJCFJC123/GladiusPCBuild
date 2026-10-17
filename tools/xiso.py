"""Minimal XDVDFS (Xbox ISO) reader: list / extract files. Usage: xiso.py image.iso [list|extract outdir]"""
import struct, sys, os

SECTOR = 2048

class XIso:
    def __init__(self, path, base=0):
        self.f = open(path, 'rb')
        self.base = base
        self.f.seek(base + 32 * SECTOR)
        hdr = self.f.read(SECTOR)
        assert hdr[:20] == b'MICROSOFT*XBOX*MEDIA', 'not an XDVDFS image'
        self.root_sector, self.root_size = struct.unpack_from('<II', hdr, 20)

    def read(self, sector, size, off=0):
        self.f.seek(self.base + sector * SECTOR + off)
        return self.f.read(size)

    def walk(self, sector=None, size=None, prefix=''):
        if sector is None:
            sector, size = self.root_sector, self.root_size
        if not size:
            return
        data = self.read(sector, size)
        stack = [0]
        seen = set()
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(data):
                continue
            seen.add(off)
            l, r, s, sz, attr, nlen = struct.unpack_from('<HHIIBB', data, off)
            if l == 0xFFFF and r == 0xFFFF:
                continue
            name = data[off + 14:off + 14 + nlen].decode('latin1')
            path = prefix + name
            if attr & 0x10:
                yield path + '/', s, sz, True
                yield from self.walk(s, sz, path + '/')
            else:
                yield path, s, sz, False
            if l:
                stack.append(l * 4)
            if r:
                stack.append(r * 4)

    def extract(self, out):
        for path, s, sz, is_dir in self.walk():
            dst = os.path.join(out, path)
            if is_dir:
                os.makedirs(dst, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as o:
                left, sec = sz, s
                while left:
                    n = min(left, 64 * SECTOR * 16)
                    o.write(self.read(sec, n))
                    sec += n // SECTOR
                    left -= n
            print(f'{sz:12,d}  {path}')


if __name__ == '__main__':
    iso = XIso(sys.argv[1])
    if len(sys.argv) > 3 and sys.argv[2] == 'extract':
        iso.extract(sys.argv[3])
    else:
        for path, s, sz, d in iso.walk():
            print(f'{"<dir>" if d else f"{sz:12,d}"}  {path}')

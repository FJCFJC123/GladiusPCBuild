"""XBE loader helpers for the Gladius PC port (retail default.xbe, XDK 5558)."""
import struct

XBE_PATH = 'default.xbe'                            # default: the current folder
BASE = 0x10000
KTHUNK = 0x33B160


class Xbe:
    def __init__(self, path=XBE_PATH):
        self.data = open(path, 'rb').read()
        d = self.data
        n = struct.unpack_from('<I', d, 0x11C)[0]
        sa = struct.unpack_from('<I', d, 0x120)[0] - BASE
        self.sections = []
        for i in range(n):
            o = sa + i * 56
            fl, va, vs, ra, rs, na = struct.unpack_from('<IIIIII', d, o)
            name = d[na - BASE:na - BASE + 16].split(b'\0')[0].decode()
            self.sections.append((name, va, vs, ra, rs, fl))

    def section_of(self, va):
        for s in self.sections:
            if s[1] <= va < s[1] + s[2]:
                return s
        return None

    def read(self, va, n):
        s = self.section_of(va)
        if not s:
            hdr = struct.unpack_from('<I', self.data, 0x108)[0]          # SizeOfHeaders
            if BASE <= va < BASE + hdr:
                return self.data[va - BASE:va - BASE + n]
            return None
        off = va - s[1]
        if off >= s[4]:
            return bytes(n)                     # bss
        return self.data[s[3] + off:s[3] + off + n]

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]

    def section_bytes(self, name):
        s = next(s for s in self.sections if s[0] == name)
        return s[1], self.data[s[3]:s[3] + s[4]]

    def kernel_thunks(self):
        """[(slot_va, ordinal)] in table order."""
        out, va = [], KTHUNK
        while True:
            v = self.u32(va)
            if not v:
                return out
            out.append((va, v & 0x7FFFFFFF))
            va += 4

"""Convert the retail Gladius default.xbe into a native Windows PE executable.

Layout (no code moves, no relocations):
  0x00010000  PE headers (the XBE header page is only read by XAPI/XPP/D3D, which are replaced)
  0x00011000  .xbe   one RWX section = every XBE section at its original address (incl. .data bss)
  PORT_VA     .port  import directory + names, TLS directory, startup stub, patch cave

The XBE kernel thunk table (0x33B160, 162 slots) becomes the import address table of
gladiusport.dll: export krnl_<Name> per slot. The DLL forwards the few kernel calls app code makes
(critical sections) to ntdll and traps everything else, so a missed replacement is reported by name.
The XBE TLS block (0x90 zero-fill, index variable 0x418B8C) becomes a normal PE TLS directory.
Entry point = .port stub -> gladiusport!port_start(GAME_MAIN, info).

Usage: python xbe2pe.py [default.xbe] [out.exe]
"""
import struct, sys, os
from xbe import Xbe, BASE, KTHUNK

DLL = b'gladiusport.dll'
SECT_ALIGN, FILE_ALIGN = 0x1000, 0x200
XBE_LO = 0x11000                     # first page after the PE headers
GAME_MAIN = 0x6DC50                  # main(0,0,0) as called by XAPI mainXapiStartup
RT_INIT = (0x37B840, 0x37B84C)       # XAPI _rtinit table: CRT _mtinit 0x277A02, _ioinit 0x28118D
CXX_INIT = (0x37B850, 0x37BECC)      # __xc_a..__xc_z (C++ static constructors)
C_INIT = (0x37BED0, 0x37BEE4)        # __xi_a..__xi_z
FPINIT_PTR = 0x3891D0                # _FPinit pointer checked first by _cinit
CAVE_SIZE = 0x40000                  # room for patch stubs (.port)

def align(v, a):
    return (v + a - 1) & ~(a - 1)


def resource_section(icons, rva):
    """.rsrc contents for the exe icon: RT_ICON 1..n (PNG images) + RT_GROUP_ICON 1, at section RVA `rva`.
    icons = [(size, PNG bytes)] (xbeicon.icon_images). Tree: type -> id -> language (0x409) -> data entry."""
    LANG = 0x409
    group = struct.pack('<HHH', 0, 1, len(icons)) + b''.join(
        struct.pack('<BBBBHHIH', s & 0xFF, s & 0xFF, 0, 0, 1, 32, len(p), i + 1) for i, (s, p) in enumerate(icons))
    tree = {3: {i + 1: p for i, (s, p) in enumerate(icons)}, 14: {1: group}}     # RT_ICON, RT_GROUP_ICON
    def dir_hdr(n):
        return struct.pack('<IIHHHH', 0, 0, 0, 0, 0, n)
    # sizes: root + per type (dir + entries) + per id (lang dir + 1 entry) + data entries
    leaves = [(t, i) for t in sorted(tree) for i in sorted(tree[t])]
    root_sz = 16 + 8 * len(tree)
    type_sz = {t: 16 + 8 * len(tree[t]) for t in tree}
    dirs_sz = root_sz + sum(type_sz.values()) + len(leaves) * (16 + 8)
    data_entry_at = dirs_sz
    blob_at = align(data_entry_at + 16 * len(leaves), 8)
    out = bytearray(dir_hdr(len(tree)))
    type_off, at = {}, root_sz
    for t in sorted(tree):
        type_off[t] = at
        at += type_sz[t]
    lang_off, at2 = {}, at
    for leaf in leaves:
        lang_off[leaf] = at2
        at2 += 24
    for t in sorted(tree):                                        # root entries
        out += struct.pack('<II', t, 0x80000000 | type_off[t])
    for t in sorted(tree):                                        # type directories
        out += dir_hdr(len(tree[t]))
        for i in sorted(tree[t]):
            out += struct.pack('<II', i, 0x80000000 | lang_off[(t, i)])
    blobs, blob_pos = bytearray(), {}
    for n, leaf in enumerate(leaves):                              # language directories -> data entries
        out += dir_hdr(1) + struct.pack('<II', LANG, data_entry_at + 16 * n)
        blob_pos[leaf] = blob_at + len(blobs)
        blobs += tree[leaf[0]][leaf[1]]
        while len(blobs) % 8:
            blobs.append(0)
    for leaf in leaves:                                           # data entries (OffsetToData is an RVA)
        out += struct.pack('<IIII', rva + blob_pos[leaf], len(tree[leaf[0]][leaf[1]]), 0, 0)
    out += bytes(blob_at - len(out))
    return bytes(out + blobs)


def build(xbe_path, out_path, gui=False):
    """gui=True: Windows subsystem (no console window)."""
    from kernel_names import KERNEL_IMPORTS as names
    x = Xbe(xbe_path)
    thunks = x.kernel_thunks()
    assert len(thunks) == len(names) == 162

    # ---- image of all XBE sections at their addresses
    hi = max(s[1] + s[2] for s in x.sections)
    xbe_vsize = align(hi, SECT_ALIGN) - XBE_LO
    img = bytearray(xbe_vsize)
    init_end = XBE_LO
    hdr_size = struct.unpack_from('<I', x.data, 0x108)[0]
    img[0:hdr_size - 0x1000] = x.data[0x1000:hdr_size]            # XBE header page 2 (harmless copy)
    for name, va, vs, ra, rs, fl in x.sections:
        img[va - XBE_LO:va - XBE_LO + rs] = x.data[ra:ra + rs]
        init_end = max(init_end, va + rs)
    raw_xbe = bytes(img[:align(init_end - XBE_LO, FILE_ALIGN)])

    # ---- .port section
    PORT_VA = XBE_LO + xbe_vsize
    port = bytearray()
    def here():
        return PORT_VA + len(port)
    def put(b):
        a = here()
        port.extend(b)
        return a
    def pad(n=4):
        while len(port) % n:
            port.append(0)

    # hint/name table
    hint_names = []
    for n in names:
        pad(2)
        hint_names.append(put(struct.pack('<H', 0) + b'krnl_' + n.encode() + b'\0'))
    pad(4)
    dll_name = put(DLL + b'\0')
    pad(4)
    # INT (OriginalFirstThunk) - the IAT is the XBE thunk table itself
    int_va = put(b''.join(struct.pack('<I', a - BASE) for a in hint_names) + b'\0\0\0\0')
    for i, a in enumerate(hint_names):                              # IAT initial contents = INT
        struct.pack_into('<I', img, KTHUNK + 4 * i - XBE_LO, a - BASE)
    raw_xbe = bytes(img[:align(init_end - XBE_LO, FILE_ALIGN)])

    # TLS directory (same index variable / zero fill as the XBE)
    tls_x = struct.unpack_from('<I', x.data, 0x12C)[0]
    _, _, tls_index, _, tls_zero, _ = struct.unpack('<6I', x.read(tls_x, 24))
    # The Windows loader sizes each thread's TLS block from the raw template only and ignores SizeOfZeroFill:
    # an empty template gave every thread a 0-byte block, and the game CRT's per-thread data (block+0xC..0x90)
    # was written into neighbouring heap blocks (the _getptd / small-object heap corruption). So the XBE's zero
    # fill becomes an explicit zero template.
    pad(16)
    tls_raw = put(bytes(max(tls_zero, 16)))                         # zero-filled template, tls_zero bytes
    tls_cb = put(bytes(8))                                          # no callbacks
    tls_dir = put(struct.pack('<6I', tls_raw, tls_raw + tls_zero, tls_index, tls_cb, 0, 0))

    # XBE section headers (56 bytes each, name pointers re-pointed into .port) for XGetSectionHandleA:
    # sections are always mapped here, so a handle is just this header (+4 address, +8 size)
    nsec = struct.unpack_from('<I', x.data, 0x11C)[0]
    shdr_va = struct.unpack_from('<I', x.data, 0x120)[0]
    shdrs = bytearray(x.read(shdr_va, 56 * nsec))
    for i in range(nsec):
        name_va = struct.unpack_from('<I', shdrs, i * 56 + 0x14)[0]
        nm = x.read(name_va, 64).split(b'\x00')[0]
        struct.pack_into('<I', shdrs, i * 56 + 0x14, put(nm + b'\x00'))
    pad(16)
    sect_table = put(bytes(shdrs))

    # port info block passed to the DLL
    pad(16)
    info = put(struct.pack('<12I', 0x54524F50, GAME_MAIN, CXX_INIT[0], CXX_INIT[1], C_INIT[0], C_INIT[1],
                           FPINIT_PTR, 0, RT_INIT[0], RT_INIT[1], sect_table, nsec))   # 'PORT'
    # gladiusport.dll!port_start gets its own tiny IAT (the kernel IAT is the fixed 162-slot XBE table)
    # second IAT: port_start + every replaced XAPI function (tools/replacements.py)
    from replacements import REPLACE
    port_names = [b'port_start'] + [n.encode() for a, n, argb, note in REPLACE]
    name_vas = []
    for n in port_names:
        pad(2)
        name_vas.append(put(struct.pack('<H', 0) + n + b'\0'))
    pad(4)
    ps_int = put(b''.join(struct.pack('<I', a - BASE) for a in name_vas) + bytes(4))
    ps_iat = put(b''.join(struct.pack('<I', a - BASE) for a in name_vas) + bytes(4))
    # import directory: two descriptors for the same DLL (legal), kernel slots + port functions
    pad(16)
    imp_dir = put(struct.pack('<IIIII', int_va - BASE, 0, 0, dll_name - BASE, KTHUNK - BASE) +
                  struct.pack('<IIIII', ps_int - BASE, 0, 0, dll_name - BASE, ps_iat - BASE) + bytes(20))
    imp_size = 60
    pad(16)
    entry = put(b'\x68' + struct.pack('<I', info) +                 # push info
                b'\xFF\x15' + struct.pack('<I', ps_iat) +           # call [port_start]
                b'\xC3')                                            # ret (port_start never returns)

    # stand-in for the Xbox KTHREAD read by CRT _getptd (only +0x28 TlsData != 0 is checked)
    pad(16)
    fake_kthread = put(struct.pack('<I', 0) * 10 + struct.pack('<I', 1) + bytes(0x100 - 44))

    # in-place byte patches (replacements.PATCHES)
    from replacements import PATCHES
    for a, old, new, note in PATCHES:
        new = new.replace('FAKE_KTHREAD', struct.pack('<I', fake_kthread).hex())
        old_b, new_b = bytes.fromhex(old), bytes.fromhex(new)
        assert len(old_b) == len(new_b), hex(a)
        cur = bytes(img[a - XBE_LO:a - XBE_LO + len(old_b)])
        assert cur == old_b, f'patch {a:X}: found {cur.hex()} expected {old}'
        img[a - XBE_LO:a - XBE_LO + len(new_b)] = new_b

    # replaced functions: entry -> jmp rel32 (5 bytes, fits even 5-byte thunks) -> .port trampoline jmp [slot]
    addrs = sorted(a for a, n, argb, note in REPLACE)
    assert len(set(addrs)) == len(addrs), 'duplicate replacement entries'
    assert all(b - a >= 5 for a, b in zip(addrs, addrs[1:])), 'replacement entries overlap'
    pad(16)
    for i, (a, n, argb, note) in enumerate(REPLACE):
        slot = ps_iat + 4 * (i + 1)
        tramp = put(b'\xFF\x25' + struct.pack('<I', slot) + b'\xCC\xCC')
        img[a - XBE_LO:a - XBE_LO + 5] = b'\xE9' + struct.pack('<i', tramp - (a + 5))
    raw_xbe = bytes(img[:align(init_end - XBE_LO, FILE_ALIGN)])
    pad(16)
    cave = here()
    port.extend(bytes(CAVE_SIZE))
    port_vsize = align(len(port), SECT_ALIGN)
    raw_port = bytes(port) + bytes(align(len(port), FILE_ALIGN) - len(port))

    # ---- .rsrc: the exe icon from the XBE's title image ($$XTIMAGE), when it has one
    from xbeicon import icon_images
    icons = icon_images(x)
    RSRC_VA = PORT_VA + port_vsize
    raw_rsrc = resource_section(icons, RSRC_VA - BASE) if icons else b''
    rsrc_vsize = align(len(raw_rsrc), SECT_ALIGN)
    raw_rsrc += bytes(align(len(raw_rsrc), FILE_ALIGN) - len(raw_rsrc))

    # ---- PE headers
    nsec = 3 if icons else 2
    hdr_raw = align(0x40 + 4 + 20 + 224 + 40 * nsec, FILE_ALIGN)
    xbe_ptr = hdr_raw
    port_ptr = xbe_ptr + len(raw_xbe)
    rsrc_ptr = port_ptr + len(raw_port)
    size_image = PORT_VA + port_vsize + rsrc_vsize - BASE

    stack_reserve = 0x100000
    dos = bytearray(0x40)
    dos[0:2] = b'MZ'
    struct.pack_into('<I', dos, 0x3C, 0x40)
    coff = struct.pack('<4sHHIIIHH', b'PE\0\0', 0x14C, nsec, 0, 0, 0, 224,
                       0x0001 | 0x0002 | 0x0020 | 0x0100)          # relocs stripped, exe, large-address-aware, 32-bit
    dirs = [(0, 0)] * 16
    dirs[1] = (imp_dir - BASE, imp_size)                             # import
    if icons:
        dirs[2] = (RSRC_VA - BASE, rsrc_vsize)                       # resources (icon)
    dirs[9] = (tls_dir - BASE, 24)                                   # TLS
    dirs[12] = (KTHUNK - BASE, 163 * 4)                              # IAT
    opt = struct.pack('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII',
                      0x10B, 7, 10, len(raw_xbe), 0, 0, entry - BASE, XBE_LO - BASE, XBE_LO - BASE,
                      BASE, SECT_ALIGN, FILE_ALIGN, 5, 1, 0, 0, 5, 1, 0,
                      size_image, hdr_raw, 0, 2 if gui else 3,      # subsystem 2 = windows, 3 = console
                      0x0000,                                       # no ASLR / NX
                      stack_reserve, 0x10000, 0x100000, 0x1000, 0, 16)
    opt += b''.join(struct.pack('<II', a, s) for a, s in dirs)
    assert len(opt) == 224
    def sect(name, va, vsize, raw_size, ptr, chars):
        return struct.pack('<8sIIIIIIHHI', name, vsize, va - BASE, raw_size, ptr, 0, 0, 0, 0, chars)
    RWX = 0x20 | 0x40 | 0x20000000 | 0x40000000 | 0x80000000
    secs = (sect(b'.xbe', XBE_LO, xbe_vsize, len(raw_xbe), xbe_ptr, RWX) +
            sect(b'.port', PORT_VA, port_vsize, len(raw_port), port_ptr, RWX))
    if icons:
        secs += sect(b'.rsrc', RSRC_VA, rsrc_vsize, len(raw_rsrc), rsrc_ptr, 0x40 | 0x40000000)   # data, read
    headers = bytes(dos) + coff + opt + secs
    headers += bytes(hdr_raw - len(headers))
    out = headers + raw_xbe + raw_port + raw_rsrc
    open(out_path, 'wb').write(out)

    layout = {'PORT_VA': PORT_VA, 'entry': entry, 'info': info, 'cave': cave, 'cave_size': CAVE_SIZE,
              'image_end': PORT_VA + port_vsize}
    with open(os.path.splitext(out_path)[0] + '.layout.txt', 'w') as f:
        for k, v in layout.items():
            f.write(f'{k} = 0x{v:X}\n')
    print(f'{out_path}: {len(out):,} bytes, image 0x{BASE:X}-0x{BASE + size_image:X}, '
          f'.port 0x{PORT_VA:X}, entry 0x{entry:X}, cave 0x{cave:X}, icon {[s for s, _ in icons] or "none"}')
    return layout


if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\ob\Desktop\Protius\default.xbe'
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', 'build', 'gladius.exe')
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    build(src, dst)

"""Every Xbox D3D8 entry point the game/engine/CRI code calls -> replaced by gladiusport.dll (d3d_<Name>).

Built from XbSymbolDatabase output (conventions/argument counts for XDK 5558) restricted to targets listed in
inventory/boundary.txt, plus the few unnamed ones identified by hand. (addr, name, conv, stack_args, register_args)
"""
import os, re

HERE = os.path.dirname(__file__)

MANUAL = [
    (0x2AE040, 'D3D_Get2DSurfaceDesc', 'stdcall', 3, []),      # jmp 0x2B3410 (container, level, desc), ret 0xC
    (0x2B01F0, 'D3DDevice_CreatePushBuffer2', 'stdcall', 2, []),   # size(+0x18 header), flag; Common 0x81020001
    (0x2B1050, 'D3D_CreateSaveSurface', 'stdcall', 4, []),     # engine-save; ret 0x10
]
KEEP_NATIVE = {0x2AE8D0}                                       # returns 1, pure


def _used_targets(prefixes):
    used = set()
    for line in open(os.path.join(HERE, '..', 'inventory', 'boundary.txt')):
        a, n = int(line.split()[0], 16), line.split()[1]
        if n.startswith(prefixes):
            used.add(a)
    return used


def _from_symbols(lib, used):
    """(addr, name, conv, stack_dwords, regs) for XbSymbolDatabase functions of `lib` in `used`.
    'psh2' marks an 8-byte by-value argument (two stack dwords)."""
    rows = {}
    for line in open(os.path.join(HERE, '..', 'inventory', 'xbsymbols_raw.txt')):
        m = re.match(lib + r'__FUN__(\w+?)__(\w+)\((.*)\) = 0x([0-9a-f]+)', line.strip())
        if not m:
            continue
        conv, name, args, addr = m.groups()
        addr = int(addr, 16)
        if addr in used:
            a = [x.strip() for x in args.split(',') if x.strip()]
            stack = sum(2 if x.startswith('psh2') else 1 for x in a if x.startswith('psh'))
            rows[addr] = (addr, name, conv, stack, [x.split()[0] for x in a if not x.startswith('psh')])
    return rows


def entries():
    rows = _from_symbols('D3D8', _used_targets(('D3D8!', 'D3D!')))
    for e in MANUAL:
        rows[e[0]] = e
    return [rows[a] for a in sorted(rows) if a not in KEEP_NATIVE]


def dsound_entries():
    rows = _from_symbols('DSOUND', _used_targets(('DSOUND!',)))
    return [rows[a] for a in sorted(rows)]


if __name__ == '__main__':
    for e in entries():
        print(e)

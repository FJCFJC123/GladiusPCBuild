"""Port a GameCube Gladius mod's audio.bec changes onto the Xbox audio (data\\Audio\\XBOX).

    python tools/gcaudio.py <GC vanilla audio.bec> <GC mod audio.bec> <Xbox Audio\\XBOX dir> <names .py> <out dir>
                            [--xdelta <xdelta3.exe>]

Both platforms describe each sound group with a text .flo (GC data/audio/gc/<grp>/<name>/g_<name>.flo, Xbox
Audio\\XBOX\\<Grp>\\<Name>\\X_<NAME>.FLO); event names, event order and the SoundDataFiles -> SimpleEvents ->
EventMaps chain match, the indices (links) do not always. For every flo the mod changes, the lines it appends
(SoundDataFiles, SimpleEvents, EventMaps; any other edit is refused) are appended to the Xbox flo, translated:
  - a new sound (a GC .dsp in the group's .dsh bundle) must be a byte copy of a vanilla GC clip: the Xbox copy of that
    clip (found by its event name in its own group's Xbox flo) is appended to the group's .XSB, with its .XBB header.
  - an event map pointing at an existing GC event points at the Xbox event an existing name with the same GC link
    has on Xbox (General*Galdr_FireAffinity_IS -> the link General*Archer_FireAffinity_IS uses on Xbox).
GC files the game never loads (repack copies under made-up names) are listed, not ported.
Out: <out dir>/Audio/XBOX/... (the changed Xbox files), gcaudio_report.txt, and with --xdelta one .xdelta per file
beside it (source = the Xbox file).
<names .py>: the audio.bec name list ("0X<hash>": "data/audio/...", audiobechashes_corrected.py).
"""
import collections
import hashlib
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from becdiff import path_hash  # noqa: E402  (audio.bec uses the same hash)


# ------------------------------------------------------------------------------------------------ audio.bec
class AudioBec:
    """audio.bec: ' ceb' header like gladius.bec; 16-byte entries (hash, offset, flags << 24, size); data raw."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        hd = self.f.read(16)
        n = struct.unpack_from('<I', hd, 8)[0]
        raw = self.f.read(n * 16)
        self.ents = {}
        for i in range(n):
            h, off, _, sz = struct.unpack_from('<4I', raw, i * 16)
            self.ents[h] = (off, sz)

    def get(self, h):
        off, sz = self.ents[h]
        self.f.seek(off)
        return self.f.read(sz)


# ------------------------------------------------------------------------------------------------ flo text
HEADERS = ('SoundDataPath', 'SoundDataBundles', 'SoundDataFiles', 'SoundParameterSets', 'SimpleEvents',
           'RandomEvents', 'CompoundEvents', 'EventMaps')
SUBS = ('Pre', 'Pan', 'Pos')


def flo_sections(text):
    """[(section, [line, ...])], lines without line ends; EventMaps / SoundParameterSets split into
    'EventMaps/Pos' etc. Each section's lines: the count line, then its entries (blank lines kept)."""
    out, cur, top = [], ('', []), ''
    for line in text.split('\n'):
        s = line.strip()
        if s in HEADERS:
            top = s
            out.append(cur)
            cur = (s, [])
        elif s in SUBS and top in ('EventMaps', 'SoundParameterSets'):
            out.append(cur)
            cur = (f'{top}/{s}', [])
        else:
            cur[1].append(line)
    out.append(cur)
    return out


def entries(lines):
    return [ln for ln in lines if re.match(r'\s*\d+,', ln)]


def count_line(lines):
    for i, ln in enumerate(lines):
        if ln.strip():
            return i
    return None


def emap(line):
    m = re.match(r'\s*(\d+),\s*(\d+),\s*(\d+),\s*(\S+)', line)
    return int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)


def fields(line):
    return [f.strip() for f in line.split(',')]


def appended(van, mod, name):
    """the entry lines the mod adds at the end of section <name>; refuse anything else"""
    v, m = entries(dict(van)[name]), entries(dict(mod)[name])
    if m[:len(v)] != v:
        raise SystemExit(f'{name}: the mod edits existing lines - not supported')
    return m[len(v):]


# ------------------------------------------------------------------------------------------------ Xbox banks
class XboxGroup:
    """One Xbox sound group: X_<NAME>.FLO + <NAME>.XBB (u32 size - 4, u32 count, RIFF/WAVE headers: data size
    0xFFFFFFF8 = streamed, then u32 offset, u32 length into <NAME>.XSB; else the data inline) + <NAME>.XSB."""

    def __init__(self, root, grp, name):
        d = find_dir(root, grp, name)
        self.dir = d
        files = {f.lower(): f for f in os.listdir(d)}
        self.flo_name = files[f'x_{name}.flo']
        self.xbb_name = files.get(f'{name}.xbb')
        self.xsb_name = files.get(f'{name}.xsb')
        self.flo_raw = open(os.path.join(d, self.flo_name), 'rb').read()
        self.crlf = b'\r\n' in self.flo_raw
        self.secs = flo_sections(self.flo_raw.decode('latin-1').replace('\r', ''))
        self.xbb = bytearray(open(os.path.join(d, self.xbb_name), 'rb').read()) if self.xbb_name else None
        self.xsb = bytearray(open(os.path.join(d, self.xsb_name), 'rb').read()) if self.xsb_name else None
        self.ents = []
        if self.xbb is not None:
            n, o = struct.unpack_from('<I', self.xbb, 4)[0], 8
            for _ in range(n):
                dsz = struct.unpack_from('<I', self.xbb, o + 44)[0]
                if dsz == 0xFFFFFFF8:
                    self.ents.append((o, 56))
                    o += 56
                else:
                    ln = 48 + dsz + (dsz & 1)
                    self.ents.append((o, ln))
                    o += ln
            if o != len(self.xbb):
                raise SystemExit(f'{self.xbb_name}: unexpected layout')
        self.changed = set()
        if self.flo_bytes() != self.flo_raw:
            raise SystemExit(f'{self.flo_name}: does not read back byte for byte')

    def sec(self, name):
        return dict(self.secs)[name]

    def sound(self, event):
        """(RIFF header, ADPCM data) of the sound simple event map <event> plays"""
        for ln in entries(self.sec('EventMaps/Pos')):
            _, t, link, nm = emap(ln)
            if nm.split('*')[-1].upper() == event.upper():
                break
        else:
            raise SystemExit(f'{self.flo_name}: no event {event}')
        if t != 0:
            raise SystemExit(f'{self.flo_name}: {event} is not a simple event')
        simple = {int(f[0]): int(f[1]) for f in map(fields, entries(self.sec('SimpleEvents')))}
        files = {int(f[0]): f[2] for f in map(fields, entries(self.sec('SoundDataFiles')))}
        slot = int(re.match(r'(\d+)', files[simple[link]]).group(1))
        o, ln = self.ents[slot]
        hdr = bytes(self.xbb[o:o + 56])
        if struct.unpack_from('<I', hdr, 44)[0] == 0xFFFFFFF8:
            off, size = struct.unpack_from('<2I', hdr, 48)
            return hdr[:48], bytes(self.xsb[off:off + size])
        dsz = struct.unpack_from('<I', hdr, 44)[0]
        return bytes(self.xbb[o:o + 40]) + bytes(self.xbb[o + 40:o + 48]), bytes(self.xbb[o + 48:o + 48 + dsz])

    def add_sound(self, hdr, data):
        """append a streamed sound; returns its slot"""
        h = bytearray(hdr[:48])
        struct.pack_into('<I', h, 4, 0x30)                    # RIFF size of a streamed header
        struct.pack_into('<I', h, 44, 0xFFFFFFF8)
        h += struct.pack('<2I', len(self.xsb), len(data))
        self.xsb += data
        self.ents.append((len(self.xbb), 56))
        self.xbb += h
        struct.pack_into('<2I', self.xbb, 0, len(self.xbb) - 4, len(self.ents))
        self.changed |= {self.xbb_name, self.xsb_name}
        return len(self.ents) - 1

    def append(self, name, lines):
        for i, (s, ls) in enumerate(self.secs):
            if s == name:
                c = count_line(ls)
                last = max(j for j, ln in enumerate(ls) if re.match(r'\s*\d+,', ln)) if entries(ls) else c
                ls[last + 1:last + 1] = lines
                ls[c] = str(int(ls[c]) + len(lines))
                self.changed.add(self.flo_name)
                return
        raise SystemExit(f'{self.flo_name}: no section {name}')

    def flo_bytes(self):
        out = []
        for s, ls in self.secs:
            if s:
                out.append(s.split('/')[-1])
            out += ls
        t = '\n'.join(out)
        return (t.replace('\n', '\r\n') if self.crlf else t).encode('latin-1')


def find_dir(root, *parts):
    d = root
    for p in parts:
        sub = {x.lower(): x for x in os.listdir(d)}
        if p.lower() not in sub:
            raise SystemExit(f'no {p} under {d}')
        d = os.path.join(d, sub[p.lower()])
    return d


# ------------------------------------------------------------------------------------------------ main
def main():
    argv = sys.argv[1:]
    xdelta = None
    if '--xdelta' in argv:
        i = argv.index('--xdelta')
        xdelta, argv = argv[i + 1], argv[:i] + argv[i + 2:]
    if len(argv) != 5:
        raise SystemExit(__doc__)
    van, mod = AudioBec(argv[0]), AudioBec(argv[1])
    xroot, out = argv[2], argv[4]
    src = open(argv[3], encoding='latin-1').read()
    names = {int(h, 16): n.lower() for h, n in re.findall(r'0x([0-9A-Fa-f]{8})\s*:\s*"([^"]+)"', src, re.I)}
    report = []

    van_md5 = collections.defaultdict(list)                  # vanilla clips by content
    for h in van.ents:
        if names.get(h, '').endswith('.dsp'):
            van_md5[hashlib.md5(van.get(h)).digest()].append(names[h])

    changed = [h for h in mod.ents if h not in van.ents or van.get(h) != mod.get(h)]
    flos = sorted(names[h] for h in changed if names.get(h, '').endswith('.flo'))
    groups, used = {}, set()

    def group(grp, name):
        k = (grp, name)
        if k not in groups:
            groups[k] = XboxGroup(xroot, grp, name)
        return groups[k]

    for flo in flos:
        m = re.match(r'data/audio/gc/([^/]+)/([^/]+)/g_([^/]+)\.flo$', flo)
        if not m or m.group(2) != m.group(3):
            report.append(f'not ported\t{flo}\tnot a group flo')
            continue
        grp, name = m.group(1), m.group(2)
        used.add(path_hash(flo))
        vtext = van.get(path_hash(flo)).decode('latin-1').replace('\r', '')
        mtext = mod.get(path_hash(flo)).decode('latin-1').replace('\r', '')
        vs, ms = flo_sections(vtext), flo_sections(mtext)
        for (a, la), (b, lb) in zip(vs, ms):
            if a != b or (a not in ('SoundDataFiles', 'SimpleEvents', 'EventMaps/Pos') and entries(la) != entries(lb)):
                raise SystemExit(f'{flo}: {a or b} changed - not supported')
        xg = group(grp, name)

        # new sound files -> Xbox sounds
        dsh = next((f for f in map(fields, entries(dict(ms)['SoundDataBundles'])) if f[1] == 's'), None)
        slots = []
        if dsh:
            used.add(path_hash(f'data/audio/gc/{grp}/{name}/{dsh[2].lower()}'))
            d = mod.get(path_hash(f'data/audio/gc/{grp}/{name}/{dsh[2].lower()}'))
            slots = [s.decode('latin-1').lower() for s in re.findall(rb'[A-Za-z0-9_]+\.dsp', d)]
        file_map = {}
        vfiles, xfiles = entries(dict(vs)['SoundDataFiles']), entries(xg.sec('SoundDataFiles'))
        new_files = []
        for ln in appended(vs, ms, 'SoundDataFiles'):
            k, kind, ref = fields(ln)
            slot = int(re.match(r'(\d+)', ref).group(1))
            dsp = f'data/audio/gc/{grp}/{name}/{slots[slot]}'
            used.add(path_hash(dsp))
            data = mod.get(path_hash(dsp))
            srcs = van_md5.get(hashlib.md5(data).digest())
            if not srcs:
                raise SystemExit(f'{dsp}: new audio, not a copy of a vanilla clip - needs a converter')
            sm = re.match(r'data/audio/gc/([^/]+)/([^/]+)/(.+)\.dsp$', srcs[0])
            hdr, adpcm = group(sm.group(1), sm.group(2)).sound(sm.group(3))
            xslot = xg.add_sound(hdr, adpcm)
            k2 = int(k) - len(vfiles) + len(xfiles)
            file_map[int(k)] = k2
            new_files.append(f'{k2},\tc,\t{xslot}s.xbb')
            report.append(f'sound\t{dsp}\tXbox {sm.group(2)} {sm.group(3).upper()} -> {xg.xbb_name} slot {xslot}')
        if new_files:
            xg.append('SoundDataFiles', new_files)

        # new simple events
        simple_map, new_simple = {}, []
        vsim, xsim = entries(dict(vs)['SimpleEvents']), entries(xg.sec('SimpleEvents'))
        for ln in appended(vs, ms, 'SimpleEvents'):
            f = fields(ln)
            e2 = int(f[0]) - len(vsim) + len(xsim)
            if int(f[1]) not in file_map:
                raise SystemExit(f'{flo}: simple event {f[0]} plays an existing file - not supported')
            simple_map[int(f[0])] = e2
            new_simple.append(f'{e2},\t{file_map[int(f[1])]}, {", ".join(f[2:])}')
        if new_simple:
            xg.append('SimpleEvents', new_simple)

        # new event maps
        vmaps = [emap(ln) for ln in entries(dict(vs)['EventMaps/Pos'])]
        xmaps = {e[3]: e for e in (emap(ln) for ln in entries(xg.sec('EventMaps/Pos')))}
        by_link = collections.defaultdict(list)
        for e in vmaps:
            by_link[(e[1], e[2])].append(e[3])
        id_shift = max(e[0] for e in xmaps.values()) - max(e[0] for e in vmaps)
        new_maps = []
        for ln in appended(vs, ms, 'EventMaps/Pos'):
            i, t, link, nm = emap(ln)
            if nm in xmaps:
                raise SystemExit(f'{flo}: {nm} is already on Xbox')
            if t == 0 and link in simple_map:
                t2, l2, how = 0, simple_map[link], 'new event'
            else:
                hits = {(xmaps[s][1], xmaps[s][2]) for s in by_link[(t, link)] if s in xmaps}
                if len(hits) != 1:
                    raise SystemExit(f'{flo}: {nm}: no single Xbox event for GC {t},{link} ({hits})')
                (t2, l2), how = hits.pop(), f'as {by_link[(t, link)][0]}'
            new_maps.append(f'{i + id_shift},\t{t2}, {l2}, {nm}')
            report.append(f'event\t{nm}\tGC {t},{link} -> Xbox {t2},{l2} ({how})')
        if new_maps:
            xg.append('EventMaps/Pos', new_maps)

    for h in changed:
        if h not in used:
            report.append(f'not ported	{h:08X}	{names.get(h, "unnamed")} (not reached from a changed group flo)')
    written = []
    for xg in groups.values():
        rel = os.path.relpath(xg.dir, os.path.dirname(os.path.dirname(xroot.rstrip('/\\'))))
        for fn in sorted(xg.changed):
            data = {xg.flo_name: xg.flo_bytes, xg.xbb_name: lambda: bytes(xg.xbb),
                    xg.xsb_name: lambda: bytes(xg.xsb)}[fn]()
            p = os.path.join(out, rel, fn)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'wb') as f:
                f.write(data)
            written.append((os.path.join(xg.dir, fn), p))
    with open(os.path.join(out, 'gcaudio_report.txt'), 'w') as f:
        f.write('\n'.join(report) + '\n')
    for k, v in collections.Counter(r.split('\t')[0] for r in report).items():
        print(f'{v:6}  {k}')
    for s, p in written:
        print('  ' + p)
        if xdelta:
            subprocess.check_call([xdelta, '-e', '-9', '-S', 'none', '-f', '-s', s, p, p + '.xdelta'])


if __name__ == '__main__':
    main()

"""GLB characters -> retail class models + a boot school with one unit of each.

Settings: tools/glbchars.json (not included; one entry per model, see main()). Each entry is retargeted
(tools/retarget.py) onto its class's skeleton and animations and written loose over the class (data\\characters\\
<class>\\<class>.pax / .ptx / _var01.._var04.ptx). data\\school\\curschool.tok (the boot school) becomes Ursula + one
unit per converted character, copied from a unit of that class in its schooltest_*.tok and renamed.

    python tools/glbchars.py                     (all; --only <id> ... for some)
    python tools/glbchars.py --remove            (deletes the class overrides and the boot school)
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
LOOSE = os.path.join(ROOT, 'game', 'data')
BEC = os.path.join(ROOT, 'bec_unpacked', 'data')
SCHOOL = os.path.join(LOOSE, 'school', 'curschool.tok')


def char_dir(cls):
    return os.path.join(LOOSE, 'characters', cls.lower())


def convert(e, src, log):
    cls = e['class'].lower()
    os.makedirs(char_dir(cls), exist_ok=True)
    cmd = [sys.executable, os.path.join(TOOLS, 'retarget.py'), os.path.join(src, e['glb']),
           os.path.join(BEC, 'characters', cls, cls + '.pax'), os.path.join(char_dir(cls), cls + '.pax'),
           '--preset', 'auto'] + (['--map', os.path.join(TOOLS, e['map'])] if e.get('map') else []) + (
           ['--preset-json', os.path.join(TOOLS, e['preset_json'])] if e.get('preset_json') else []) + e.get('args', [])
    t0 = time.time()
    with open(log, 'w', encoding='utf-8') as f:
        f.write(' '.join(cmd) + '\n\n')
        f.flush()
        r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        tail = open(log, encoding='utf-8', errors='replace').read().strip().splitlines()[-1:]
        return False, f'FAILED (exit {r.returncode}): {tail[0] if tail else ""}  [{log}]'
    return True, f'ok in {time.time() - t0:.0f} s -> characters\\{cls}'


def unit_block(e):
    """the first unit of the class in its test school, renamed"""
    t = open(os.path.join(BEC, 'school', e['school'] + '.tok'), 'rb').read().decode('latin1')
    blocks = re.split(r'(?=^\s*CREATEUNIT:)', t, flags=re.M)
    want = e['school_class'].lower()
    pick = None
    for b in blocks[1:]:
        m = re.match(r'\s*CREATEUNIT:\s*"[^"]*"\s*,\s*"([^"]*)"', b)
        if m and m.group(1).lower() == want:
            pick = b
            break
    if pick is None:                                    # e.g. DervishF from the Dervish school: same skills
        for b in blocks[1:]:
            m = re.match(r'\s*CREATEUNIT:\s*"[^"]*"\s*,\s*"([^"]*)"', b)
            if m and want.startswith(m.group(1).lower()):
                pick = b
                break
    if pick is None:
        return None
    pick = re.sub(r'(CREATEUNIT:\s*)"[^"]*"\s*,\s*"[^"]*"',
                  lambda m: f'{m.group(1)}"{e["unit"]}", "{e["school_class"]}"', pick, count=1)
    # test schools equip developer items the release item list lacks (the school screen crashes on them): take the gear
    # of a unit of the same class whose items all exist
    gear = valid_gear(e['school_class'])
    pick = re.sub(r'ITEMSCOMP:.*', lambda m: 'ITEMSCOMP:\t\t\t' + gear, pick, count=1)
    return pick.rstrip() + '\r\n\r\n'


ITEMS = None


def valid_gear(cls):
    """an ITEMSCOMP value for the class with only existing items (from any retail school file), else nothing"""
    global ITEMS
    if ITEMS is None:
        t = open(os.path.join(BEC, 'config', 'items.tok'), 'rb').read().decode('latin1')
        ITEMS = {m.lower() for m in re.findall(r'ITEMCREATE:\s*"([^"]+)"', t)}
    best = None
    sdir = os.path.join(BEC, 'school')
    for f in sorted(os.listdir(sdir)):
        t = open(os.path.join(sdir, f), 'rb').read().decode('latin1')
        for b in re.split(r'(?=^\s*CREATEUNIT:)', t, flags=re.M)[1:]:
            m = re.match(r'\s*CREATEUNIT:\s*"[^"]*"\s*,\s*"([^"]*)"', b)
            g = re.search(r'ITEMSCOMP:\s*(.*)', b)
            if not m or not g or m.group(1).lower() != cls.lower():
                continue
            names = [x.strip().strip('"') for x in g.group(1).split('//')[0].split(',')]
            if all(not x or x.lower() in ITEMS for x in names) and any(names):
                score = sum(1 for x in names if x)              # the fullest valid loadout
                if best is None or score > best[0]:
                    best = (score, ', '.join(f'"{x}"' for x in names[:5]))
    return best[1] if best else '"", "", "", "", ""'


def write_school(done):
    base = open(os.path.join(BEC, 'school', 'curschool.tok'), 'rb').read().decode('latin1')
    # keep the header and Ursula (the hero); Urlan stays out to leave room for the test units
    blocks = re.split(r'(?=^\s*CREATEUNIT:)', base, flags=re.M)
    head = re.sub(r'(NAME:\s*)"[^"]*"', lambda m: m.group(1) + '"GLB Test School"', blocks[0])
    head = re.sub(r'(GOLD:\s*)\d+', lambda m: m.group(1) + '9999', head)
    out = ['// Boot school written by tools/glbchars.py: Ursula + the converted GLB characters\r\n', head]
    out += [b.rstrip() + '\r\n\r\n' for b in blocks[1:] if re.search(r'CREATEUNIT:\s*"Ursula"', b)]
    units = []
    for e in done:
        b = unit_block(e)
        if b:
            out.append(b)
            units.append(f'{e["unit"]} ({e["school_class"]})')
    os.makedirs(os.path.dirname(SCHOOL), exist_ok=True)
    open(SCHOOL, 'wb').write(''.join(out).encode('latin1'))
    return units


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', default=os.path.join(os.path.expanduser('~'), 'Desktop', 'GLBChars'))
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--remove', action='store_true')
    args = ap.parse_args()
    cfg = json.load(open(os.path.join(TOOLS, 'glbchars.json'), encoding='utf-8'))
    if args.remove:
        for e in cfg['characters']:
            shutil.rmtree(char_dir(e['class']), ignore_errors=True)
        if os.path.exists(SCHOOL):
            os.remove(SCHOOL)
        print('removed the class overrides and the boot school')
        return
    logs = os.path.join(ROOT, 'build', 'glbchars')
    os.makedirs(logs, exist_ok=True)
    for e in cfg['characters']:
        if args.only and e['id'] not in args.only:
            continue
        if not os.path.exists(os.path.join(args.src, e['glb'])):
            print(f'  {e["id"]:10} missing {e["glb"]} - skipped')
            continue
        print(f'  {e["id"]:10} {e["glb"]} -> {e["class"]} ...', flush=True)
        ok, msg = convert(e, args.src, os.path.join(logs, e['id'] + '.log'))
        print(f'  {"":10} {msg}', flush=True)
    done = [e for e in cfg['characters']
            if os.path.exists(os.path.join(char_dir(e['class']), e['class'].lower() + '.pax'))]
    units = write_school(done)
    print(f'boot school {SCHOOL}: Ursula + {", ".join(units)}')


if __name__ == '__main__':
    main()

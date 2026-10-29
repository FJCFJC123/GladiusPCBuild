"""Batch decompiler: python -m dog <input file|dir> [-o out_dir] [--map]

.scb -> .scp, .boo -> .moo. Demo files that still carry their debug source (INFO block)
are decompiled from the bytecode too; the embedded source truncates long lines.
"""
import argparse
import sys
from pathlib import Path

from . import scb
from .decompile import decompile_file


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("-o", "--out", help="output folder (default: next to the inputs)")
    ap.add_argument("--map", action="store_true", help="also write <name>.map: source line -> code offsets "
                    "(GladiusPort script debugger)")
    a = ap.parse_args()
    src = Path(a.input)
    files = [src] if src.is_file() else sorted(p for p in src.rglob("*") if p.suffix.lower() in (".scb", ".boo"))
    ok = warn = fail = 0
    for f in files:
        out_dir = Path(a.out) if a.out else f.parent
        try:
            dst, warnings = decompile_file(f, out_dir, a.map)
            ok += 1
            for w in warnings:
                warn += 1
                print(f"  {f.name}: {w}")
        except Exception as e:
            fail += 1
            print(f"FAILED {f.name}: {e}", file=sys.stderr)
    print(f"{ok} decompiled, {warn} warnings, {fail} failed")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())

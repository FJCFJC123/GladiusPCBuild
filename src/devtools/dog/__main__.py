"""Dog scripts: decompile bytecode, or compile source.

  python -m dog <input file|dir> [-o out_dir] [--map]      .scb -> .scp, .boo -> .moo (decompile)
  python -m dog compile <file.scp|.moo> [-o out_file]      .scp -> .scb, .moo -> .boo (compile, release format)

Demo files that still carry their debug source (INFO block) are decompiled from the bytecode too; the embedded
source truncates long lines. Compile errors print as '<file>(<line>) Error: "<message>"' (exit code 1).
"""
import argparse
import sys
from pathlib import Path

from . import scb
from .decompile import decompile_file


def compile_main(argv):
    from .compile import CompileError, compile_source
    ap = argparse.ArgumentParser(prog="python -m dog compile")
    ap.add_argument("input")
    ap.add_argument("-o", "--out", help="output file (default: next to the source, .scb / .boo)")
    a = ap.parse_args(argv)
    src = Path(a.input)
    dst = Path(a.out) if a.out else src.with_suffix(".boo" if src.suffix.lower() == ".moo" else ".scb")
    try:
        data = compile_source(src.read_text(encoding="cp1252", errors="replace"), src.stem)
    except CompileError as e:
        print(f'{src.name}({e.line}) Error: "{e.msg}"')
        return 1
    except RecursionError:
        print(f'{src.name}(0) Error: "expression nested too deeply"')
        return 1
    dst.write_bytes(data)
    print(f"{src.name} -> {dst.name} ({len(data)} bytes)")
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "compile":
        return compile_main(sys.argv[2:])
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

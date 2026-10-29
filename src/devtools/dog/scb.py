"""Dog compiled-script container (SGOD) parser and disassembler.

Layout (little endian): 'SGOD' u16 version (0x85 demo, 0x86 release) u16 1, then a
TOC of (tag[::-1], offset) pairs; offset 0xFFFFFFFF = absent.
  NSTR  normal string table: size, count, NUL-terminated strings (string literals)
  SSTR  symbol string table: size, count, strings (identifier names)
  SYMB  symbols: size, then (name_off, type, value) u32 triples
  SASY  shared symbols (None/Null/True/False...): size, triples; referenced with bit 0x20000000
  CODE  size, bytecode
  EXRT  exports ('XRTP')
  FILE  referenced files (usually absent)
  INFO  'OFNI' debug source: count, (len, bytes, NUL) records; LINE n -> record n. Stripped in release.

Opcode table = protius.exe Dog op table @0x948DA0 (flags, name, handler): flags low byte
0x01/0x05/0x0D = 0/1/3 u32 operands. Each instruction = u8 opcode + operands.
"""
import re
import struct

_COLOR = re.compile(rb"\x1bc[0-9A-Fa-f]{6}")

OPS = [
    "INVALID", "NOP", "ARG", "STATEMENT", "JUMPTARGET",
    "WHILE", "GOENDWHILE", "ENDWHILE", "GOTO", "CALL",
    "STOP", "IF", "ELSEIF", "ELSE", "ENDIF",
    "RETURN", "RETURNVAL", "DISCARD", "FOR", "SKIP",
    "NEXT_SYM", "PLUS_PLUS", "MINUS_MINUS", "PLUS_EQUALS", "MINUS_EQUALS",
    "MULT_EQUALS", "DIV_EQUALS", "OR_EQUALS", "AND_EQUALS", "XOR_EQUALS",
    "MOD_EQUALS", "SWITCH", "ENDSWITCH", "CASE", "BREAK",
    "CONTINUE", "DEFAULT", "INT", "FLOAT", "STRING",
    "VECTOR", "ARTICLE", "PUSHINT", "PUSHFLOAT", "PUSHSTRING",
    "PUSHVECTOR", "PUSHSYMBOL", "POPINT", "POPFLOAT", "POPSTRING",
    "POPVECTOR", "POPSYMBOL", "ARRAYREF", "CALLFUNC", "UNEG",
    "LNEG", "MULTIPLY", "DIVIDE", "MOD", "ADD",
    "SUB", "LT", "GT", "LE", "GE",
    "EQ", "NE", "AND", "XOR", "OR",
    "LAND", "LOR", "ASSIGN", "GOTRUE", "FUNCLABEL",
    "GOTOLABEL", "BRA_ENDIF", "LINE", "ALLOC_INT_ARRAY", "ALLOC_FLOAT_ARRAY",
    "ALLOC_ARRAY", "ALLOC_ARTICLE", "PRINT_ID", "PRINT_STRING",
]
OP = {n: i for i, n in enumerate(OPS)}

# Flag byte of each op-table entry, in opcode order, read from protius.exe @0x948DA0
# (stride 12): 0x01 -> 0 operands, 0x05 -> 1, 0x0D -> 3.
_FLAGS = bytes([
    0x01, 0x01, 0x01, 0x01, 0x01,   # INVALID NOP ARG STATEMENT JUMPTARGET
    0x01, 0x05, 0x05, 0x05, 0x0D,   # WHILE GOENDWHILE ENDWHILE GOTO CALL
    0x01, 0x05, 0x05, 0x01, 0x01,   # STOP IF ELSEIF ELSE ENDIF
    0x01, 0x01, 0x01, 0x01, 0x05,   # RETURN RETURNVAL DISCARD FOR SKIP
    0x05, 0x01, 0x01, 0x01, 0x01,   # NEXT_SYM PLUS_PLUS MINUS_MINUS PLUS_EQUALS MINUS_EQUALS
    0x01, 0x01, 0x01, 0x01, 0x01,   # MULT_ DIV_ OR_ AND_ XOR_EQUALS
    0x01, 0x01, 0x05, 0x05, 0x05,   # MOD_EQUALS SWITCH ENDSWITCH CASE BREAK
    0x05, 0x01, 0x05, 0x05, 0x05,   # CONTINUE DEFAULT INT FLOAT STRING
    0x05, 0x05, 0x05, 0x05, 0x05,   # VECTOR ARTICLE PUSHINT PUSHFLOAT PUSHSTRING
    0x0D, 0x05, 0x05, 0x05, 0x05,   # PUSHVECTOR PUSHSYMBOL POPINT POPFLOAT POPSTRING
    0x0D, 0x05, 0x01, 0x0D, 0x01,   # POPVECTOR POPSYMBOL ARRAYREF CALLFUNC UNEG
    0x01, 0x01, 0x01, 0x01, 0x01,   # LNEG MULTIPLY DIVIDE MOD ADD
    0x01, 0x01, 0x01, 0x01, 0x01,   # SUB LT GT LE GE
    0x01, 0x01, 0x01, 0x01, 0x01,   # EQ NE AND XOR OR
    0x01, 0x01, 0x01, 0x01, 0x05,   # LAND LOR ASSIGN GOTRUE FUNCLABEL
    0x05, 0x05, 0x05, 0x01, 0x01,   # GOTOLABEL BRA_ENDIF LINE ALLOC_INT_ARRAY ALLOC_FLOAT_ARRAY
    0x01, 0x01, 0x05, 0x05,         # ALLOC_ARRAY ALLOC_ARTICLE PRINT_ID PRINT_STRING
])
NOPERANDS = [{0x01: 0, 0x05: 1, 0x0D: 3}[f] for f in _FLAGS]
assert len(NOPERANDS) == len(OPS)

SHARED_BIT = 0x20000000
GLOBAL_OBJ = 0x40000000


class Instr:
    __slots__ = ("off", "op", "args", "size")

    def __init__(self, off, op, args, size):
        self.off, self.op, self.args, self.size = off, op, args, size

    @property
    def name(self):
        return OPS[self.op] if self.op < len(OPS) else f"OP_{self.op:02X}"

    def __repr__(self):
        return f"{self.off:04x} {self.name} {self.args}"


def _strings(data, off):
    size, count = struct.unpack_from("<II", data, off + 4)
    blob = data[off + 12: off + 12 + size]
    out, p = {}, 0
    for _ in range(count):
        e = blob.index(b"\0", p)
        out[p] = blob[p:e].decode("cp1252", errors="replace")
        p = e + 1
    return out


def _triples(data, off):
    size = struct.unpack_from("<I", data, off + 4)[0]
    return [struct.unpack_from("<III", data, off + 8 + i * 12) for i in range(size // 12)]


class ScriptFile:
    def __init__(self, data, name=""):
        if data[:4] != b"SGOD":
            raise ValueError(f"{name}: not a Dog SGOD file")
        self.name = name
        self.version = struct.unpack_from("<H", data, 4)[0]
        self.toc, p = {}, 8
        while p + 8 <= len(data):
            tag = data[p:p + 4]
            if tag == b"\0\0\0\0" or not tag.isalpha():
                break
            self.toc[tag[::-1].decode()] = struct.unpack_from("<I", data, p + 4)[0]
            p += 8
        has = lambda t: self.toc.get(t, 0xFFFFFFFF) != 0xFFFFFFFF
        self.nstr = _strings(data, self.toc["NSTR"]) if has("NSTR") else {}
        self.sstr = _strings(data, self.toc["SSTR"]) if has("SSTR") else {}
        self.symbols = _triples(data, self.toc["SYMB"]) if has("SYMB") else []
        self.shared = _triples(data, self.toc["SASY"]) if has("SASY") else []
        co = self.toc["CODE"]
        size = struct.unpack_from("<I", data, co + 4)[0]
        self.code = data[co + 8: co + 8 + size]
        self.source_lines = self._info(data) if has("INFO") else None

    def _info(self, data):
        off = self.toc["INFO"]
        count = struct.unpack_from("<I", data, off + 4)[0]
        p, lines = off + 8, []
        for _ in range(count):
            n = struct.unpack_from("<I", data, p)[0]
            raw = _COLOR.sub(b"", data[p + 4:p + 4 + n])      # strip ESC c RRGGBB syntax colours
            lines.append(raw.decode("cp1252", errors="replace"))
            p += 4 + n + 1
        return lines

    # --- names -----------------------------------------------------------
    def sym(self, idx):
        """Symbol name for an operand (bit 0x20000000 = shared table)."""
        if idx & SHARED_BIT:
            tab, i = self.shared, idx & ~SHARED_BIT
        else:
            tab, i = self.symbols, idx
        if 0 <= i < len(tab):
            return self.sstr.get(tab[i][0], f"sym{idx}")
        return f"sym{idx:#x}"

    def sym_entry(self, idx):
        tab, i = (self.shared, idx & ~SHARED_BIT) if idx & SHARED_BIT else (self.symbols, idx)
        return tab[i] if 0 <= i < len(tab) else None

    def string(self, off):
        return self.nstr.get(off, f"<str{off}>")

    # --- code ------------------------------------------------------------
    def instructions(self, keep_lines=True):
        out, p, code = [], 0, self.code
        while p < len(code):
            op = code[p]
            n = NOPERANDS[op] if op < len(OPS) else 0
            args = list(struct.unpack_from(f"<{n}I", code, p + 1)) if n and p + 1 + 4 * n <= len(code) else []
            ins = Instr(p, op, args, 1 + 4 * n)
            if keep_lines or ins.name != "LINE":
                out.append(ins)
            p += ins.size
        return out

    def describe(self, ins):
        n, a = ins.name, ins.args
        if n in ("PUSHSYMBOL", "POPSYMBOL", "FUNCLABEL", "INT", "FLOAT", "STRING", "VECTOR",
                 "ARTICLE", "POPINT", "POPFLOAT", "POPSTRING", "GOTOLABEL", "PRINT_ID"):
            return f"{n} {self.sym(a[0])}"
        if n in ("PUSHSTRING", "PRINT_STRING"):
            return f'{n} "{self.string(a[0])}"'
        if n == "PUSHFLOAT":
            return f"{n} {struct.unpack('<f', struct.pack('<I', a[0]))[0]:g}"
        if n == "PUSHVECTOR":
            return f"{n} " + ",".join(f"{struct.unpack('<f', struct.pack('<I', x))[0]:g}" for x in a)
        if n == "CALLFUNC":
            obj = "global" if a[2] == GLOBAL_OBJ else self.sym(a[2])
            return f"{n} {obj}.{self.sym(a[0])} argc={a[1]}"
        if n == "CALL":
            return f"{n} raw={[hex(x) for x in a]} sym1={self.sym(a[1])} str1={self.nstr.get(a[1])}"
        if n == "GOTO":
            return f"{n} {self.sym(a[0])}"
        return f"{n} " + " ".join(f"{x:#x}" for x in a)


def load(path):
    from pathlib import Path
    p = Path(path)
    return ScriptFile(p.read_bytes(), p.name)

"""Dog bytecode -> Dog source (.scb -> .scp, .boo -> .moo).

The VM keeps structure in the bytecode, so control flow is rebuilt from block markers
(offsets are only cross-checks; demo files count LINE ops in them, release files don't):

  if     : cond IF ; then ; [BRA_ENDIF ; {cond ELSEIF ; body ; BRA_ENDIF} ; [else ; ELSE]] ; ENDIF
  while  : WHILE ; cond ; GOENDWHILE ; body ; ENDWHILE
  for    : [INT v] ; v = A ; DISCARD ; FOR ; body ; v </> B ; SKIP ; v +=/-= S ; DISCARD ; NEXT_SYM
  func   : FUNCLABEL f ; POPSYMBOL param (last first) ... ; body ; None RETURNVAL (= endfunction)
  stmt   : expr ; DISCARD      decl: INT v / a n ALLOC_INT_ARRAY
Expressions are rebuilt on a value stack; locals are named 'name#Function' (release:
'name#<index>'), shared globals have no '#', and get 'shared' on their first definition.
"""
import math
import struct
from decimal import Decimal

from . import scb

BINOPS = {  # op -> (text, precedence)
    "LOR": ("||", 1), "LAND": ("&&", 2), "OR": ("|", 3), "XOR": ("^", 4), "AND": ("&", 5),
    "EQ": ("==", 6), "NE": ("!=", 6), "LT": ("<", 7), "GT": (">", 7), "LE": ("<=", 7), "GE": (">=", 7),
    "ADD": ("+", 8), "SUB": ("-", 8), "MULTIPLY": ("*", 9), "DIVIDE": ("/", 9), "MOD": ("%", 9),
}
COMPOUND = {"PLUS_EQUALS": "+=", "MINUS_EQUALS": "-=", "MULT_EQUALS": "*=", "DIV_EQUALS": "/=",
            "OR_EQUALS": "|=", "AND_EQUALS": "&=", "XOR_EQUALS": "^=", "MOD_EQUALS": "%="}
DECLS = {"INT": "int", "FLOAT": "float", "STRING": "string", "VECTOR": "vector", "ARTICLE": "article"}
ALLOCS = {"ALLOC_INT_ARRAY": "int", "ALLOC_FLOAT_ARRAY": "float", "ALLOC_ARRAY": "array",
          "ALLOC_ARTICLE": "article"}
ATOM = 99
BLOCK_END = ("ENDIF", "ELSE", "ELSEIF", "BRA_ENDIF", "ENDWHILE", "SKIP", "EOF")


class DecompileError(Exception):
    pass


GROUP = {"LOR": "lor", "LAND": "land", "OR": "bit", "XOR": "bit", "AND": "bit",
         "EQ": "cmp", "NE": "cmp", "LT": "cmp", "GT": "cmp", "LE": "cmp", "GE": "cmp",
         "ADD": "add", "SUB": "add", "MULTIPLY": "mul", "DIVIDE": "mul", "MOD": "mul"}
# (parent group, child group) pairs whose binding is proven by the demo corpus; every
# other mix gets explicit parentheses, so the output parses the same whatever Dog's
# precedence table is (bitwise vs comparison, && vs || ...).
BARE_OK = {("lor", "cmp"), ("land", "cmp"), ("cmp", "add"), ("cmp", "mul"), ("add", "mul"),
           ("lor", "unary"), ("land", "unary"), ("cmp", "unary"), ("add", "unary"),
           ("mul", "unary"), ("bit", "unary")}


class E:
    """Expression: source text + operator group (for safe parenthesisation)."""
    __slots__ = ("text", "prec", "kind", "var", "group")

    def __init__(self, text, prec=ATOM, kind="expr", var=None, group=None):
        self.text, self.prec, self.kind, self.var, self.group = text, prec, kind, var, group

    def operand(self, parent_group, right=False):
        """Text for use as an operand of a binary operator of `parent_group`."""
        if self.group is None:
            return self.text
        if self.group == parent_group and parent_group not in ("cmp", "bit"):
            # same family, left-associative: only the right side needs parentheses
            return f"({self.text})" if right else self.text
        if (parent_group, self.group) in BARE_OK:
            return self.text
        return f"({self.text})"

    def wrap(self, prec):
        return f"({self.text})" if self.prec < prec else self.text


def _float(bits):
    """The shortest decimal that reads back as the same float32, positional, at least one decimal
    ("1.0", "0.25", "-3.5"; same text as numpy's format_float_positional(unique, trim="0"), checked
    on 1.4M values - plain Python, so the decompiler runs on a stock Python install)."""
    f = struct.unpack("<f", struct.pack("<I", bits))[0]
    if math.isnan(f):
        return "nan"
    if math.isinf(f):
        return "-inf" if f < 0 else "inf"
    want = struct.pack("<f", f)
    for p in range(1, 10):
        s = "%.*g" % (p, f)
        try:
            if struct.pack("<f", float(s)) == want:
                break
        except OverflowError:                           # rounded past the largest float32
            pass
    t = format(Decimal(s), "f")
    if "." not in t:
        t += ".0"
    t = t.rstrip("0")
    if t.endswith("."):
        t += "0"
    if f == 0 and math.copysign(1, f) < 0 and not t.startswith("-"):
        t = "-" + t
    return t


def _quote(s):
    # the compiler unescapes \\ \" \n; tabs appear literally in the original sources
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


class L(str):
    """A source line plus the code offsets of the instructions it was built from (the script
    debugger's line map). Behaves as a plain str everywhere else."""
    __slots__ = ("offs",)

    def __new__(cls, text, offs=()):
        line = super().__new__(cls, text)
        line.offs = list(offs)
        return line


def ind(lines):
    """Indent a block one level, keeping each line's offsets."""
    return [L("\t" + l, getattr(l, "offs", ())) for l in lines]


class Decompiler:
    def __init__(self, sf):
        self.sf = sf
        self.mark = 0            # self.ins index up to which instructions belong to a tagged line
        full = sf.instructions(keep_lines=True)
        self.func_at = {i.off: sf.sym(i.args[0]) for i in full if i.name == "FUNCLABEL"}
        self.func_names = {n.lower() for n in self.func_at.values()}
        self.ins = [i for i in full if i.name not in ("LINE", "NOP")]
        self.pos = 0
        self.shared_done = set()
        self._last_decl = None
        self.referenced = []     # every variable use, in file order
        self.nested_call_quirk = False
        self.nested_fn = False   # inside a function header met within a block
        self.warnings = []
        self.notes = []

    # ------------------------------------------------------------ names
    def name(self, idx):
        n = self.sf.sym(idx)
        return n.split("#", 1)[0]

    BUILTINS = {"self", "none", "null", "true", "false"}

    def scope(self, idx):
        """'shared' (SASY table, bit 0x20000000), 'global' (file-level: SYMB entry without
        '#Function'), or None for locals / built-ins."""
        name = self.sf.sym(idx)
        if name.lower() in self.BUILTINS or name.lower() in self.func_names:
            return None               # built-ins and function names (e.g. fork targets)
        if idx & scb.SHARED_BIT:
            return "shared"
        return "global" if "#" not in name else None

    def qualify(self, var, text):
        """Prefix 'shared'/'global' on a variable's first definition. The compiler resolves
        scope by name for the whole file (a read before the 'shared' line still compiles
        as shared), so one keyword anywhere is enough."""
        if var is None or var in self.shared_done:
            return text
        sc = self.scope(var)
        if not sc:
            return text
        self.shared_done.add(var)
        return f"{sc} {text}"

    def tag(self, text, extra=()):
        """A line made of every instruction consumed since the previous tagged line."""
        offs = [self.ins[k].off for k in range(self.mark, self.pos)] + list(extra)
        self.mark = max(self.mark, self.pos)
        return L(text, offs)

    def see(self, var):
        self.referenced.append(var)

    def lhs(self, e, text):
        return self.qualify(e.var, text)

    # ------------------------------------------------------------ driver
    def run(self):
        out = []
        top, term, stack = self.block(("FUNCLABEL",))
        if stack:
            raise DecompileError(f"{len(stack)} values left at top level")
        out += top
        while self.pos < len(self.ins):
            fn = self.ins[self.pos]
            self.pos += 1
            params = []
            while self.pos < len(self.ins) and self.ins[self.pos].name == "POPSYMBOL":
                params.insert(0, self.name(self.ins[self.pos].args[0]))
                self.pos += 1
            head = self.tag(f"function {self.name(fn.args[0])}( {', '.join(params)} )" if params
                            else f"function {self.name(fn.args[0])}( )")
            body = self.function_body(fn)
            if out:
                out.append("")
            out.append(head)
            out += ind(body)
            out.append(self.tag("endfunction"))

        # globals that are used but never defined in this file still need their scope
        # keyword once: declare them at the top of the first function
        decl, seen = [], set()
        for v in self.referenced:
            if v in seen or not self.scope(v) or v in self.shared_done:
                continue
            seen.add(v)
            self.shared_done.add(v)
            decl.append(f"\t{self.scope(v)} {self.name(v)}")
        if decl:
            first = next((k for k, l in enumerate(out) if l.startswith("function ")), None)
            if first is None:
                out = [d.strip() for d in decl] + out
            else:
                out[first + 1:first + 1] = decl
        return out

    def function_body(self, fn):
        """Decompile one function body. If the stack doesn't balance, retry with the
        nested-call argc reading; failing that, emit the disassembly as comments."""
        start, saved = self.pos, (set(self.shared_done), list(self.referenced))
        for quirk in (False, True):
            self.pos, self.mark, self.nested_call_quirk = start, start, quirk
            self.shared_done, self.referenced = set(saved[0]), list(saved[1])
            try:
                body, term, stack = self.block(("FUNCLABEL",), function_end=True)
                if stack:
                    raise DecompileError(f"{len(stack)} values left on stack")
                if quirk:   # rule confirmed against real source (battle_belfort_beast_0x_taketwo)
                    self.notes.append(f"{self.name(fn.args[0])}: nested 'call' argc rule applied")
                return body
            except (DecompileError, IndexError) as e:
                err = e
        self.nested_call_quirk = False
        self.pos = start
        end = next((k for k in range(start, len(self.ins)) if self.ins[k].name == "FUNCLABEL"), len(self.ins))
        self.pos = self.mark = end
        self.warnings.append(f"{self.name(fn.args[0])}: not decompiled ({err})")
        return [L(f"// DECOMPILE FAILED ({err}) - disassembly:")] + \
               [L(f"//   {self.sf.describe(i)}", [i.off]) for i in self.ins[start:end]]

    # ------------------------------------------------------------ blocks
    def block(self, stops, function_end=False):
        """Parse statements until an instruction named in `stops` (not consumed) or EOF.
        Returns (lines, stop_name, leftover_stack)."""
        lines, stack = [], []
        while self.pos < len(self.ins):
            i = self.ins[self.pos]
            n = i.name
            if n in stops:
                return lines, n, stack
            self.pos += 1
            a = i.args
            if n == "PUSHINT":
                # bit-flag constants like 0x80000000 are written in hex in the sources
                stack.append(E(f"0x{a[0]:08X}" if a[0] & 0x80000000 else str(a[0])))
            elif n == "PUSHFLOAT":
                stack.append(E(_float(a[0])))
            elif n == "PUSHSTRING":
                stack.append(E(_quote(self.sf.string(a[0]))))
            elif n == "PUSHVECTOR":
                stack.append(E("<" + ", ".join(_float(x) for x in a) + ">"))
            elif n == "PUSHSYMBOL":
                self.see(a[0])
                stack.append(E(self.name(a[0]), var=a[0]))
            elif n in BINOPS:
                b, x = stack.pop(), stack.pop()
                op, p = BINOPS[n]
                g = GROUP[n]
                stack.append(E(f"{x.operand(g)} {op} {b.operand(g, right=True)}", p, group=g))
            elif n in ("UNEG", "LNEG"):
                x = stack.pop()
                inner = x.text if x.group in (None, "unary") else f"({x.text})"
                stack.append(E(("-" if n == "UNEG" else "!") + inner, 10, group="unary"))
            elif n == "ARRAYREF":
                idx, base = stack.pop(), stack.pop()
                stack.append(E(f"{base.text}[{idx.text}]", var=base.var))
            elif n == "CALLFUNC":
                args = [stack.pop() for _ in range(a[1])][::-1]
                fname = self.name(a[0])
                if (fname == "_GetArticle" and a[2] != scb.GLOBAL_OBJ and len(args) == 1
                        and args[0].text.startswith('"')):
                    # member access obj.name compiles to obj._GetArticle("name")
                    self.see(a[2])
                    stack.append(E(f"{self.name(a[2])}.{args[0].text[1:-1]}", var=a[2]))
                    continue
                call = f"{fname}({', '.join(x.text for x in args)})"
                if a[2] != scb.GLOBAL_OBJ:
                    self.see(a[2])
                    call = f"{self.name(a[2])}.{call}"
                stack.append(E(call))
            elif n == "CALL":
                args, want = [], a[2]
                while want > 0:
                    x = stack.pop()
                    args.insert(0, x)
                    # the compiler counts a nested 'call' inside the argument list once more
                    # in argc (confirmed against real source); tried only if the plain
                    # reading leaves the stack unbalanced
                    want -= 2 if (self.nested_call_quirk and x.kind == "call") else 1
                target = self.func_at.get(a[1], f"func_{a[1]:x}") if a[0] == 1 else self.sf.sym(a[1])
                stack.append(E(f"call {target}({', '.join(x.text for x in args)})", kind="call"))
            elif n == "ASSIGN":
                v, t = stack.pop(), stack.pop()
                stack.append(E(self.lhs(t, f"{t.text} = {v.text}"), 0, "assign", t.var))
            elif n in COMPOUND:
                v, t = stack.pop(), stack.pop()
                stack.append(E(f"{t.text} {COMPOUND[n]} {v.text}", 0, "compound", t.var))
            elif n in ("PLUS_PLUS", "MINUS_MINUS"):
                t = stack.pop()
                stack.append(E(t.text + ("++" if n == "PLUS_PLUS" else "--"), 0, "compound", t.var))
            elif n == "DISCARD":
                if len(stack) == 1 and stack[0].text == "0xDEADC0DE":
                    # the release compiler replaces print/printf/debug statements with a
                    # no-op 'push 0xDEADC0DE; discard'. 'print()' compiles back to exactly
                    # that (dog.compile strips it the same way); its text is lost.
                    lines.append(self.tag("print()    // debug output (its text was stripped by the release compiler)"))
                    stack.clear()
                    self._last_decl = None
                elif len(stack) > 1:
                    # comma expression statement, e.g. 'OnBattleEndLose, "56sf", "56sf"'
                    lines.append(self.tag(", ".join(x.text for x in stack)))
                    stack.clear()
                    self._last_decl = None
                elif stack:
                    e = stack.pop()
                    # 'int x' then 'x = v'  ->  'int x = v'
                    if (e.kind == "assign" and lines and self._last_decl
                            and self._last_decl[0] == e.var and lines[-1] == self._last_decl[1]):
                        lines[-1] = self.tag(self._last_decl[1] + " = " + e.text.split(" = ", 1)[1], lines[-1].offs)
                    elif e.kind == "expr" and e.group is None and e.var is None and "(" not in e.text:
                        # a lone literal statement is a no-op (e.g. the stray '.0' from the
                        # source typo 'x = 0.0.0'); kept as code - dog.compile accepts it
                        lines.append(self.tag(f"{e.text}    // no-op statement (a source typo)"))
                    else:
                        lines.append(self.tag(e.text))
                    self._last_decl = None
            elif n in DECLS:
                decl = self.qualify(a[0], f"{DECLS[n]} {self.name(a[0])}")
                lines.append(self.tag(decl))
                self._last_decl = (a[0], decl)
                continue
            elif n in ALLOCS:
                size, base = stack.pop(), stack.pop()
                lines.append(self.tag(self.qualify(base.var, f"{ALLOCS[n]} {base.text}[{size.text}]")))
            elif n == "RETURNVAL":
                v = stack.pop()
                nxt = self.ins[self.pos].name if self.pos < len(self.ins) else "EOF"
                if function_end and v.text == "None" and nxt in ("FUNCLABEL", "EOF"):
                    continue  # implicit end of function
                if v.text == "None" and (nxt == "FUNCLABEL" or (self.nested_fn and nxt in BLOCK_END)):
                    lines.append("endfunction")   # a function written inside a block (compiled inline)
                    self.nested_fn = False
                    continue
                # the PC compiler requires parentheses: 'return()' not 'return'
                lines.append(self.tag("return()" if v.text == "None" else f"return ({v.text})"))
            elif n == "RETURN":
                lines.append(self.tag("return()"))
            elif n == "IF":
                lines += self.if_block(stack.pop())
            elif n == "WHILE":
                lines += self.while_block()
            elif n == "FOR":
                lines += self.for_block(lines)
            elif n == "GOTO":
                lines.append(self.tag(f"goto {self.name(a[0])}"))
            elif n == "GOTOLABEL":
                lines.append(self.tag(f"{self.name(a[0])}:"))
            elif n == "STOP":
                lines.append(self.tag("stop"))
            elif n == "PRINT_STRING":
                lines.append(self.tag(f"print {_quote(self.sf.string(a[0]))}"))
            elif n == "PRINT_ID":
                lines.append(self.tag(f"print {self.name(a[0])}"))
            elif n == "FUNCLABEL":
                # a function header inside an open block: the original source forgot an
                # endif/endfunction and the compiler accepted it - reproduce it verbatim
                params = []
                while self.pos < len(self.ins) and self.ins[self.pos].name == "POPSYMBOL":
                    params.insert(0, self.name(self.ins[self.pos].args[0]))
                    self.pos += 1
                lines.append(self.tag(f"function {self.name(a[0])}( {', '.join(params)} )" if params
                                      else f"function {self.name(a[0])}( )"))
                self.nested_fn = True
            else:
                raise DecompileError(f"unhandled {n} at {i.off:#x}")
        return lines, "EOF", stack

    def _expect(self, name):
        if self.pos >= len(self.ins) or self.ins[self.pos].name != name:
            got = self.ins[self.pos].name if self.pos < len(self.ins) else "EOF"
            raise DecompileError(f"expected {name}, got {got}")
        self.pos += 1

    def if_block(self, cond):
        out = [self.tag(f"if ( {cond.text} )")]
        body, term, st = self.block(("BRA_ENDIF", "ENDIF", "ELSE", "ELSEIF"))
        out += ind(body)
        if term == "ENDIF":
            self.pos += 1
            return out + [self.tag("endif")]
        if term != "BRA_ENDIF":
            raise DecompileError(f"if: unexpected {term}")
        self.pos += 1
        while True:
            body, term, st = self.block(("BRA_ENDIF", "ENDIF", "ELSE", "ELSEIF"))
            if term == "ELSEIF":
                if len(st) != 1 or body:
                    raise DecompileError("elseif: condition not isolated")
                self.pos += 1
                out.append(self.tag(f"elseif ( {st[0].text} )"))
                body, term, st = self.block(("BRA_ENDIF", "ENDIF", "ELSE", "ELSEIF"))
                out += ind(body)
                if term == "BRA_ENDIF":
                    self.pos += 1
                    continue
                if term == "ENDIF":
                    self.pos += 1
                    return out + [self.tag("endif")]
                raise DecompileError(f"elseif: unexpected {term}")
            if term == "ELSE":
                self.pos += 1
                out.append(self.tag("else"))
                out += ind(body)
                self._expect("ENDIF")
                return out + [self.tag("endif")]
            if term == "ENDIF":
                if body:
                    out.append(L("else"))
                    out += ind(body)
                self.pos += 1
                return out + [self.tag("endif")]
            raise DecompileError(f"if chain: unexpected {term}")

    def while_block(self):
        pre, term, st = self.block(("GOENDWHILE",))
        if term != "GOENDWHILE" or pre or len(st) != 1:
            raise DecompileError("while: malformed condition")
        self.pos += 1
        head = self.tag(f"while ( {st[0].text} )")
        body, term, _ = self.block(("ENDWHILE",))
        self._expect("ENDWHILE")
        return [head] + ind(body) + [self.tag("endwhile")]

    def for_block(self, lines):
        # init statement 'v = A' (and an implicit 'int v' declaration) precede FOR
        if not lines or " = " not in lines[-1]:
            raise DecompileError("for: no init")
        init = lines.pop()
        head_offs = list(getattr(init, "offs", ())) + self.tag("").offs   # init, FOR
        var, start = init.split(" = ", 1)
        words = var.split()
        qual = words[0] + " " if words[0] in ("shared", "global") else ""
        var = words[-1]                       # drop implicit 'int' / scope keyword
        if lines and lines[-1].split()[-1:] == [var] and "int" in lines[-1].split():
            head_offs = list(getattr(lines.pop(), "offs", ())) + head_offs
        var_decl = qual + var
        body, term, st = self.block(("SKIP",))
        if term != "SKIP" or len(st) != 1:
            raise DecompileError("for: malformed limit test")
        self.pos += 1
        head_offs += self.tag("").offs                  # limit test, SKIP
        cond = st[0].text
        op = " < " if " < " in cond else " > "
        limit = cond.split(op, 1)[1] if op in cond else cond
        inc, term, st2 = self.block(("NEXT_SYM",))
        for l in inc:
            head_offs += getattr(l, "offs", ())         # the step
        self._expect("NEXT_SYM")
        step = "1"
        if inc and (" += " in inc[-1] or " -= " in inc[-1]):
            sep = " += " if " += " in inc[-1] else " -= "
            step = inc[-1].split(sep, 1)[1]
            if sep == " -= ":
                step = "-" + step
        head = f"for {var_decl} = {start} to {limit} step {step}"
        return [L(head, head_offs)] + ind(body) + [self.tag(f"next {var}")]


def decompile(sf):
    return Decompiler(sf).run()


def line_map(sf, lines):
    """The script debugger's line map: 'dogmap 1 <code bytes> <crc32 of code>', then one row
    per source line that has code: '<line number> <code offset> ...' (hex offsets)."""
    import zlib
    rows = [f"dogmap 1 {len(sf.code)} {zlib.crc32(sf.code) & 0xFFFFFFFF:08x}"]
    for n, l in enumerate(lines, 1):
        offs = sorted(set(getattr(l, "offs", ())))
        if offs:
            rows.append(f"{n} " + " ".join(f"{o:x}" for o in offs))
    return rows


def decompile_file(path, out_dir, write_map=False):
    """.scb -> <out_dir>/<name>.scp, .boo -> .moo (+ <name>.map with write_map).
    Returns (output path, warnings)."""
    from pathlib import Path
    path, out_dir = Path(path), Path(out_dir)
    sf = scb.load(path)
    d = Decompiler(sf)
    lines = d.run()
    dst = out_dir / (path.stem + (".moo" if path.suffix.lower() == ".boo" else ".scp"))
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("\r\n".join(lines) + "\r\n", encoding="cp1252", errors="replace", newline="")
    if write_map:
        dst.with_suffix(".map").write_text("\n".join(line_map(sf, lines)) + "\n", encoding="ascii", newline="")
    return dst, d.warnings

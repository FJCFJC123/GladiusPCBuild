"""Dog source -> Dog bytecode (.scp -> .scb, release format 0x86), the counterpart of decompile.py.

Rebuilds the game's own Dog compiler (Dog::Compiler, LucasArts' SoyPrawn engine; reference: the PC build's
compiler in protius.exe), so the output matches the retail .scb files:

  lexer     identifiers become symbols and string literals become strings as they are read (case-insensitive
            de-duplication, first spelling kept): that is the order of the SSTR / SYMB / NSTR tables
  parser    the bison grammar of the original (121 rules), as recursive descent building the same parse trees
            (MakeParseNode / MergeParseNodes); code is the post-order walk of those trees, nodes of opcodes 0..4
            (NOP, ARG, STATEMENT ...) emit nothing
  scopes    after parsing, each function (last first) claims the plain names it uses: a name nobody claimed yet is
            renamed in place to 'name#<function symbol>', a name another function claimed gets a new symbol at the
            end of the table; shared / global names and function names stay as they are
  shared    shared symbols (and None/Null/True/False) are numbered in table order, operands get bit 0x20000000
  branches  IF / ELSEIF / BRA_ENDIF, WHILE loops and FOR loops are patched to code offsets, string operands to
            NSTR offsets, function / goto labels become exports, 'call f()' becomes a local call (kind 1, code
            offset) when f is exported here, else an external one (kind 2, symbol 'f@file')
  release   like the retail compiler: no LINE ops or source info, locals 'name#<index>', and print / printf / debug
            statements replaced by 'push 0xDEADC0DE; discard' (their names and strings stay in the tables)

compile_source(text, name) -> bytes; errors raise CompileError(line, message).
"""
import re
import struct

from . import scb

OP = scb.OP
NOPS = scb.NOPERANDS
SHARED_BIT = scb.SHARED_BIT
GLOBAL_OBJ = scb.GLOBAL_OBJ
STRIPPED_CALLS = {"print", "printf", "debug"}   # release: debug output statements


class CompileError(Exception):
    def __init__(self, line, msg):
        super().__init__(f"line {line}: {msg}")
        self.line, self.msg = line, msg


# ------------------------------------------------------------------ tables
class Sym:
    __slots__ = ("name", "id", "type", "value", "shared", "glob", "callname", "funcdef", "decorated", "scope",
                 "off", "shared_index")

    def __init__(self, name, sid):
        self.name, self.id = name, sid
        self.type = 0x20 if "@" in name else 0
        self.value = 0
        self.shared = self.glob = self.callname = self.funcdef = self.decorated = False
        self.scope = -1
        self.off = 0
        self.shared_index = -1


def fixup_string(s):
    """FixupStringControlChars: \\n \\t \\" \\' \\\\ escapes, unescaped quotes dropped, other escapes vanish."""
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\":
            i += 1
            n = s[i] if i < len(s) else ""
            m = {"n": "\n", "t": "\t", '"': '"', "'": "'", "\\": "\\"}.get(n)
            if m is not None:
                out.append(m)
        elif c != '"':
            out.append(c)
        i += 1
    return "".join(out)


# ------------------------------------------------------------------ parse tree
class N:
    __slots__ = ("op", "o", "kids", "link", "off", "line")

    def __init__(self, op, o0=0, kids=(), line=-1):
        self.op = op
        self.o = [o0, 0, 0]
        self.kids = list(kids) + [None] * (4 - len(kids))
        self.link = None
        self.off = 0
        self.line = line


def mk(op, o0=0):
    return N(op, o0)


def merge(a, b, op, o0=0):
    return N(op, o0, (a, b))


def merge1(a, op, o0=0):
    return N(op, o0, (a,))


def emits(op):
    return op >= 5


def size(op):
    return 1 + 4 * NOPS[op]


def scan_for_arg(n):
    """ARG nodes under n, not looking inside function calls (nested 'call' arguments do count: the original's
    argc quirk)."""
    if n is None or n.op == OP["CALLFUNC"]:
        return 0
    return (n.op == OP["ARG"]) + scan_for_arg(n.kids[0]) + scan_for_arg(n.kids[1])


def count_child_nodes(n):
    return scan_for_arg(n.kids[0]) + scan_for_arg(n.kids[1])


def walk(n):
    """Post-order: kids, then the node (the code order)."""
    if n is None:
        return
    stack = [(n, 0)]
    while stack:
        x, k = stack.pop()
        if k < 4:
            stack.append((x, k + 1))
            if x.kids[k] is not None:
                stack.append((x.kids[k], 0))
        else:
            yield x


# ------------------------------------------------------------------ lexer
KEYWORDS = {k: k.upper() for k in (
    "case", "default", "if", "elseif", "else", "endif", "endwhile", "switch", "endswitch", "while", "do", "function",
    "endfunction", "structure", "endstructure", "for", "step", "next", "to", "goto", "continue", "break",
    "call", "int", "float", "vector", "article", "string", "array", "return", "returnval", "shared", "global")}
TWO = {"<=": "LE_OP", ">=": "GE_OP", "==": "EQ_OP", "!=": "NE_OP", "&&": "AND_OP", "||": "OR_OP", "++": "PLUS_PLUS",
       "--": "MINUS_MINUS", "+=": "PLUS_EQUALS", "-=": "MINUS_EQUALS", "*=": "MULT_EQUALS", "/=": "DIV_EQUALS",
       "|=": "OR_EQUALS", "&=": "AND_EQUALS", "^=": "XOR_EQUALS", "%=": "MOD_EQUALS"}
NUM = r"[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?"
RE_VEC = re.compile(r"<\s*(" + NUM + r")\s*,\s*(" + NUM + r")\s*,\s*(" + NUM + r")\s*>")
RE_ID = re.compile(r"[A-Za-z_][A-Za-z0-9_@]*")
RE_HEX = re.compile(r"0[xX][0-9A-Fa-f]+")
RE_FLOAT = re.compile(r"(?:\d+\.\d*|\.\d+)(?:[eE][-+]?\d+)?|\d+[eE][-+]?\d+")
RE_INT = re.compile(r"\d+")


def f32(x):
    return struct.unpack("<I", struct.pack("<f", float(x)))[0]


class Tok:
    __slots__ = ("kind", "val", "line", "text")

    def __init__(self, kind, val, line, text):
        self.kind, self.val, self.line, self.text = kind, val, line, text

    def __repr__(self):
        return f"{self.kind}:{self.text}"


class Compiler:
    def __init__(self, text, name=""):
        self.src, self.pos, self.line = text, 0, 1
        self.name = name                                   # this script (FILE list leaves it out)
        self.syms, self.sym_by = [], {}
        self.strs, self.str_by = [], {}
        self.funcs = []                                    # (node, function symbol id), head first (AddHead)
        self.q = []                                        # lexed tokens not consumed yet (lookahead)
        for n in ("self", "None", "Null", "True", "False"):
            self.new_sym(n)
        s = self.syms
        s[0].type, s[0].glob = 0x100, True                 # CompilerInit
        for k in (1, 2):
            s[k].type, s[k].shared = 0x100, True
        s[3].type, s[3].value, s[3].shared = 2, 1, True
        s[4].type, s[4].value, s[4].shared = 2, 0, True

    # ---- tables
    def new_sym(self, name):
        name = fixup_string(name)
        k = name.lower()
        if k in self.sym_by:
            return self.sym_by[k]
        s = Sym(name, len(self.syms))
        self.syms.append(s)
        self.sym_by[k] = s.id
        return s.id

    def add_sym_raw(self, name):                           # SymbolHolder ctor: appended, no search
        s = Sym(name, len(self.syms))
        self.syms.append(s)
        self.sym_by.setdefault(name.lower(), s.id)
        return s

    def new_str(self, text):
        text = fixup_string(text)
        if text.startswith("##") and len(text) >= 4:      # release: dialogue markup '##f-' / '##m-' / '##--' removed
            text = text[4:]
        k = text.lower()
        if k in self.str_by:
            return self.str_by[k]
        self.strs.append(text)
        self.str_by[k] = len(self.strs) - 1
        return len(self.strs) - 1

    # ---- lexer
    def lex(self):
        s, n = self.src, len(self.src)
        while True:
            while self.pos < n and s[self.pos] in " \t\r\n\f\v;":           # ';' is ignored, as in the original
                if s[self.pos] == "\n":
                    self.line += 1
                self.pos += 1
            if s.startswith("//", self.pos):
                e = s.find("\n", self.pos)
                self.pos = n if e < 0 else e
                continue
            if s.startswith("/*", self.pos):
                e = s.find("*/", self.pos + 2)
                e = n if e < 0 else e + 2
                self.line += s.count("\n", self.pos, e)
                self.pos = e
                continue
            break
        line = self.line
        if self.pos >= n:
            return Tok("EOF", None, line, "")
        c = s[self.pos]
        if c == '"':
            e = self.pos + 1
            while e < n and s[e] != '"' and s[e] != "\n":
                e += 2 if s[e] == "\\" else 1
            if e >= n or s[e] != '"':
                raise CompileError(line, "unterminated string")
            raw = s[self.pos:e + 1]
            self.pos = e + 1
            return Tok("STRING_LITERAL", self.new_str(raw), line, raw)
        if c == "<":
            m = RE_VEC.match(s, self.pos)
            if m:
                self.pos = m.end()
                return Tok("VCONSTANT", [f32(m.group(k)) for k in (1, 2, 3)], line, m.group(0))
        if c.isdigit() or (c == "." and self.pos + 1 < n and s[self.pos + 1].isdigit()):
            m = RE_HEX.match(s, self.pos)
            if m:
                self.pos = m.end()
                return Tok("ICONSTANT", int(m.group(0), 16) & 0xFFFFFFFF, line, m.group(0))
            m = RE_FLOAT.match(s, self.pos)
            if m:
                self.pos = m.end()
                return Tok("FCONSTANT", f32(m.group(0)), line, m.group(0))
            m = RE_INT.match(s, self.pos)
            self.pos = m.end()
            return Tok("ICONSTANT", int(m.group(0)) & 0xFFFFFFFF, line, m.group(0))
        m = RE_ID.match(s, self.pos)
        if m:
            self.pos = m.end()
            w = m.group(0)
            kw = KEYWORDS.get(w.lower())
            if kw:
                return Tok(kw, None, line, w)
            return Tok("IDENTIFIER", self.new_sym(w), line, w)
        two = s[self.pos:self.pos + 2]
        if two in TWO:
            self.pos += 2
            return Tok(TWO[two], None, line, two)
        self.pos += 1
        return Tok(c, None, line, c)

    def peek(self, k=0):
        while len(self.q) <= k:
            self.q.append(self.lex())
        return self.q[k]

    def take(self, kind=None):
        t = self.peek()
        if kind is not None and t.kind != kind:
            raise CompileError(t.line, f"expected {kind}, found '{t.text or 'end of file'}'")
        self.q.pop(0)
        return t

    def at(self, *kinds):
        return self.peek().kind in kinds

    # ---- program
    def parse(self):
        top = None
        while not self.at("EOF"):
            if not self.at("FUNCTION"):
                t = self.peek()
                raise CompileError(t.line, f"expected 'function', found '{t.text}'")
            top = merge(top, self.function(), 3)
        return top

    def function(self):
        self.take("FUNCTION")
        f = self.take("IDENTIFIER").val
        self.take("(")
        params = self.opt_list(reverse=True, close=")")
        self.take(")")
        body = self.opt_statements()
        self.take("ENDFUNCTION")
        self.syms[f].funcdef = True
        label = mk(OP["FUNCLABEL"], f)
        self.push_to_pop(params)
        c20 = merge(label, params, 3)
        c24 = merge1(body, 3)
        ret = merge(mk(OP["PUSHSYMBOL"], self.new_sym("None")), mk(OP["RETURNVAL"]), 3)
        node = merge(merge(c20, c24, 3), ret, 3)
        self.funcs.insert(0, (node, f))
        return node

    def push_to_pop(self, n):
        if n is None:
            return
        self.push_to_pop(n.kids[0])
        self.push_to_pop(n.kids[1])
        conv = {OP["PUSHINT"]: OP["POPINT"], OP["PUSHFLOAT"]: OP["POPFLOAT"], OP["PUSHSTRING"]: OP["POPSTRING"],
                OP["PUSHVECTOR"]: OP["POPVECTOR"], OP["PUSHSYMBOL"]: OP["POPSYMBOL"]}
        n.op = conv.get(n.op, n.op)

    # ---- statements
    STOPS = ("ENDFUNCTION", "ENDIF", "ELSEIF", "ELSE", "ENDWHILE", "NEXT", "}", "EOF")

    def opt_statements(self):
        lst = None
        while not self.at(*self.STOPS):
            lst = merge(lst, self.statement(), 3)
        return lst

    def statements(self):
        t = self.peek()
        if self.at(*self.STOPS):
            raise CompileError(t.line, f"statement expected before '{t.text or 'end of file'}'")
        return self.opt_statements()

    def statement(self):
        t = self.peek()
        k = t.kind
        if k == "FUNCTION":
            return self.function()
        if k == "IF":
            return self.if_statement()
        if k == "WHILE":
            return self.while_statement()
        if k == "FOR":
            return self.for_statement()
        if k == "SWITCH":
            self.take()
            return merge(self.expression(), None, OP["SWITCH"])
        if k == "CASE":
            self.take()
            return merge(self.expression(), mk(OP["CASE"]), 3)
        if k == "GOTO":
            self.take()
            return mk(OP["GOTO"], self.take("IDENTIFIER").val)
        if k == "BREAK":
            self.take()
            return mk(OP["BREAK"])
        if k == "CONTINUE":
            self.take()
            return mk(OP["CONTINUE"])
        if k == "RETURN":
            self.take()
            self.take("(")
            e = None if self.at(")") else self.assignment()
            self.take(")")
            if e is None:
                e = mk(OP["PUSHSYMBOL"], self.new_sym("None"))
            return merge(e, mk(OP["RETURNVAL"]), 3)
        if k == "{":
            self.take()
            body = None if self.at("}") else self.statements()
            self.take("}")
            return body if body is not None else mk(0)
        if k in ("INT", "FLOAT", "VECTOR", "ARTICLE", "STRING", "ARRAY", "SHARED", "GLOBAL"):
            return self.type_identifier()
        if k == "IDENTIFIER" and self.peek(1).kind == ":":   # label 'name:'
            self.take()
            self.take()
            return mk(OP["GOTOLABEL"], t.val)
        e = self.expression()
        if (e.op == OP["CALLFUNC"] and e.o[2] == GLOBAL_OBJ and
                self.syms[e.o[0]].name.lower() in STRIPPED_CALLS):
            e = mk(OP["PUSHINT"], 0xDEADC0DE)               # release compiler: debug output removed
        return merge(e, mk(OP["DISCARD"]), 3)

    def if_statement(self):
        self.take("IF")
        cond = self.expression()
        then = self.opt_statements()
        c38 = merge1(then, 3, 0xDDDDDDDD)
        c40 = merge1(cond, OP["IF"])
        t2 = merge(c40, c38, 3)
        elifs = None
        while self.at("ELSEIF"):
            self.take()
            c = self.expression()
            body = self.statements()
            c50 = merge(mk(OP["BRA_ENDIF"]), c, OP["ELSEIF"])
            elifs = merge(elifs, merge(c50, body, 3), 3)
        els = None
        if self.at("ELSE"):
            self.take()
            body = self.statements()
            els = merge(mk(OP["BRA_ENDIF"]), body, OP["ELSE"])
        self.take("ENDIF")
        t2 = merge(t2, elifs, 3)
        t2 = merge(t2, els, 3)
        return merge(t2, mk(OP["ENDIF"]), 3)

    def while_statement(self):
        self.take("WHILE")
        cond = self.expression()
        body = self.opt_statements()
        self.take("ENDWHILE")
        w, gw = mk(OP["WHILE"]), mk(OP["GOENDWHILE"])
        node = merge(merge(w, merge(cond, gw, 1), 1), body, OP["ENDWHILE"])
        node.link = w
        gw.link = node
        return node

    def best_guess_type(self, n):
        if n is None:
            return 0
        t = {OP["PUSHINT"]: OP["INT"], OP["PUSHFLOAT"]: OP["FLOAT"], OP["PUSHSTRING"]: OP["STRING"]}.get(n.op)
        if t:
            return t
        for k in n.kids:
            t = self.best_guess_type(k)
            if t:
                return t
        return 0

    def for_statement(self):
        self.take("FOR")
        v = self.take("IDENTIFIER").val
        self.take("=")
        a = self.expression()
        self.take("TO")
        b = self.expression()
        step = None
        if self.at("STEP"):
            self.take()
            step = self.expression()
        body = self.opt_statements()
        self.take("NEXT")
        t = self.take("IDENTIFIER")
        if t.val != v:
            raise CompileError(t.line, "for loop: 'next' names a different variable")
        c68 = merge(mk(OP["PUSHSYMBOL"], v), a, OP["ASSIGN"])
        c6c = mk(self.best_guess_type(a), v)
        c68 = merge(merge(c6c, c68, 3), mk(OP["DISCARD"]), 3)
        c74 = merge1(c68, OP["FOR"])
        c78 = merge(c74, body, 3)
        c7c = merge(mk(OP["PUSHSYMBOL"], v), b, OP["LT"])
        c80 = merge(c7c, None, OP["SKIP"])
        tmp = merge(c78, c80, 3)
        c88 = merge(mk(OP["PLUS_EQUALS"]), mk(OP["DISCARD"]), 3)
        if step is None:
            step = mk(OP["PUSHFLOAT"], f32(1.0)) if c6c.op == OP["FLOAT"] else mk(OP["PUSHINT"], 1)
        t2 = merge(mk(OP["PUSHSYMBOL"], v), merge(step, c88, 3), 3)
        c98 = mk(OP["NEXT_SYM"], v)
        c98.link = c74
        c80.link = c98
        return merge(merge(tmp, t2, 3), c98, 3)

    DECL = {"INT": "INT", "FLOAT": "FLOAT", "VECTOR": "VECTOR", "ARTICLE": "ARTICLE", "STRING": "STRING"}
    ALLOC = {"ARRAY": "ALLOC_ARRAY", "INT": "ALLOC_INT_ARRAY", "FLOAT": "ALLOC_FLOAT_ARRAY"}

    def basic_or_vardecl(self):
        t = self.take()
        x = self.take("IDENTIFIER").val
        if t.kind in self.ALLOC and (t.kind == "ARRAY" or self.at("[")):
            self.take("[")
            e = self.expression()
            self.take("]")
            return merge(mk(OP["PUSHSYMBOL"], x), e, OP[self.ALLOC[t.kind]])
        if t.kind == "ARRAY":
            raise CompileError(t.line, "array needs a size")
        decl = mk(OP[self.DECL[t.kind]], x)
        if self.at("=") and t.kind in ("INT", "FLOAT", "ARTICLE", "STRING"):
            self.take()
            e = self.expression()
            asg = merge(merge(mk(OP["PUSHSYMBOL"], x), e, OP["ASSIGN"]), mk(OP["DISCARD"]), 3)
            return merge(decl, asg, 3)
        return decl

    def type_identifier(self):
        t = self.peek()
        if t.kind not in ("SHARED", "GLOBAL"):
            return self.basic_or_vardecl()
        self.take()
        flag = "shared" if t.kind == "SHARED" else "glob"
        if self.at("IDENTIFIER"):
            x = self.take().val
            setattr(self.syms[x], flag, True)
            if self.at("="):
                self.take()
                e = self.expression()
                return merge(merge(mk(OP["PUSHSYMBOL"], x), e, OP["ASSIGN"]), mk(OP["DISCARD"]), 3)
            return mk(OP["NOP"], x)
        node = self.basic_or_vardecl()
        child = node.kids[0]                               # the declared name is the first child's symbol
        if child is not None:
            setattr(self.syms[child.o[0]], flag, True)
        return node

    # ---- expressions
    BIN = [  # lowest first: (tokens, opcode names)
        ({"OR_OP": "LOR"}),
        ({"AND_OP": "LAND"}),
        ({"|": "OR"}),
        ({"^": "XOR"}),
        ({"&": "AND"}),
        ({"EQ_OP": "EQ", "NE_OP": "NE"}),
        ({"<": "LT", ">": "GT", "LE_OP": "LE", "GE_OP": "GE"}),
        ({"+": "ADD", "-": "SUB"}),
        ({"*": "MULTIPLY", "/": "DIVIDE", "%": "MOD"}),
    ]
    ASSIGNS = {"=": "ASSIGN", "PLUS_EQUALS": "PLUS_EQUALS", "MINUS_EQUALS": "MINUS_EQUALS",
               "MULT_EQUALS": "MULT_EQUALS", "DIV_EQUALS": "DIV_EQUALS", "OR_EQUALS": "OR_EQUALS",
               "AND_EQUALS": "AND_EQUALS", "XOR_EQUALS": "XOR_EQUALS", "MOD_EQUALS": "MOD_EQUALS"}

    def expression(self):
        e = self.assignment()
        while self.at(","):
            self.take()
            e = merge(e, self.assignment(), 1)
        return e

    def assignment(self):
        u = self.unary()
        k = self.peek().kind
        if k in self.ASSIGNS:
            self.take()
            return merge(u, self.assignment(), OP[self.ASSIGNS[k]])
        return self.binary(0, u)

    def binary(self, level, left=None):
        if level == len(self.BIN):
            return left if left is not None else self.unary()
        e = self.binary(level + 1, left)
        ops = self.BIN[level]
        while self.peek().kind in ops:
            op = ops[self.take().kind]
            e = merge(e, self.binary(level + 1), OP[op])
        return e

    def unary(self):
        if self.at("-", "!"):
            t = self.take()
            opn = mk(OP["UNEG"] if t.kind == "-" else OP["LNEG"])
            return merge(self.unary(), opn, 1)
        return self.postfix()

    def args(self, reverse=False):
        lst = None
        if self.at(")"):
            return None
        while True:
            e = self.assignment()
            if lst is None:
                lst = merge1(e, OP["ARG"])
            else:
                lst = merge(e, lst, OP["ARG"]) if reverse else merge(lst, e, OP["ARG"])
            if not self.at(","):
                return lst
            self.take()

    def opt_list(self, reverse, close):
        return None if self.at(close) else self.args(reverse)

    def callfunc(self, fsym, argnode, obj):
        n = merge(argnode, None, OP["CALLFUNC"], fsym)
        n.o[1] = count_child_nodes(n)
        n.o[2] = obj
        self.syms[fsym].callname = True
        return n

    def postfix(self):
        t = self.peek()
        if t.kind == "CALL":
            self.take()
            f = self.take("IDENTIFIER").val
            self.take("(")
            a = self.args()
            self.take(")")
            n = merge1(a, OP["CALL"], 0)
            n.o[1] = f
            n.o[2] = count_child_nodes(n)
            self.syms[f].callname = True
            e = n
        elif t.kind == "IDENTIFIER":
            self.take()
            if self.at("("):
                self.take()
                a = self.args()
                self.take(")")
                e = self.callfunc(t.val, merge(None, a, 1), GLOBAL_OBJ)
            elif self.at("."):
                self.take()
                m = self.take("IDENTIFIER")
                if self.at("("):
                    self.take()
                    a = self.args()
                    self.take(")")
                    e = self.callfunc(m.val, a, t.val)
                else:
                    sid = self.new_str(self.syms[m.val].name)
                    arg = merge1(mk(OP["PUSHSTRING"], sid), OP["ARG"])
                    e = self.callfunc(self.new_sym("_GetArticle"), arg, t.val)
            else:
                e = mk(OP["PUSHSYMBOL"], t.val)
        else:
            e = self.primary()
        while self.at("["):
            self.take()
            i = self.expression()
            self.take("]")
            e = merge(e, i, OP["ARRAYREF"])
        return e

    def primary(self):
        t = self.take()
        if t.kind == "ICONSTANT":
            return mk(OP["PUSHINT"], t.val)
        if t.kind == "FCONSTANT":
            return mk(OP["PUSHFLOAT"], t.val)
        if t.kind == "STRING_LITERAL":
            return mk(OP["PUSHSTRING"], t.val)
        if t.kind == "VCONSTANT":
            n = mk(OP["PUSHVECTOR"])
            n.o = list(t.val)
            return n
        if t.kind == "(":
            e = self.expression()
            self.take(")")
            return e
        raise CompileError(t.line, f"unexpected '{t.text or 'end of file'}'")

    # ---- after parsing: scopes (ProcessAllFunctions)
    SYM_OP0 = {OP[n] for n in ("GOTO", "INT", "FLOAT", "STRING", "VECTOR", "ARTICLE", "PUSHSYMBOL", "POPSYMBOL")}

    def process_functions(self):
        funcsyms = {f for _, f in self.funcs}
        for node, f in self.funcs:
            for n in walk(node):
                if n.op in self.SYM_OP0:
                    k = 0
                elif n.op in (OP["CALL"], OP["CALLFUNC"]):
                    k = 2
                else:
                    continue
                sid = n.o[k]
                if not (0 <= sid < len(self.syms)):
                    continue
                s = self.syms[sid]
                if s.callname:
                    dec = f"{s.name}#{f}"
                    hit = next((x for x in self.syms if x.name.lower() == dec.lower()), None)
                    if hit is None:
                        hit = self.add_sym_raw(dec)
                    hit.decorated = True
                    n.o[k] = hit.id
                elif s.scope < 0 or s.scope == f or s.shared or s.glob:
                    if s.scope < 0 and not s.shared and not s.glob and sid not in funcsyms:
                        self.sym_by.pop(s.name.lower(), None)
                        s.name = f"{s.name}#{f}"
                        self.sym_by.setdefault(s.name.lower(), s.id)
                        s.scope, s.decorated = f, True
                else:
                    base = s.name.split("#", 1)[0]
                    nid = self.new_sym(f"{base}#{f}")
                    self.syms[nid].decorated = True
                    self.syms[nid].scope = f
                    n.o[k] = nid

    # ---- binary output
    def build(self, tree):
        self.process_functions()
        acc = 0
        for s in self.syms:
            s.off = acc
            acc += len(s.name.encode("cp1252", "replace")) + 1
        str_off, acc = [], 0
        for t in self.strs:
            str_off.append(acc)
            acc += len(t.encode("cp1252", "replace")) + 1
        out = bytearray(76)
        toc = {}

        def align():
            while len(out) % 4:
                out.append(0)

        def strings(tag, items):
            align()
            toc[tag] = len(out)
            blob = b"".join(x.encode("cp1252", "replace") + b"\0" for x in items)
            out.extend(tag[::-1].encode() + struct.pack("<II", len(blob), len(items)) + blob)

        strings("NSTR", self.strs)
        strings("SSTR", [s.name for s in self.syms])
        align()
        toc["SYMB"] = len(out)
        out.extend(b"BMYS" + struct.pack("<I", 12 * len(self.syms)))
        for s in self.syms:
            out.extend(struct.pack("<III", s.off, s.type, s.value))
        shared = [s for s in self.syms if s.shared]
        if shared:
            align()
            toc["SASY"] = len(out)
            out.extend(b"YSAS" + struct.pack("<I", 12 * len(shared)))
            for k, s in enumerate(shared):
                out.extend(struct.pack("<III", s.off, s.type, s.value))
                s.shared_index = k
        # shared operands
        def shr(sid):
            if 0 <= sid < len(self.syms) and self.syms[sid].shared:
                return self.syms[sid].shared_index | SHARED_BIT
            return sid
        op0 = self.SYM_OP0 | {OP[n] for n in ("FUNCLABEL", "GOTOLABEL", "PRINT_ID", "PRINT_STRING")}
        nodes = list(walk(tree))
        for n in nodes:
            if n.op == OP["CALL"]:
                n.o[1] = shr(n.o[1])
            elif n.op == OP["CALLFUNC"]:
                n.o[2] = shr(n.o[2])
                n.o[0] = shr(n.o[0])
            elif n.op in op0:
                n.o[0] = shr(n.o[0])
        # opcodes: offsets, loop jumps, serialization
        pc = 0
        for n in nodes:
            if emits(n.op):
                n.off = pc
                pc += size(n.op)
        for n in nodes:
            if n.link is not None:
                tgt = {OP["GOENDWHILE"]: "ENDWHILE", OP["ENDWHILE"]: "WHILE", OP["SKIP"]: "NEXT_SYM",
                       OP["NEXT_SYM"]: "FOR"}.get(n.op)
                if tgt:
                    n.o[0] = n.link.off + size(OP[tgt])
        code = bytearray()
        for n in nodes:
            if n.op == OP["CALLFUNC"]:
                n.o[1] = count_child_nodes(n)
            if emits(n.op):
                code.append(n.op)
                for k in range(NOPS[n.op]):
                    code.extend(struct.pack("<I", n.o[k] & 0xFFFFFFFF))
        exports = self.patch(code, str_off)
        align()
        toc["CODE"] = len(out)
        out.extend(b"EDOC" + struct.pack("<I", len(code)) + code)
        align()
        toc["EXRT"] = len(out)
        out.extend(b"TRPX" + struct.pack("<I", len(exports)))
        for name_off, at in exports:
            out.extend(struct.pack("<IIII", 0, 0, name_off, at) + bytes(20))     # release: 36-byte entries
        files = self.file_list()
        if files:
            align()
            toc["FILE"] = len(out)
            out.extend(b"ELIF" + struct.pack("<I", len(files)))
            for f in files:
                out.extend(f.encode("cp1252", "replace") + b"\0")
            align()
        hdr = bytearray(b"SGOD" + struct.pack("<HH", 0x86, 1))
        for tag in ("NSTR", "SSTR", "SYMB", "SASY", "CODE", "EXRT", "FILE", "INFO"):
            hdr.extend(tag[::-1].encode() + struct.pack("<I", toc.get(tag, 0xFFFFFFFF)))
        hdr.extend(b"\0\0\0\0")
        out[:76] = hdr
        return bytes(out)

    def patch(self, code, str_off):
        """MyPatchBranches: IF / ELSEIF / BRA_ENDIF targets, string offsets, exports, call kinds."""
        def ins():
            p = 0
            while p < len(code):
                yield p, code[p]
                p += size(code[p])
        offs = list(ins())
        u32 = lambda p: struct.unpack_from("<I", code, p)[0]
        def put(p, v):
            struct.pack_into("<I", code, p, v & 0xFFFFFFFF)
        IF, ELSEIF, ENDIF, BRA = OP["IF"], OP["ELSEIF"], OP["ENDIF"], OP["BRA_ENDIF"]
        for i, (p, op) in enumerate(offs):
            if op in (IF, ELSEIF):
                depth = 0
                for q, o in offs[i + 1:]:
                    if o == IF:
                        depth += 1
                    elif o in (ENDIF, BRA):
                        if depth == 0:
                            put(p + 1, q + size(o) if o == BRA else q)
                            break
                        if o == ENDIF:
                            depth -= 1
        exports = []
        for p, op in offs:
            if op == OP["PUSHSTRING"]:
                put(p + 1, str_off[u32(p + 1)])
            elif op in (OP["FUNCLABEL"], OP["GOTOLABEL"]):
                sid = u32(p + 1)
                exports.append((self.syms[sid].off if sid < len(self.syms) else 0, p))
        for p, op in offs:
            if op == OP["CALL"]:
                sid = u32(p + 5)
                put(p + 1, 2)
                if sid < len(self.syms):
                    hit = next((at for name_off, at in exports if name_off == self.syms[sid].off), None)
                    if hit is not None:
                        put(p + 1, 1)
                        put(p + 5, hit)
        for i, (p, op) in enumerate(offs):
            if op == BRA:
                depth = 0
                for q, o in offs[i + 1:]:
                    if o == IF:
                        depth += 1
                    if o == ENDIF:
                        if depth == 0:
                            put(p + 1, q)
                            break
                        depth -= 1
        return exports

    def file_list(self):
        """Other scripts this one calls into ('func@file' symbols): '<file>.scp', in the order of the original's
        32-bucket hash table."""
        seen, out = set(), []
        me = self.name.lower()
        for s in self.syms:
            if s.type != 0x20 or "@" not in s.name:
                continue
            part = s.name.split("@", 1)[1]
            if not part or part.lower() == me:
                continue
            f = part + ".scp"
            if f.lower() not in seen:
                seen.add(f.lower())
                out.append(f)
        return hash_order(out)


def _crc_table():
    t = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
        t.append(c)
    return t


_T = _crc_table()


def hash_order(names):
    """HashedArray<int,32> iteration order (verified against the retail FILE lists)."""
    def h(s):
        v = 0
        for ch in s.encode("cp1252", "replace"):
            v = _T[(v ^ (ch & 0xDF)) & 0xFF] ^ (v >> 8)
        return v
    return sorted(names[::-1], key=lambda s: h(s) & 31)   # buckets in order, newest first inside a bucket


def compile_source(text, name=""):
    c = Compiler(text, name)
    tree = c.parse()
    return c.build(tree)


def compile_file(src, dst=None):
    """.scp -> .scb (.moo -> .boo) next to it, or at dst."""
    from pathlib import Path
    src = Path(src)
    data = compile_source(src.read_text(encoding="cp1252", errors="replace"), src.stem)
    if dst is None:
        dst = src.with_suffix(".boo" if src.suffix.lower() == ".moo" else ".scb")
    Path(dst).write_bytes(data)
    return Path(dst)

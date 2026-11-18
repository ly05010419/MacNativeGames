#!/usr/bin/env python3
"""c1_bounded.tsv: the translated shaders whose every read of c1 (the shader's own constant segment) is proven to stay
inside its data rounded up to 256 bytes plus the 256 zero bytes after it (2026-11-16, Ocarina of Time's memory).

The runtime (nvn_mtl.m c1_store) gives a shader's c1 an exclusive 64 KiB of zeros unless every read is proven to stay in
that span, as reads past the data must give 0 (as on the Switch); proven ones share 4 MiB banks (data, then the 256 zero
bytes, then the next shader). At load it proves only literal indexes (c1_reads_bounded). Ocarina of Time's translated MSL
reads c1 at computed indexes (`(*spvDescriptorSet0.fp_c1)._m0[int(_3227)][_3229]`, the index a chain of min / shifts /
masks): 11,874 of its 15,904 shaders with c1 failed that rule, and a session made 7,433 exclusive buffers, 464 MB.

This proves the computed indexes offline, by interval arithmetic over each variable's definitions (the idea of nxnative's
dm/gfx/tools/c1_guard.py; this is our own code). Anything not understood is unbounded: the list may only miss a shader,
never name one that can read past its span. The packager writes the list beside build/shader-map.tsv (games/oot/build.py
c1_list); the runtime shares a listed shader's c1 when its MSL has the listed byte length, and judges every other shader
as before.

Soundness rules: only scalar int / uint / bool variables named _<digits> are tracked; a variable needs a one-line
declaration, and its value is the union of its initialiser and every plain `_N = expr;` (SPIR-V's phi copies: a variable
declared without a value is read only after one of them, as SPIR-V's dominance rules require); any other write (compound
assignment, ++ / --, a component write, address-of, an argument to a helper with a non-const thread& parameter, a second
declaration) makes it unknown, and so does reaching it again while it is evaluated (a loop). 32-bit wrap: a possible
overflow or an out-of-range conversion gives the whole type; shifts need a known amount in [0, 31]; division a positive
divisor. Floats, vectors, buffer / texture reads and stage inputs are unknown. A read at float4 index i is in the span if
0 <= i and 16 * (i + 1) <= span; a dynamic component index (x[i][j]) must lie in [0, 3].

  python3 -I tools/c1_bounds.py <shader-seed dir> <out tsv>
"""
import json
import os
import re
import sys
import threading
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor

HEADER = '# c1 bounded v1'
I32_MIN, I32_MAX, U32_MAX = -(1 << 31), (1 << 31) - 1, (1 << 32) - 1
TYPE_RANGE = {'int': (I32_MIN, I32_MAX), 'uint': (0, U32_MAX), 'bool': (0, 1)}
TRACKED = ('int', 'uint', 'bool')


class V:
    """An integer value's range (int / uint / bool), or an untracked kind (float, other) with no range."""
    __slots__ = ('kind', 'lo', 'hi')

    def __init__(self, kind, lo=None, hi=None):
        self.kind, self.lo, self.hi = kind, lo, hi

    def ranged(self):
        return self.kind in TYPE_RANGE


def top(kind):
    return V(kind, *TYPE_RANGE[kind]) if kind in TYPE_RANGE else V(kind)


def fit(kind, lo, hi):
    """The exact result [lo, hi] in a 32-bit type: any value out of the type's range wraps, so the whole type."""
    tlo, thi = TYPE_RANGE[kind]
    return V(kind, lo, hi) if tlo <= lo and hi <= thi else top(kind)


def union(a, b):
    if a.kind != b.kind or not a.ranged():
        return top(a.kind if a.kind == b.kind else 'other')
    return V(a.kind, min(a.lo, b.lo), max(a.hi, b.hi))


def convert(v, kind):
    """A C conversion of an integer value to int / uint (32-bit, bits kept)."""
    if v.kind == 'bool':
        v = V('int', v.lo, v.hi)
    if v.kind not in ('int', 'uint'):
        return top(kind)
    if v.kind == kind:
        return v
    if kind == 'int':
        if v.hi <= I32_MAX:
            return V('int', v.lo, v.hi)
        if v.lo > I32_MAX:
            return V('int', v.lo - (1 << 32), v.hi - (1 << 32))
        return top('int')
    if v.lo >= 0:
        return V('uint', v.lo, v.hi)
    if v.hi < 0:
        return V('uint', v.lo + (1 << 32), v.hi + (1 << 32))
    return top('uint')


# ------------------------------------------------------------------------------------------------ expressions
TOKEN = re.compile(r"""
  (?P<ws>\s+)
| (?P<num>0[xX][0-9a-fA-F]+[uU]?|\d+\.\d*(?:[eE][-+]?\d+)?[fhFH]?|\d+[eE][-+]?\d+[fhFH]?|\.\d+(?:[eE][-+]?\d+)?[fhFH]?|\d+[uU]?[lL]?)
| (?P<id>[A-Za-z_]\w*)
| (?P<op><<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^~!<>?:()\[\],.=])
""", re.X)
PRECEDENCE = {'||': 1, '&&': 2, '|': 3, '^': 4, '&': 5, '==': 6, '!=': 6, '<': 7, '<=': 7, '>': 7, '>=': 7,
              '<<': 8, '>>': 8, '+': 9, '-': 9, '*': 10, '/': 10, '%': 10}


class ParseError(Exception):
    pass


class Parser:
    def __init__(self, text):
        self.t, i = [], 0
        while i < len(text):
            m = TOKEN.match(text, i)
            if not m:
                raise ParseError(text[i])
            i = m.end()
            if m.lastgroup != 'ws':
                self.t.append((m.lastgroup, m.group()))
        self.i = 0

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else (None, None)

    def eat(self, want=None):
        tok = self.peek()
        if tok[0] is None or (want is not None and tok[1] != want):
            raise ParseError(want)
        self.i += 1
        return tok

    def whole(self):
        e = self.expr()
        if self.i != len(self.t):
            raise ParseError('trailing')
        return e

    def expr(self):
        c = self.binary(1)
        if self.peek()[1] != '?':
            return c
        self.eat('?'); a = self.expr(); self.eat(':'); b = self.expr()
        return ('tern', c, a, b)

    def binary(self, least):
        left = self.unary()
        while True:
            tok = self.peek()
            p = PRECEDENCE.get(tok[1]) if tok[0] == 'op' else None
            if p is None or p < least:
                return left
            self.eat()
            left = ('bin', tok[1], left, self.binary(p + 1))

    def unary(self):
        tok = self.peek()
        if tok[0] == 'op' and tok[1] in ('-', '+', '!', '~', '*', '&'):
            self.eat()
            return ('un', tok[1], self.unary())
        return self.postfix(self.primary())

    def primary(self):
        kind, val = self.eat()
        if kind == 'num':
            return ('num', val)
        if kind == 'id':
            if val == 'as_type' and self.peek()[1] == '<':
                self.eat('<'); ty = []
                while self.peek()[1] != '>':
                    ty.append(self.eat()[1])
                self.eat('>'); self.eat('('); arg = self.expr(); self.eat(')')
                return ('as_type', ''.join(ty), arg)
            if self.peek()[1] == '<':
                raise ParseError('template')
            return ('id', val)
        if val == '(':
            e = self.expr(); self.eat(')')
            return e
        raise ParseError(val)

    def postfix(self, e):
        while True:
            v = self.peek()[1]
            if v == '(':
                self.eat('('); args = []
                if self.peek()[1] != ')':
                    args.append(self.expr())
                    while self.peek()[1] == ',':
                        self.eat(','); args.append(self.expr())
                self.eat(')')
                e = ('call', e, args)
            elif v == '[':
                self.eat('['); idx = self.expr(); self.eat(']')
                e = ('index', e, idx)
            elif v == '.' and self.peek(1)[0] == 'id':
                self.eat('.'); e = ('member', e, self.eat()[1])
            else:
                return e


def number(text):
    t = text.lower()
    hexa = t.startswith('0x')
    if not hexa and ('.' in t or 'e' in t or t.endswith(('f', 'h'))):
        return V('float')
    n = int(t.rstrip('ul'), 16 if hexa else 10)
    if 'u' in t or n > I32_MAX:
        return V('uint', n, n) if n <= U32_MAX else V('other')
    return V('int', n, n)


# ------------------------------------------------------------------------------------------------ one shader's variables
TYPES = r'(?:int|uint|bool|float|half|short|ushort|long|ulong|char|uchar|(?:int|uint|float|half|bool|short|ushort)[234](?:x[234])?)'
DECL = re.compile(r'^\s*(?:const\s+)?(' + TYPES + r')\s+(_\d+)\s*=\s*(.+);\s*$')
DECL_NO_VALUE = re.compile(r'^\s*(' + TYPES + r')\s+(_\d+)\s*;\s*$')
ASSIGN = re.compile(r'^\s*(_\d+)\s*=\s*(.+);\s*$')
DEF_PREFIX = re.compile(r'\s*(?:const\s+)?(?:' + TYPES + r')?\s*')
TYPED_NAME = re.compile(r'\b' + TYPES + r'\s*&?\s+(_\d+)\b')
WRITE = re.compile(r'(?<![\w.])(_\d+)\b((?:\s*(?:\[[^\]]*\]|\.\w+))*)\s*(\+\+|--|<<=|>>=|[-+*/%&|^]=|=(?!=))')
PRE_INC = re.compile(r'(\+\+|--)\s*(_\d+)\b')
ADDRESS = re.compile(r'(?:^|[(,=?:]|return)\s*&\s*(_\d+)\b')
REF_FUNCTION = re.compile(r'(\w+)\s*\(([^()]*\bthread\s+(?!const\b)[^()]*&[^()]*)\)\s*$')
VARIABLE = re.compile(r'_\d+$')


def matching(s, i, open_c, close_c):
    depth = 0
    for j in range(i, len(s)):
        if s[j] == open_c:
            depth += 1
        elif s[j] == close_c:
            depth -= 1
            if depth == 0:
                return j
    return -1


class Shader:
    """The scalar variables of one shader's MSL: their declared type and every value assigned to them."""

    def __init__(self, lines):
        self.decl, self.defs, self.unknown, self.memo, self.busy, self.parsed = {}, defaultdict(list), set(), {}, set(), {}
        ref_functions = {m.group(1) for ln in lines if not ln.rstrip().endswith(';') for m in [REF_FUNCTION.search(ln)] if m}
        ref_call = re.compile(r'\b(' + '|'.join(map(re.escape, sorted(ref_functions))) + r')\s*\(') if ref_functions else None
        for ln in lines:
            own = None
            m = DECL.match(ln) or DECL_NO_VALUE.match(ln)
            if m:
                own = m.group(2)
                if own in self.decl:
                    self.unknown.add(own)   # declared twice
                self.decl[own] = m.group(1)
                if m.re is DECL:
                    self.defs[own].append(m.group(3))
            else:
                m = ASSIGN.match(ln)
                if m:
                    own = m.group(1)
                    if re.search(r'(?<![=!<>])=(?!=)', m.group(2)):
                        self.unknown.add(own)   # a = b = c
                    self.defs[own].append(m.group(2))
            for mm in TYPED_NAME.finditer(ln):   # parameters, loop headers, several per line
                if mm.group(1) != own:
                    self.unknown.add(mm.group(1))
            for mm in re.finditer(r'\b(_\d+)\s*\[\s*\d+\s*\]\s*[;=]', ln):
                self.unknown.add(mm.group(1))   # an array
            for mm in WRITE.finditer(ln):   # every write but the line's own whole-variable definition
                v, access, op = mm.groups()
                if not (v == own and not access.strip() and op == '=' and DEF_PREFIX.fullmatch(ln[:mm.start()])):
                    self.unknown.add(v)
            self.unknown.update(mm.group(2) for mm in PRE_INC.finditer(ln))
            self.unknown.update(mm.group(1) for mm in ADDRESS.finditer(ln))
            if ref_call:
                for mm in ref_call.finditer(ln):
                    end = matching(ln, mm.end() - 1, '(', ')')
                    self.unknown.update(re.findall(r'\b_\d+\b', ln[mm.end():end if end > 0 else len(ln)]))

    def parse(self, text):
        if text not in self.parsed:
            try:
                self.parsed[text] = Parser(text).whole()
            except ParseError:
                self.parsed[text] = ('unparsed',)
        return self.parsed[text]

    def var(self, name):
        if name in self.memo:
            return self.memo[name]
        ty = self.decl.get(name)
        if ty is None:
            return V('other')
        kind = ty if ty in TRACKED else 'float' if ty in ('float', 'half') else 'other'
        if kind not in TRACKED or name in self.unknown or not self.defs[name]:
            self.memo[name] = top(kind)
            return self.memo[name]
        if name in self.busy:
            return top(kind)   # reached again while evaluated: a loop
        self.busy.add(name)
        value = None
        for text in self.defs[name]:
            v = self.eval(self.parse(text))
            v = (v if v.kind == 'bool' else top('bool')) if kind == 'bool' else convert(v, kind)
            value = v if value is None else union(value, v)
        self.busy.discard(name)
        self.memo[name] = value
        return value

    def eval(self, e):
        t = e[0]
        if t == 'num':
            return number(e[1])
        if t == 'id':
            if VARIABLE.match(e[1]):
                return self.var(e[1])
            return V('bool', int(e[1] == 'true'), int(e[1] == 'true')) if e[1] in ('true', 'false') else V('other')
        if t == 'as_type':
            v = self.eval(e[2])
            if e[1] in ('int', 'uint'):
                return convert(v, e[1]) if v.kind in ('int', 'uint') else top(e[1])
            return V('other')
        if t == 'tern':
            return union(self.eval(e[2]), self.eval(e[3]))
        if t == 'un':
            return V('other') if e[1] in ('*', '&') else self.unary(e[1], self.eval(e[2]))
        if t == 'bin':
            return self.binary(e[1], self.eval(e[2]), self.eval(e[3]))
        if t == 'call':
            return self.call(e)
        return V('other')   # members, indexes into buffers and vectors, unparsed text

    def call(self, e):
        if e[1][0] != 'id':
            return V('other')
        name, vals = e[1][1], [self.eval(a) for a in e[2]]
        if name in ('int', 'uint') and len(vals) == 1:
            return convert(vals[0], name)
        if name == 'bool' and len(vals) == 1:
            return V('bool', 0, 1)
        if name in ('min', 'max') and len(vals) == 2:
            a, b = vals
            if a.kind == b.kind and a.kind in ('int', 'uint'):
                pick = min if name == 'min' else max
                return V(a.kind, pick(a.lo, b.lo), pick(a.hi, b.hi))
            return V('other')
        if name == 'clamp' and len(vals) == 3:
            x, a, b = vals
            if x.kind == a.kind == b.kind and x.kind in ('int', 'uint') and a.hi <= b.lo:
                return V(x.kind, min(max(x.lo, a.lo), b.lo), min(max(x.hi, a.hi), b.hi))
            return top(x.kind) if x.ranged() else V('other')   # min > max is undefined in MSL
        if name == 'abs' and len(vals) == 1 and vals[0].kind == 'int':
            v = vals[0]
            if v.lo == I32_MIN:
                return top('int')
            return V('int', 0 if v.lo <= 0 <= v.hi else min(abs(v.lo), abs(v.hi)), max(abs(v.lo), abs(v.hi)))
        if name == 'select' and len(vals) == 3:
            return union(vals[0], vals[1])
        return V('other')

    @staticmethod
    def unary(op, v):
        if op == '!':
            return V('bool', 0, 1)
        if op == '+':
            return v
        if v.kind == 'bool':
            v = convert(v, 'int')
        if not v.ranged():
            return V(v.kind)
        if op == '-':
            if v.kind == 'int':
                return fit('int', -v.hi, -v.lo)
            return V('uint', 0, 0) if v.lo == v.hi == 0 else top('uint')
        if op == '~':
            return V('int', ~v.hi, ~v.lo) if v.kind == 'int' else V('uint', U32_MAX - v.hi, U32_MAX - v.lo)
        return V('other')

    @staticmethod
    def binary(op, a, b):
        if op in ('==', '!=', '<', '<=', '>', '>=', '&&', '||'):
            return V('bool', 0, 1)
        if not (a.ranged() and b.ranged()):
            return V('float') if 'float' in (a.kind, b.kind) else V('other')
        a = convert(a, 'int') if a.kind == 'bool' else a
        b = convert(b, 'int') if b.kind == 'bool' else b
        if op in ('<<', '>>'):
            if not (0 <= b.lo and b.hi <= 31):
                return top(a.kind)
            if op == '>>':
                c = [a.lo >> b.lo, a.lo >> b.hi, a.hi >> b.lo, a.hi >> b.hi]
                return V(a.kind, min(c), max(c))
            return top(a.kind) if a.lo < 0 else fit(a.kind, a.lo << b.lo, a.hi << b.hi)
        kind = 'uint' if 'uint' in (a.kind, b.kind) else 'int'
        a, b = convert(a, kind), convert(b, kind)
        if op == '+':
            return fit(kind, a.lo + b.lo, a.hi + b.hi)
        if op == '-':
            return fit(kind, a.lo - b.hi, a.hi - b.lo)
        if op == '*':
            c = [a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi]
            return fit(kind, min(c), max(c))
        if op == '/':
            if b.lo <= 0:
                return top(kind)
            q = lambda x, y: abs(x) // y if x >= 0 else -(abs(x) // y)   # C truncates toward zero
            c = [q(a.lo, b.lo), q(a.lo, b.hi), q(a.hi, b.lo), q(a.hi, b.hi)]
            return V(kind, min(c), max(c))
        if op == '%':
            if b.lo <= 0:
                return top(kind)
            return V(kind, max(a.lo, -(b.hi - 1)) if a.lo < 0 else 0, min(a.hi, b.hi - 1) if a.hi > 0 else 0)
        if op == '&':
            caps = [v.hi for v in (a, b) if v.lo >= 0]
            return V(kind, 0, min(caps)) if caps else top(kind)
        if op in ('|', '^'):
            if a.lo >= 0 and b.lo >= 0:
                return V(kind, max(a.lo, b.lo) if op == '|' else 0, (1 << max(a.hi, b.hi).bit_length()) - 1)
            return top(kind)
        return V('other')


# ------------------------------------------------------------------------------------------------ one shader
def literal_rule(src, length):
    """nvn_mtl.m c1_reads_bounded: every read a literal index inside the span (the runtime proves these itself)."""
    limit = (((length + 255) & ~255) + 256) // 16
    for m in re.finditer(r'_c1\)\._m0\[', src):
        d = re.match(r'(\d+)u?\]', src[m.end():m.end() + 16])
        if not d or int(d.group(1)) >= limit:
            return False
    return True


def proven(src, length):
    """Every c1 read of this MSL proven inside ((length + 255) & ~255) + 256 bytes."""
    span = ((length + 255) & ~255) + 256
    names = set(re.findall(r'\b([a-z]+_c1)\b', src))
    if not names:
        return True
    lines = src.split('\n')
    shader = Shader(lines)
    name = re.compile(r'\b(' + '|'.join(map(re.escape, sorted(names))) + r')\b')
    for ln in lines:
        for mm in name.finditer(ln):
            before, after = ln[:mm.start()], ln[mm.end():]
            if re.match(r'\s*\[\[(id|buffer)\(\d+\)\]\]', after):
                continue   # its declaration
            if not (before.endswith('(*spvDescriptorSet0.') and after.startswith(')._m0[')):
                return False   # read some other way
            i = mm.end() + len(')._m0')
            j = matching(ln, i, '[', ']')
            if j < 0:
                return False
            index = shader.eval(shader.parse(ln[i + 1:j]))
            if index.kind not in ('int', 'uint') or index.lo < 0 or 16 * (index.hi + 1) > span:
                return False
            if ln[j + 1:j + 2] == '[':
                k = matching(ln, j + 1, '[', ']')
                comp = shader.eval(shader.parse(ln[j + 2:k])) if k > 0 else V('other')
                if not (comp.ranged() and comp.lo >= 0 and comp.hi <= 3):
                    return False
    return True


def judge(json_path):
    """(key, MSL bytes) when the runtime's own rule fails and the proof holds; None otherwise."""
    try:
        with open(json_path) as f:
            c1 = json.load(f).get('c1') or ''
        if not c1:
            return None
        metal = json_path[:-len('.json')] + '.metal'
        with open(metal, 'rb') as f:
            raw = f.read()
        src = raw.decode('utf-8')
        if literal_rule(src, len(c1) // 2):
            return None
        result = []

        def go():
            try:
                result.append(proven(src, len(c1) // 2))
            except (RecursionError, ValueError, IndexError):
                pass   # unproven
        worker = threading.Thread(target=go)   # deep expressions: a large stack (init)
        worker.start(); worker.join()
        if result and result[0]:
            return os.path.basename(json_path)[:-len('_b.json')], len(raw)
    except (OSError, ValueError, UnicodeDecodeError):
        return None   # unread is unproven
    return None


def init():
    threading.stack_size(512 << 20)
    sys.setrecursionlimit(300000)


def write_list(seed, out, stamp, workers=8):
    """out = the shaders of seed proven by this module, under a header naming stamp (the seed manifest's SHA-256); an
    existing list with the same stamp is kept. Returns (rows, kept)."""
    head = f'{HEADER} seed {stamp}'
    if os.path.isfile(out):
        with open(out) as f:
            if f.readline().rstrip('\n') == head:
                return sum(1 for line in f if line.strip() and not line.startswith('#')), True
    files = sorted(os.path.join(seed, n) for n in os.listdir(seed) if n.endswith('_b.json'))
    with ProcessPoolExecutor(max_workers=workers, initializer=init) as pool:
        rows = sorted(r for r in pool.map(judge, files, chunksize=64) if r)
    temporary = f'{out}.partial-{os.getpid()}'
    with open(temporary, 'w') as f:
        f.write(head + '\n# key<TAB>MSL bytes: every c1 read proven inside its data rounded up to 256 bytes plus 256 (tools/c1_bounds.py)\n')
        f.writelines(f'{k}\t{n}\n' for k, n in rows)
    os.replace(temporary, out)
    return len(rows), False


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    n, kept = write_list(sys.argv[1], sys.argv[2], 'manual')
    print(f'{n} shaders proven{" (kept)" if kept else ""}: {sys.argv[2]}')

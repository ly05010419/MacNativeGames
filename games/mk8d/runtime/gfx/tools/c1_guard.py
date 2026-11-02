#!/usr/bin/env python3
# c1_guard.py <dmsh_b version dir> <output tsv>: lists the shaders whose reads of c1 (the shader's embedded constant segment) are not proven to stay inside the span the host's
# shared c1 banks give them. nvn_mtl.m c1_store packs the c1 of unlisted shaders into 4 MB banks: the data rounded up to 256 bytes, then a 256-byte zero gap, then the next shader's data.
# A listed shader gets an exclusive 64 KiB of zeros instead (reads past the data return 0, as on the Switch). The host loads the list only when its header names its library's chain version
# and inputs stamp (dmsh_chain.py table_head). dmsh_build.py runs it after each full build.
# An index is proven when interval arithmetic over the whole expression, following each variable's definition, gives 0 <= index and 16 * (index + 1) <= that span (_m0 is float4[4096]).
# Literals, & K, % K, >> K, min, max, clamp and casts bound a value; inputs, buffer reads and anything not understood are unbounded. Every stage's c1 counts (vp_, fp_, cp_, gp_ ...).
# The first version followed only the last variable of an index and took any mask, min or clamp it met as proof: `_1 + (_2 & 3u)` with _1 read from an input passed,
# masks wider than the data passed, and compute shaders (cp_c1) were never checked.
import datetime
import json
import multiprocessing
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dmsh_chain   # noqa: E402


ACCESS = re.compile(r"\b[a-z]+_c1\)\._m0\[([^\]]+)\]")
DEFINITION = re.compile(r"\b(u?int|u?short|u?long|bool)\s+(_\d+)\s*=\s*([^;]+);")
ASSIGNMENT = re.compile(r"(?<![\w.])(_\d+)\s*(?:<<|>>|[-+*/%&|^])?=(?!=)|(?<![\w.])(_\d+)\s*(?:\+\+|--)|(?:\+\+|--)\s*(_\d+)\b")
TOKEN = re.compile(r"\s*(0[xX][0-9a-fA-F]+[uU]?|\d+\.\d*(?:[eE][-+]?\d+)?f?|\d+[uU]?|[A-Za-z_]\w*(?:\.\w+)*|\.\w+|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^~!?:(),<>\[\]])")
UNBOUNDED = (None, None)
CASTS = {"int", "uint", "short", "ushort", "long", "ulong"}
BINARY = {"||": 1, "&&": 2, "|": 3, "^": 4, "&": 5, "==": 6, "!=": 6, "<": 7, ">": 7, "<=": 7, ">=": 7, "<<": 8, ">>": 8, "+": 9, "-": 9, "*": 10, "/": 10, "%": 10}


def tokens(text):
    # A reinterpreting cast: an unsigned result is only known to be non-negative, a signed one is unbounded
    text = re.sub(r"\bas_type<u(?:int|short|long)>\s*\(", "as_type_u(", text)
    text = re.sub(r"\bas_type<\w+>\s*\(", "as_type(", text)
    out, i = [], 0
    while i < len(text):
        m = TOKEN.match(text, i)
        if not m:
            if text[i:].strip(): raise ValueError(text[i:])
            break
        out.append(m.group(1)); i = m.end()
    return out


def known(*values):
    return all(v is not None for v in values)


class Interval:
    """Bounds of integer expressions in one shader's MSL; definitions are evaluated once each, at most 32 deep."""
    def __init__(self, definitions):
        self.definitions, self.memo, self.depth = definitions, {}, 0

    def variable(self, name):
        if name in self.memo: return self.memo[name]
        if name not in self.definitions or self.depth > 32: return UNBOUNDED
        self.memo[name] = UNBOUNDED   # a cycle is unbounded
        self.depth += 1
        kind, text = self.definitions[name]
        try: value = self.expression(text)
        finally: self.depth -= 1
        if kind == "bool": value = (0, 1)
        elif kind.startswith("u") and not (known(value[0]) and value[0] >= 0): value = (0, None)   # an unsigned variable: a possibly negative value wraps to a huge one
        self.memo[name] = value
        return value

    def expression(self, text):
        try:
            self.toks, self.pos = tokens(text), 0
            value = self.ternary()
            if self.pos != len(self.toks): return UNBOUNDED
            return value
        except (ValueError, IndexError, RecursionError):
            return UNBOUNDED

    def peek(self): return self.toks[self.pos] if self.pos < len(self.toks) else None

    def take(self, want=None):
        t = self.toks[self.pos]
        if want is not None and t != want: raise ValueError(t)
        self.pos += 1
        return t

    def ternary(self):
        cond = self.binary(1)
        if self.peek() != "?": return cond
        self.take("?"); a = self.ternary(); self.take(":"); b = self.ternary()
        return (min(a[0], b[0]) if known(a[0], b[0]) else None, max(a[1], b[1]) if known(a[1], b[1]) else None)

    def binary(self, level):
        left = self.unary()
        while self.peek() in BINARY and BINARY[self.peek()] >= level:
            op = self.take(); right = self.binary(BINARY[op] + 1)
            left = self.combine(op, left, right)
        return left

    @staticmethod
    def combine(op, a, b):
        (al, ah), (bl, bh) = a, b
        if op in ("||", "&&", "==", "!=", "<", ">", "<=", ">="): return (0, 1)
        if op == "+": return (al + bl if known(al, bl) else None, ah + bh if known(ah, bh) else None)
        if op == "-": return (al - bh if known(al, bh) else None, ah - bl if known(ah, bl) else None)
        if op == "*":
            if not known(al, ah, bl, bh): return UNBOUNDED
            p = [al * bl, al * bh, ah * bl, ah * bh]; return (min(p), max(p))
        if op in ("/", ">>", "<<"):
            if not known(al, ah, bl, bh) or bl != bh or bl < 0 or (op == "/" and bl == 0): return UNBOUNDED
            if op == "<<": return (al << bl, ah << bl)
            if op == ">>": return (al >> bl, ah >> bl)
            return (al // bl, ah // bl) if al >= 0 else UNBOUNDED
        if op == "%":
            if not known(bl, bh) or bl != bh or bl <= 0: return UNBOUNDED
            return (0, bl - 1) if known(al) and al >= 0 else (-(bl - 1), bl - 1)
        if op == "&":   # x & K with K >= 0 lies in [0, K], whatever the sign of x
            caps = [h for l, h in (a, b) if known(l, h) and l >= 0]
            return (0, min(caps)) if caps else UNBOUNDED
        if op in ("|", "^"):
            if not known(al, ah, bl, bh) or al < 0 or bl < 0: return UNBOUNDED
            return (0, (1 << max(ah, bh).bit_length()) - 1)
        return UNBOUNDED

    def unary(self):
        t = self.peek()
        if t == "-": self.take(); l, h = self.unary(); return (-h if known(h) else None, -l if known(l) else None)
        if t == "+": self.take(); return self.unary()
        if t == "!": self.take(); self.unary(); return (0, 1)
        if t in ("~", "*"): self.take(); self.unary(); return UNBOUNDED   # complement; a dereference reads memory
        return self.postfix(self.primary())

    def postfix(self, value):
        while self.peek() is not None and (self.peek() == "[" or self.peek().startswith(".")):   # an array element or a member: unbounded
            if self.take() == "[": self.ternary(); self.take("]")
            value = UNBOUNDED
        return value

    def primary(self):
        t = self.take()
        if t == "(":
            v = self.ternary(); self.take(")"); return v
        if re.fullmatch(r"0[xX][0-9a-fA-F]+[uU]?|\d+[uU]?", t):
            v = int(t.rstrip("uU"), 0); return (v, v)
        if re.fullmatch(r"\d+\.\d*(?:[eE][-+]?\d+)?f?", t): return UNBOUNDED
        if not re.fullmatch(r"[A-Za-z_]\w*(?:\.\w+)*", t): raise ValueError(t)
        if self.peek() == "(":
            self.take("(")
            args = []
            if self.peek() != ")":
                args.append(self.ternary())
                while self.peek() == ",": self.take(","); args.append(self.ternary())
            self.take(")")
            return self.call(t, args)
        if re.fullmatch(r"_\d+", t):   # a temporary: its definition is in the same function
            save = (self.toks, self.pos)
            try: return self.variable(t)
            finally: self.toks, self.pos = save
        if t in ("true", "false"): return (0, 1)
        return UNBOUNDED   # inputs, uniforms, vector components, constants we do not track

    @staticmethod
    def call(name, args):
        if name == "as_type_u" and len(args) == 1: return (0, None)
        if name in CASTS and len(args) == 1:
            l, h = args[0]
            if name.startswith("u") and not (known(l) and l >= 0): return (0, None)   # a negative value wraps to a huge unsigned one
            return (l, h)
        if name == "min" and len(args) == 2:
            (al, ah), (bl, bh) = args
            hs = [h for h in (ah, bh) if h is not None]
            return (min(al, bl) if known(al, bl) else None, min(hs) if hs else None)
        if name == "max" and len(args) == 2:
            (al, ah), (bl, bh) = args
            ls = [l for l in (al, bl) if l is not None]
            return (max(ls) if ls else None, max(ah, bh) if known(ah, bh) else None)
        if name == "clamp" and len(args) == 3:
            return (args[1][0], args[2][1])
        return UNBOUNDED


def c1_span(json_path):
    """Bytes a shared bank gives the shader's c1: its data rounded up to 256 bytes plus the 256-byte zero gap (nvn_mtl.m c1_store); None when the reflection has no c1."""
    try:
        with open(json_path, encoding="utf-8") as stream: length = len(json.load(stream).get("c1") or "") // 2
    except (OSError, ValueError):
        return None
    return (length + 255) // 256 * 256 + 256


def definitions_of(source):
    """Integer temporaries written only where they are declared. A variable assigned again (a loop counter, a value merged from branches) is left out, so it reads as unbounded.
    A name may be declared more than once with the same text: vertex shaders carry a copy of the main function (main0_z, dmsh_build.py vs_depth); differing declarations are left out too"""
    writes, decls = {}, {}
    for m in ASSIGNMENT.finditer(source):
        name = m.group(1) or m.group(2) or m.group(3); writes[name] = writes.get(name, 0) + 1
    for kind, name, text in DEFINITION.findall(source): decls.setdefault(name, set()).add((kind, text.strip()))
    counts = {}
    for _, name, _ in DEFINITION.findall(source): counts[name] = counts.get(name, 0) + 1
    return {name: next(iter(d)) for name, d in decls.items() if len(d) == 1 and writes.get(name, 0) == counts[name]}


def proven(index, definitions, span):
    low, high = Interval(definitions).expression(index)
    return span is not None and known(low, high) and low >= 0 and 16 * (high + 1) <= span


def unproven(job):
    """(version dir, a .metal file name) → the shader's key when one of its c1 reads is not proven bounded, else None. Each shader is checked on its own, so main()
    spreads them over every core, as cb_extent.py does (Breath of the Wild's 19,592 libraries, FINAL FANTASY RESONANCE's 73,496: minutes on one core)"""
    version_dir, name = job
    with open(os.path.join(version_dir, name), encoding="utf-8") as stream:
        source = stream.read()
    accesses = set(ACCESS.findall(source))
    if not accesses:
        return None
    key = name[:-len("_b.metal")] if name.endswith("_b.metal") else name[:-6]
    definitions = definitions_of(source)
    span = c1_span(os.path.join(version_dir, key + "_b.json"))
    return None if all(proven(index, definitions, span) for index in accesses) else key


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} <dmsh_b version dir> <output tsv>")
    version_dir, output = map(os.path.abspath, sys.argv[1:])
    jobs = [(version_dir, name) for name in sorted(os.listdir(version_dir)) if name.endswith(".metal")]
    with multiprocessing.Pool() as pool:   # map keeps the sorted order, so the list is the same as one core's
        keys = [key for key in pool.map(unproven, jobs, chunksize=64) if key]
    os.makedirs(os.path.dirname(output), exist_ok=True)
    with open(output, "w", encoding="utf-8", newline="") as stream:
        stamp = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
        stream.write(f"{dmsh_chain.table_head(version_dir)} {stamp}\n")   # the library's chain version and the profile inputs it was built from
        for key in keys:
            stream.write(key + "\n")
    print(f"not proven bounded: {len(keys)} shaders")


if __name__ == "__main__":
    main()

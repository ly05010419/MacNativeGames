#!/usr/bin/env python3
"""Dump PE facts and the export symbol map of a game EXE or DLL.

Exports become lifter entry points with demangled names. Some game EXEs
export thousands of MSVC-mangled C++ symbols (script bindings that go
through the EXE's own export table).
This script turns them into a TSV symbol map for the lifter.

Pure Python PE parsing (no pefile dependency); demangling uses llvm-undname.

Usage:
  python3 -I tools/pe_symbols.py <game.exe> [--out symbols]

Outputs (in --out, gitignored, regenerable):
  pe_info.txt      sections, entry point, TLS, imports, rich header
  exports.tsv      va<TAB>kind<TAB>mangled<TAB>demangled
  imports.tsv      iat_va<TAB>dll<TAB>name_or_ordinal
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys


def u16(d, o): return struct.unpack_from("<H", d, o)[0]
def u32(d, o): return struct.unpack_from("<I", d, o)[0]


class PE:
    def __init__(self, data):
        self.d = data
        e_lfanew = u32(data, 0x3C)
        assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0", "not a PE"
        fh = e_lfanew + 4
        self.machine = u16(data, fh)
        self.nsec = u16(data, fh + 2)
        self.timestamp = u32(data, fh + 4)
        opt_size = u16(data, fh + 16)
        oh = fh + 20
        assert u16(data, oh) == 0x10B, "expected PE32"
        self.entry_rva = u32(data, oh + 16)
        self.image_base = u32(data, oh + 28)
        self.image_size = u32(data, oh + 56)
        self.headers_size = u32(data, oh + 60)
        ndirs = u32(data, oh + 92)
        self.dirs = [(u32(data, oh + 96 + i * 8), u32(data, oh + 100 + i * 8))
                     for i in range(ndirs)]
        self.sections = []
        sh = oh + opt_size
        for i in range(self.nsec):
            o = sh + i * 40
            name = data[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, va, rsize, rptr = struct.unpack_from("<IIII", data, o + 8)
            chars = u32(data, o + 36)
            self.sections.append((name, va, vsize, rptr, rsize, chars))

    def off(self, rva):
        for _, va, vsize, rptr, rsize, _ in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return rptr + (rva - va)
        if rva < self.headers_size:
            return rva
        raise ValueError(f"rva {rva:#x} not in file")

    def cstr(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.index(b"\0", o)].decode("latin1")

    def section_of(self, rva):
        for name, va, vsize, *_ in self.sections:
            if va <= rva < va + vsize:
                return name
        return "?"

    def exports(self):
        rva, size = self.dirs[0]
        if not size:
            return []
        o = self.off(rva)
        base = u32(self.d, o + 16)
        nfuncs, nnames = u32(self.d, o + 20), u32(self.d, o + 24)
        funcs, names, ords = (self.off(u32(self.d, o + 28 + i * 4))
                              for i in range(3))
        by_index = {}
        for i in range(nnames):
            name = self.cstr(u32(self.d, names + i * 4))
            by_index[u16(self.d, ords + i * 2)] = name
        out = []
        for i in range(nfuncs):
            frva = u32(self.d, funcs + i * 4)
            if frva:
                out.append((frva, base + i, by_index.get(i)))
        return out

    def imports(self):
        rva, size = self.dirs[1]
        out = []
        o = self.off(rva)
        while True:
            ilt, _, _, name_rva, iat = struct.unpack_from("<IIIII", self.d, o)
            if not name_rva:
                break
            dll = self.cstr(name_rva)
            t = self.off(ilt or iat)
            slot = iat
            while True:
                v = u32(self.d, t)
                if not v:
                    break
                sym = f"#{v & 0xFFFF}" if v & 0x80000000 else self.cstr(v + 2)
                out.append((self.image_base + slot, dll, sym))
                t += 4
                slot += 4
            o += 20
        return out

    def rich(self):
        i = self.d.find(b"Rich", 0, 0x400)
        if i < 0:
            return []
        key = u32(self.d, i + 4)
        j = self.d.find(struct.pack("<I", 0x536E6144 ^ key), 0, i)
        out = []
        for o in range(j + 16, i, 8):
            a, b = u32(self.d, o) ^ key, u32(self.d, o + 4) ^ key
            out.append((a >> 16, a & 0xFFFF, b))
        return out


def demangle(names):
    tool = shutil.which("llvm-undname") or "/usr/lib/llvm/22/bin/llvm-undname"
    if not os.path.exists(tool):
        return {n: "" for n in names}
    res = subprocess.run([tool], input="\n".join(names) + "\n",
                         capture_output=True, text=True)
    # llvm-undname echoes each input line followed by its demangled form
    # (or an error line), then a blank line.
    out, lines, i = {}, res.stdout.splitlines(), 0
    for n in names:
        while i < len(lines) and lines[i] != n:
            i += 1
        dem = lines[i + 1] if i + 1 < len(lines) else ""
        out[n] = "" if dem.startswith("error") else dem
        i += 2
    return out


def classify(name, section):
    if name.startswith("??_C@"):
        return "string"
    if name.startswith("??_7"):
        return "vftable"
    if section != ".text":
        return "data"
    return "func"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--out", default="symbols")
    args = ap.parse_args()
    pe = PE(open(args.exe, "rb").read())
    os.makedirs(args.out, exist_ok=True)

    exps = pe.exports()
    names = [n for _, _, n in exps if n]
    dem = demangle(names)
    kinds = {}
    with open(os.path.join(args.out, "exports.tsv"), "w") as f:
        f.write("va\tkind\tmangled\tdemangled\n")
        for rva, ordinal, name in sorted(exps):
            name = name or f"ord_{ordinal}"
            kind = classify(name, pe.section_of(rva))
            kinds[kind] = kinds.get(kind, 0) + 1
            f.write(f"{pe.image_base + rva:08X}\t{kind}\t{name}\t"
                    f"{dem.get(name, '')}\n")

    imps = pe.imports()
    with open(os.path.join(args.out, "imports.tsv"), "w") as f:
        f.write("iat_va\tdll\tsymbol\n")
        for va, dll, sym in imps:
            f.write(f"{va:08X}\t{dll}\t{sym}\n")

    with open(os.path.join(args.out, "pe_info.txt"), "w") as f:
        p = lambda *a: print(*a, file=f)
        p(f"image_base {pe.image_base:#010x}  image_size {pe.image_size:#x}"
          f"  entry {pe.image_base + pe.entry_rva:#010x}"
          f"  timestamp {pe.timestamp:#010x}")
        p(f"relocations: {'yes' if pe.dirs[5][1] else 'NONE (fixed base)'}")
        tls_rva, tls_size = pe.dirs[9]
        if tls_size:
            o = pe.off(tls_rva)
            s, e, idx, cb = struct.unpack_from("<IIII", pe.d, o)
            p(f"tls: raw {s:#010x}-{e:#010x} index_va {idx:#010x}"
              f" callbacks_va {cb:#010x}")
        p("sections:")
        for name, va, vsize, rptr, rsize, ch in pe.sections:
            p(f"  {name:8s} va {pe.image_base + va:#010x} vsize {vsize:#9x}"
              f" raw {rsize:#9x} chars {ch:#010x}")
        p("imports per dll:")
        per = {}
        for _, dll, _ in imps:
            per[dll] = per.get(dll, 0) + 1
        for dll, n in per.items():
            p(f"  {dll:14s} {n}")
        p(f"exports: {len(exps)} " + " ".join(
            f"{k}={v}" for k, v in sorted(kinds.items())))
        p("rich header (prodid build count):")
        for prod, build, count in pe.rich():
            p(f"  {prod:3d} {build:5d} {count}")

    sys.stdout.write(open(os.path.join(args.out, "pe_info.txt")).read())


if __name__ == "__main__":
    main()

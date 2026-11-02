#!/usr/bin/env python3
# cb_extent.py <dmsh_b version dir> <output tsv>: how far into each uniform slot (cbuf binding) a shader can read, from the translated MSL. A line lists, per shader key, the slots whose
# reads are all proven to stay inside the first <bytes> bytes: `<key>\t<reg>:<bytes> ...` (reg as in the reflection json, bytes a multiple of 16). A slot left out may be read anywhere in its
# binding. Indices are bounded with c1_guard.py's interval arithmetic; any other use of the binding's name (a pointer handed on, a member other than _m0) leaves the slot out.
# `s<reg>` marks a storage slot the shader never writes (unwritten); the host notes GPU writes for every other storage binding (nvn_mtl.m sbw_note).
# The host (nvn_mtl.m cb_ext) reads only that much of a binding when it has to copy one: a uniform block the game updates in part inside a render pass (ubp_get). Breath of the Wild binds
# a 64 KiB block to the vertex stage and updates its first 64 bytes 221 times a frame, while its vertex shaders read at most 544 bytes of it.
# The host loads the table only when its header names its library's chain version and inputs stamp (like c1_full.tsv); dmsh_build.py makes it after each full build
import datetime
import json
import multiprocessing
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from c1_guard import Interval, definitions_of, known   # noqa: E402
import dmsh_chain   # noqa: E402

MAX = 65536


def extents(source, bindings):
    """{reg: bytes} for the cbuf bindings whose every read is a proven index into _m0"""
    out, definitions = {}, None
    for b in bindings:
        if b.get("kind") != "cbuf" or "reg" not in b: continue
        name = b["msl_name"]
        uses = len(re.findall(r"\b" + re.escape(name) + r"\b", source))
        reads = re.findall(r"\(\*spvDescriptorSet0\." + re.escape(name) + r"\)\._m0\[([^\]]+)\]", source)
        declared = len(re.findall(r"\b" + re.escape(name) + r"\s*\[\[id\(", source))   # the argument buffer member
        if uses != len(reads) + declared: continue
        high = -1
        for index in set(reads):
            if definitions is None: definitions = definitions_of(source)
            low, top = Interval(definitions).expression(index)
            if not known(low, top) or low < 0: high = None; break
            high = max(high, top)
        if high is None or 16 * (high + 1) >= MAX: continue
        out[b["reg"]] = 16 * (high + 1)
    return out


WRITE = re.compile(r"\s*(?:=(?!=)|[-+*/%&|^]=|<<=|>>=|\+\+|--)")


def unwritten(source, bindings):
    """The storage slots (sbuf bindings) the shader never writes: every use of the name is (*<set>.<name>)._m0[...] read as a value. A use followed by an assignment, after
    an address-of (atomics take &...), or of any other form (the pointer handed to a function) counts as a write. SPIRV-Cross types every storage buffer Ryujinx emits as
    writable (device, not const device), so the host takes a binding as written unless the table lists it"""
    out = []
    for b in bindings:
        if b.get("kind") != "sbuf" or "reg" not in b: continue
        name = "spvDescriptorSet0." + b["msl_name"]; ok = True
        for m in re.finditer(r"\(\*" + re.escape(name) + r"\b", source):
            j = m.end()
            if source[m.start() - 1:m.start()] == "&" or not source.startswith(")._m0[", j): ok = False; break
            j += len(")._m0"); depth = 0
            while j < len(source):   # the index and any further [..] or .member
                c = source[j]
                if c == "[": depth += 1
                elif c == "]": depth -= 1
                elif depth == 0 and not (c == "." or c.isalnum() or c == "_"): break
                j += 1
            if WRITE.match(source, j): ok = False; break
        if ok and len(re.findall(re.escape(name) + r"\b", source)) == len(re.findall(r"\(\*" + re.escape(name) + r"\b", source)):
            out.append(b["reg"])
    return sorted(set(out))


def row(path):
    """The table line for one shader's reflection json, or None"""
    try:
        with open(path, encoding="utf-8") as stream: bindings = json.load(stream).get("bindings") or []
        if not any(b.get("kind") in ("cbuf", "sbuf") for b in bindings): return None
        with open(path[:-5] + ".metal", encoding="utf-8") as stream: source = stream.read()
    except (OSError, ValueError):
        return None
    toks = [f"{r}:{n}" for r, n in sorted(extents(source, bindings).items())] + [f"s{r}" for r in unwritten(source, bindings)]
    return os.path.basename(path)[:-len("_b.json")] + "\t" + " ".join(toks) if toks else None


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} <dmsh_b version dir> <output tsv>")
    version_dir, output = map(os.path.abspath, sys.argv[1:])
    paths = [os.path.join(version_dir, n) for n in sorted(os.listdir(version_dir)) if n.endswith("_b.json")]
    with multiprocessing.Pool() as pool: rows = [r for r in pool.map(row, paths, chunksize=64) if r]
    slots, ro = sum(r.count(":") for r in rows), sum(len(re.findall(r"\bs\d+", r.split("\t", 1)[1])) for r in rows)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    with open(output, "w", encoding="utf-8", newline="") as stream:
        stamp = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
        stream.write(f"{dmsh_chain.table_head(version_dir)} {stamp}\n")   # the library's chain version and the profile inputs it was built from
        stream.write("# shader key, then reg:bytes for each uniform slot whose reads stay inside its first bytes, and s<reg> for each storage slot it never writes (cb_extent.py)\n")
        for line in rows: stream.write(line + "\n")
    print(f"bounded uniform reads: {slots} slots; storage slots never written: {ro}; {len(rows)} shaders")


if __name__ == "__main__":
    main()

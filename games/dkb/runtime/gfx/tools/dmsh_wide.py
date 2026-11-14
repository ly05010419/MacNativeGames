"""Storage-buffer reads the translator split into words, read whole again (wide_loads, a step of dmsh_build.py).

Ampere reads a storage buffer 64 or 128 bits at a time (LDG.E.64 / .128); the translator splits each into 32-bit LoadGlobal operations, and spirv-cross writes
every word with an address of its own:

    int _3487 = _3483 + 32;  uint _3499 = uint(int(uint(_3487) >> uint(2)));  float _3501 = as_type<float>((*spvDescriptorSet0.fp_s3)._m0[int(_3499)]);
    int _3503 = _3487 + 4;   uint _3505 = uint(int(uint(_3503) >> uint(2)));  float _3507 = as_type<float>((*spvDescriptorSet0.fp_s3)._m0[int(_3505)]);

Metal's front end cannot tell that such words are neighbours (each index is the shift of its own sum, then widened), so they stay separate loads, each with its
own address arithmetic. Splatoon Raiders' material shader (Hoian_UBER) reads its lights this way, 80 bytes a light in a loop over the lights, about 240 words in
all: on an M1 the scene's lit pass took 70 ms, 16 ms with the words read as below (median GPU time of the pass, the opening dialogue scene).

Here the words of one read-only buffer (const device) at byte offsets k, k+4, ... from one value, with no brace between them (one block of straight-line code),
become one read of a packed_uint2/3/4 at the first of them, and each word reads its lane. A packed vector needs only 4-byte alignment, so it may sit wherever
the words could; the shader does not write the buffer through its own binding, and a run ends at any line that may write memory or change a value (a store
through a subscript or a pointer, an atomic, a barrier, an image write, an assignment to an existing variable), so a write through another binding to the
same memory, or a common value that changes, cannot fall between its words: reading the later words earlier gives the same values. A run starts on a 16-byte
step of the common value and holds at most four words, as the original wide reads did; a run whose lowest word is not read first is left alone.
"""
import re

_IDENT = r'_[0-9A-Za-z_]+'
_BUF = r'[a-z]p_s\d+'   # storage buffer bindings: vp_ / fp_ / cp_ / gp_ ... s<n>
_ADD = re.compile(r'^\s*int (' + _IDENT + r') = (' + _IDENT + r') \+ (-?\d+);\s*$')
_IDX = re.compile(r'^\s*uint (' + _IDENT + r') = uint\(int\(uint\((' + _IDENT + r')\) >> uint\(2\)\)\);\s*$')
_LOAD = re.compile(r'\(\*spvDescriptorSet0\.(' + _BUF + r')\)\._m0\[int\((' + _IDENT + r')\)\]')
_RO = re.compile(r'^\s*const device ' + _IDENT + r'\* (' + _BUF + r') \[\[id\(\d+\)\]\];', re.M)
_OP = r'(?:[-+*/%|&^]|<<|>>)?=(?!=)'
_WRITE = re.compile(r'atomic_|_barrier\(|\.write\(|\]\s*' + _OP + r'|^\s*\*|^\s*' + _IDENT + r'\s*' + _OP)   # lines a run does not reach across
_VEC = {2: 'packed_uint2', 3: 'packed_uint3', 4: 'packed_uint4'}


def _root(v, adds):   # (value, byte offset) the sums lead back to
    off = 0
    for _ in range(64):
        if v not in adds: break
        v, k = adds[v]; off += k
    return v, off


def wide_loads(msl):
    if ' >> uint(2)' not in msl: return msl
    ro = set(_RO.findall(msl))
    if not ro: return msl
    lines = msl.split('\n')
    adds, idx = {}, {}
    for ln in lines:
        m = _ADD.match(ln)
        if m: adds[m.group(1)] = (m.group(2), int(m.group(3))); continue
        m = _IDX.match(ln)
        if m: idx[m.group(1)] = m.group(2)
    words, block = {}, 0   # (block, buffer, value) → {byte offset: (line, match)}
    for i, ln in enumerate(lines):
        if '{' in ln or '}' in ln or _WRITE.search(ln): block += 1; continue
        ms = list(_LOAD.finditer(ln))
        if len(ms) != 1 or ms[0].group(1) not in ro or ms[0].group(2) not in idx: continue
        v, off = _root(idx[ms[0].group(2)], adds)
        if off % 4 == 0: words.setdefault((block, ms[0].group(1), v), {}).setdefault(off, (i, ms[0]))
    edits, n = {}, 0
    for (_, buf, _), by_off in words.items():
        for base in sorted({off - off % 16 for off in by_off}):
            run = []
            while len(run) < 4 and base + 4 * len(run) in by_off: run.append(by_off[base + 4 * len(run)])
            if len(run) < 2 or any(i < run[0][0] for i, _ in run): continue
            name, vec = f'_dmw{n}', _VEC[len(run)]; n += 1
            decl = f'{vec} {name} = *(const device {vec}*)&(*spvDescriptorSet0.{buf})._m0[int({run[0][1].group(2)})];'
            for w, (i, m) in enumerate(run): edits.setdefault(i, []).append((m.start(), m.end(), f'{name}.{"xyzw"[w]}', decl if w == 0 else None))
    for i, es in edits.items():
        ln, pre = lines[i], []
        for s, e, rep, decl in sorted(es, reverse=True):
            ln = ln[:s] + rep + ln[e:]
            if decl: pre.append(decl)
        indent = lines[i][:len(lines[i]) - len(lines[i].lstrip())]
        lines[i] = ''.join(indent + d + '\n' for d in pre) + ln
    return '\n'.join(lines)

"""Offline geometry helpers. No game IDs or shader keys participate in this ABI.

VS capture records are 144 words: position 0, point size 4, clip distances
5..12, layer 13, viewport 14, reserved 15, generic attributes 16..143.
This matches ResourceReservations.FillGeometryInputMap. Capture uses the native
vertex fetch, including its instance divisors and format conversions. The host
gives the capture a slot per word (-1: not stored) and a record stride: the full
map in order, or only the words a lean kernel reads (lean).
"""
import re
import struct

WORDS = 144
CAPTURE_SLOT = 13
HELPERS = {'vertex_output': 0, 'geometry_vb_output': 1,
           'geometry_ib_output': 2, 'vb_info': 3, 'trb_data': 4,
           'vb_input': 1}


def output_layout(code, input_topology):
    """Geometry output bounds in the translator's fixed-input compute ABI."""
    if len(code) < 20 or input_topology not in range(5):
        raise ValueError('invalid geometry header or input topology')
    w2, w3, w4 = struct.unpack_from('<3I', code, 8)
    vertices, invocations, topology = w4 & 4095, w2 >> 24, w3 >> 24 & 15
    # ShaderDefinitions overrides a passthrough GS's raw SPH bound with the
    # non-adjacent input vertex count. Match that override for every topology;
    # raw bounds can be 1 even when a triangle kernel writes three vertices and
    # six index words per invocation (including restart padding).
    if struct.unpack_from('<I', code)[0] & (1 << 24):
        vertices = (1, 2, 2, 3, 3)[input_topology]
        topology = (1, 6, 6, 7, 7)[input_topology]
    if topology not in (1, 6, 7) or not vertices or not invocations:
        raise ValueError('invalid geometry output declaration')
    return {'vertices': vertices, 'invocations': invocations, 'output': topology,
            'index_stride': vertices * 2}


def reader(src):
    """Preserve viewport-zero culling in the generated raster vertex reader.

    ResourceReservations stores ViewportMask as one word, but the passthrough
    generator treats it as scalar while IoMap declares int[1]. SPIRV-Cross then
    emits this invalid array copy for builtin 5253 (ViewportMaskNV). Metal has
    no viewport-mask output. Replay binds viewport zero only, so its bit is the
    visibility decision: a zero moves the vertex beyond the X clip plane, which
    still culls when depth clamping is enabled. The mask must agree across a
    primitive, as for portable NV viewport-mask output; broadcasting to other
    viewports is not implemented.
    Unknown expressions are rejected rather than losing their culling effect.
    """
    builtin = 'gl_BuiltIn_5253'
    if builtin not in src:
        return src
    store = re.compile(r'\bspvArrayCopyFromStackToStack\(\s*' + builtin
                       + r'\s*,\s*as_type<int>\((\w+)\)\.elements\s*\);')
    lowered, n = store.subn(r'dm_gs_viewport_mask = as_type<uint>(\1);', src)
    if not n or builtin in lowered or 'dm_gs_viewport_mask' in src:
        raise ValueError('geometry reader: unsupported viewport-mask expression')
    output = re.search(r'struct main0_out\s*\{(.*?)\};', lowered, re.S)
    position = re.search(r'\bfloat4 (\w+) \[\[position(?:, invariant)?\]\];', output[1]) if output else None
    entry = re.search(r'\bvertex\s+main0_out\s+main0\s*\([^{}]*\)\s*\{', lowered)
    if not position or not entry:
        raise ValueError('geometry reader: missing vertex position or entry')
    begin, depth, end = entry.end(), 1, entry.end()
    while depth and end < len(lowered):
        depth += (lowered[end] == '{') - (lowered[end] == '}')
        end += 1
    if depth:
        raise ValueError('geometry reader: unterminated vertex entry')
    body = lowered[begin:end - 1]
    if body.count('dm_gs_viewport_mask') != n or 'return out;' not in body:
        raise ValueError('geometry reader: viewport mask outside the vertex entry')
    body = body.replace('return out;',
                        '{ if ((dm_gs_viewport_mask & 1u) == 0u) '
                        f'out.{position[1]} = float4(2.0, 0.0, 0.0, 1.0); return out; }}')
    return lowered[:begin] + '\n    uint dm_gs_viewport_mask = 1u;' + body + lowered[end - 1:]


def capture(src):
    """Append a rasterizer-discard vertex entry to the unmodified-depth source."""
    m = re.search(r'\bvertex main0_out main0\((.*?)\)\s*\{', src, re.S)
    if not m:
        raise ValueError('geometry capture: missing main0 vertex entry')
    params = m.group(1)
    body = src[m.end():]
    if not body.rstrip().endswith('}'):
        raise ValueError('geometry capture: unexpected function ending')
    pro = ''
    for semantic, name in [('vertex_id', 'dm_vid'), ('instance_id', 'dm_iid')]:
        found = re.search(r'uint (\w+) \[\[' + semantic + r'\]\]', params)
        if found:
            pro += f'    uint {name} = {found.group(1)};\n'
        else:
            params += (', ' if params else '') + f'uint {name} [[{semantic}]]'
    stores = [(k, f'as_type<uint>({e})') for k, e in output_words(src).items()]
    params += f', constant dm_geometry_capture& dm_gs [[buffer({CAPTURE_SLOT})]]'
    pro += ('    uint4 dm_ci = dm_gs.info; device uint* dm_capture = dm_gs.dst;\n'
            '    if (dm_vid - dm_ci.x >= dm_ci.y || dm_iid - dm_ci.z >= dm_ci.w) return;\n'
            '    device uint* dm_p = dm_capture + ((ulong(dm_iid - dm_ci.z) * dm_ci.y + dm_vid - dm_ci.x) * dm_gs.stride);\n')
    # Missing attributes hold what the normal vertex builder gives them: (0, 0, 0, 1), point size 1.
    # Each word is written once, through its slot.
    words = {k: '0x3f800000u' if k in (3, 4) or (k >= 19 and k % 4 == 3) else '0u' for k in range(WORDS)}
    words.update(stores)
    epilogue = '{ ' + ' '.join(f'dm_cw(dm_gs, dm_p, {k}, {v});' for k, v in words.items()) + ' return; }'
    if 'return out;' not in body:
        raise ValueError('geometry capture: missing return')
    return (f'\nstruct dm_geometry_capture {{ device uint* dst; uint4 info; uint stride; short slot[{WORDS}]; }};\n'
            'static inline void dm_cw(constant dm_geometry_capture& g, device uint* p, uint k, uint v) { short s = g.slot[k]; if (s >= 0) p[s] = v; }\n'
            'vertex void main0_gs(' + params + ')\n{\n' + pro + body.replace('return out;', epilogue))


def output_words(src):
    """The capture words a vertex shader writes: {word: expression over out} (the 144-word map above)."""
    st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    if not st:
        raise ValueError('geometry capture: missing output struct')
    stores = []
    for line in st.group(1).splitlines():
        line = line.strip()
        if not line:
            continue
        v = re.fullmatch(r'(float4|uint4|int4) (\w+) \[\[user\(locn(\d+)\)\]\];', line)
        if v:
            if int(v[3]) >= 32:
                raise ValueError('geometry capture: attribute outside Maxwell map')
            stores += [(16 + int(v[3]) * 4 + c, f'out.{v[2]}.{s}') for c, s in enumerate('xyzw')]
            continue
        v = re.fullmatch(r'float4 (\w+) \[\[position(?:, invariant)?\]\];', line)
        if v:
            stores += [(c, f'out.{v[1]}.{s}') for c, s in enumerate('xyzw')]
            continue
        v = re.fullmatch(r'(float|uint|int) (\w+) \[\[(point_size|render_target_array_index|viewport_array_index)\]\];', line)
        if v:
            offset = {'point_size': 4, 'render_target_array_index': 13, 'viewport_array_index': 14}[v[3]]
            stores.append((offset, f'out.{v[2]}'))
            continue
        # spirv-cross writes the array size after the attribute (float gl_ClipDistance [[clip_distance]] [8]; Luigi's Mansion 3's vertex shaders)
        v = re.fullmatch(r'float (\w+)(?:\[(\d+)\])? \[\[clip_distance\]\](?: \[(\d+)\])?;', line)
        if v and (v[2] or v[3]) and int(v[2] or v[3]) <= 8:
            stores += [(5 + c, f'out.{v[1]}[{c}]') for c in range(int(v[2] or v[3]))]
            continue
        v = re.fullmatch(r'float (\w+) \[\[user\(clip(\d)\)\]\];', line)   # the same distances again as varyings for the pixel stage (gl_ClipDistance_0 [[user(clip0)]])
        if v:
            stores.append((5 + int(v[2]), f'out.{v[1]}'))
            continue
        raise ValueError('geometry capture: unknown output ' + line)
    return dict(stores)


def flatten_sets(path):
    """Give auxiliary sets their own binding ranges in the ordinary set-0 ABI."""
    with open(path, 'rb') as f:
        data = f.read()
    w = list(struct.unpack('<%dI' % (len(data) // 4), data))
    sets, bindings = {}, {}
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 65535
        if not n or i + n > len(w):
            raise ValueError('invalid SPIR-V instruction')
        if op == 71 and n == 4:
            if w[i + 2] == 34: sets[w[i + 1]] = (w[i + 3], i + 3)
            if w[i + 2] == 33: bindings[w[i + 1]] = i + 3
        i += n
    for ident, (s, at) in sets.items():
        if s:
            if s not in (1, 2) or ident not in bindings:
                raise ValueError('unknown geometry auxiliary descriptor set')
            w[bindings[ident]] += 10000 if s == 1 else 11000
            w[at] = 0
    with open(path, 'wb') as f:
        f.write(struct.pack('<%dI' % len(w), *w))


def topology_pointer(src):
    """Topology remap uses uint words; avoid allocating a texture view per draw."""
    src, n = re.subn(r'texture_buffer<float> trb_data', 'device const uint* trb_data', src)
    if n != 1:
        raise ValueError('missing geometry topology buffer')
    # Match the balanced read argument rather than relying on expression shape.
    prefix = 'spvDescriptorSet0.trb_data.read('
    while prefix in src:
        start = src.index(prefix)
        at, depth = start + len(prefix), 1
        end = at
        while depth:
            if src[end] == '(': depth += 1
            if src[end] == ')': depth -= 1
            end += 1
        if src[end:end + 2] != '.x':
            raise ValueError('unexpected topology read')
        src = src[:start] + 'as_type<float>(spvDescriptorSet0.trb_data[' + src[at:end - 1] + '])' + src[end + 2:]
    if 'trb_data.read' in src:
        raise ValueError('unconverted geometry topology read')
    return src


def capture_input(src):
    """Keep the full Maxwell capture map readable by optimized Metal kernels.

    Copying all 128 contiguous generic components can make Metal's optimized
    AIR fail AGX pipeline compilation. Volatile capture reads retain each scalar
    load without changing the captured values or disabling kernel optimization.
    Ordinary geometry kernels which do not read captured input stay unchanged.
    """
    if 'vertex_output' not in src:
        return src
    lowered, n = re.subn(r'\bdevice (\w+)\* vertex_output\b',
                         r'device volatile \1* vertex_output', src)
    if n != 1:
        raise ValueError('unknown geometry capture-input declaration')
    return lowered


def passthrough(code):
    """SPH word 0 bit 24 (GpPassthrough): NV passthrough geometry. Per-vertex attributes
    reach the rasterizer unchanged; the shader only decides per primitive (viewport mask)
    and may write storage."""
    return len(code) >= 4 and bool(struct.unpack_from('<I', code)[0] >> 24 & 1)


MASK_WORD = 5   # the viewport mask's word in an output vertex (reader: dm_gs_viewport_mask)
_DEF = re.compile(r'^\s*(?:int|uint|float|bool) (_\d+) = (.+);$')
_PURE = re.compile(r'atomic|Subgroup|simd_|\+\+|--|[^=!<>]=[^=]')


def lean(src, out_stride):
    """A passthrough kernel without its copy of the vertex: only the viewport mask is
    written, one word per output vertex, and only the capture words it still reads are
    loaded, from a record of len(reads) words in that order. Returns (source, reads).

    The rasterizer then draws the game's own vertex shader through the kernel's
    indices (nvn_geometry.inc). Input reads and output stores must be the generated
    literal offsets; anything else raises ValueError and the full kernel is kept.
    """
    lines = src.split('\n')
    defs = {}
    for i, ln in enumerate(lines):
        m = _DEF.match(ln)
        if m: defs.setdefault(m[1], (i, m[2]))
    def base(var, stride):   # var = _X * stride -> _X
        m = re.fullmatch(r'(_\d+) \* ' + str(stride), defs.get(var, (0, ''))[1])
        return m[1] if m else None
    def offset(var, stride):   # var = _B + K with _B = _X * stride (or var itself) -> (_B, K)
        e = defs.get(var, (0, ''))[1]
        m = re.fullmatch(r'(_\d+) \+ (\d+)', e)
        if m and base(m[1], stride): return m[1], int(m[2])
        if base(var, stride): return var, 0
        return None, None
    store = re.compile(r'^\s*\(\*spvDescriptorSet0\.geometry_vb_output\)\._m0\[(_\d+)\] = (_\d+);$')
    kept = 0
    for i, ln in enumerate(lines):
        if 'geometry_vb_output' not in ln or ln.strip().startswith('device '): continue
        m = store.match(ln)
        if not m: raise ValueError('lean geometry: unknown output store')
        b, k = offset(m[1], out_stride)
        if b is None: raise ValueError('lean geometry: unknown output store')
        if k != MASK_WORD: lines[i] = ''; continue
        j, _ = defs[m[1]]
        lines[j] = re.sub(r'= .+;$', f'= {base(b, out_stride)};', lines[j]); kept += 1
    if not kept: raise ValueError('lean geometry: no viewport mask store')
    # Remove what only fed the removed stores: unused pure definitions, and local array
    # elements written at a literal index and never read.
    decl = re.compile(r'^\s*spvUnsafeArray<\w+, \d+> (_\d+);$')
    arrays = {m[1] for m in map(decl.match, lines) if m}
    for a in list(arrays):
        if re.search(r'\b' + a + r'\b(?!\[\d+\])', '\n'.join(l for l in lines if not decl.match(l))):
            arrays.discard(a)   # used whole or at a computed index: keep every element
    token = re.compile(r'_\d+(?:\[\d+\])?')
    target: list[str | None] = [None] * len(lines)   # what a removable line defines, else None
    rvalues = [()] * len(lines)
    uses, writers = {}, {}
    body = next((i for i, ln in enumerate(lines) if re.match(r'^kernel void main0\(', ln)), None)
    if body is None: raise ValueError('lean geometry: no kernel entry')
    for i, ln in enumerate(lines):
        if i <= body: continue   # type names (struct _22) are not uses of a variable _22
        m = _DEF.match(ln)
        a = None if m else re.match(r'^\s*(_\d+)\[(\d+)\] = (.+);$', ln)
        if m and not _PURE.search(m[2]): target[i], expr = m[1], m[2]
        elif a and a[1] in arrays and not _PURE.search(a[3]): target[i], expr = f'{a[1]}[{a[2]}]', a[3]
        else: expr = ln
        rvalues[i] = tuple(t for t in token.findall(expr) if not (t.split('[')[0] in arrays and '[' not in t))
        for t in rvalues[i]: uses[t] = uses.get(t, 0) + 1
        if target[i]: writers.setdefault(target[i], []).append(i)
    work = [i for i, t in enumerate(target) if t and not uses.get(t)]
    while work:
        i = work.pop()
        if not lines[i]: continue
        lines[i] = ''
        for t in rvalues[i]:
            uses[t] -= 1
            if not uses[t]: work += writers.get(t, [])
    # The remaining capture reads: literal words of the 144-word record, renumbered.
    text = '\n'.join(lines)
    if text.count('vertex_output') != len(re.findall(r'= \(\*spvDescriptorSet0\.vertex_output\)\._m0\[_\d+\];', text)) + 1:
        raise ValueError('lean geometry: unknown capture read')
    found = []
    for n in re.findall(r'= \(\*spvDescriptorSet0\.vertex_output\)\._m0\[(_\d+)\];', text):
        b, k = offset(n, WORDS)
        if b is None: raise ValueError('lean geometry: computed capture read')
        found.append((n, b, k))
    reads = sorted({k for _, _, k in found})
    slot = {k: i for i, k in enumerate(reads)}
    bases = {b for _, b, _ in found}
    for b in bases:   # each record base must serve only these reads
        if len(re.findall(r'\b' + b + r'\b', text)) - 1 > sum(1 for n, bb, _ in found if bb == b):
            raise ValueError('lean geometry: record base used elsewhere')
    for n, b, k in found:
        if n != b:
            j, _ = defs[n]; lines[j] = re.sub(r'\+ \d+;$', f'+ {slot[k]};', lines[j])
    for b in bases:
        j, _ = defs[b]; lines[j] = re.sub(r'\* 144;$', f'* {max(len(reads), 1)};', lines[j])
    return '\n'.join(l for l in lines if l != '' or True), reads

"""Offline geometry helpers. No game IDs or shader keys participate in this ABI.

VS capture records are 144 words: position 0, point size 4, clip distances
5..12, layer 13, viewport 14, reserved 15, generic attributes 16..143.
This matches ResourceReservations.FillGeometryInputMap. Capture uses the native
vertex fetch, including its instance divisors and format conversions.
"""
import re
import struct

WORDS = 144
CAPTURE_SLOT = 13
HELPERS = {'vertex_output': 0, 'geometry_vb_output': 1,
           'geometry_ib_output': 2, 'vb_info': 3, 'trb_data': 4,
           'vb_input': 1}


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
            stores += [f'dm_p[{16 + int(v[3]) * 4 + c}] = as_type<uint>(out.{v[2]}.{s});' for c, s in enumerate('xyzw')]
            continue
        v = re.fullmatch(r'float4 (\w+) \[\[position(?:, invariant)?\]\];', line)
        if v:
            stores += [f'dm_p[{c}] = as_type<uint>(out.{v[1]}.{s});' for c, s in enumerate('xyzw')]
            continue
        v = re.fullmatch(r'(float|uint|int) (\w+) \[\[(point_size|render_target_array_index|viewport_array_index)\]\];', line)
        if v:
            offset = {'point_size': 4, 'render_target_array_index': 13, 'viewport_array_index': 14}[v[3]]
            stores.append(f'dm_p[{offset}] = as_type<uint>(out.{v[2]});')
            continue
        # spirv-cross writes the array size after the attribute (float gl_ClipDistance [[clip_distance]] [8]; Luigi's Mansion 3's vertex shaders)
        v = re.fullmatch(r'float (\w+)(?:\[(\d+)\])? \[\[clip_distance\]\](?: \[(\d+)\])?;', line)
        if v and (v[2] or v[3]) and int(v[2] or v[3]) <= 8:
            stores += [f'dm_p[{5 + c}] = as_type<uint>(out.{v[1]}[{c}]);' for c in range(int(v[2] or v[3]))]
            continue
        v = re.fullmatch(r'float (\w+) \[\[user\(clip(\d)\)\]\];', line)   # the same distances again as varyings for the pixel stage (gl_ClipDistance_0 [[user(clip0)]])
        if v:
            stores.append(f'dm_p[{5 + int(v[2])}] = as_type<uint>(out.{v[1]});')
            continue
        raise ValueError('geometry capture: unknown output ' + line)
    params += f', constant dm_geometry_capture& dm_gs [[buffer({CAPTURE_SLOT})]]'
    pro += (f'    uint4 dm_ci = dm_gs.info; device uint* dm_capture = dm_gs.dst;\n'
            f'    if (dm_vid - dm_ci.x >= dm_ci.y || dm_iid - dm_ci.z >= dm_ci.w) return;\n'
            f'    device uint* dm_p = dm_capture + ((ulong(dm_iid - dm_ci.z) * dm_ci.y + dm_vid - dm_ci.x) * {WORDS});\n')
    # Initialize missing attributes exactly as the normal vertex builder does.
    epilogue = ('{ for (uint dm_k = 0; dm_k < 144; ++dm_k) dm_p[dm_k] = 0; '
                'dm_p[3] = dm_p[4] = 0x3f800000u; '
                'for (uint dm_k = 19; dm_k < 144; dm_k += 4) dm_p[dm_k] = 0x3f800000u; '
                + ' '.join(stores) + ' return; }')
    if 'return out;' not in body:
        raise ValueError('geometry capture: missing return')
    return '\nstruct dm_geometry_capture { device uint* dst; uint4 info; };\nvertex void main0_gs(' + params + ')\n{\n' + pro + body.replace('return out;', epilogue)


def flatten_sets(path):
    """Give auxiliary sets their own binding ranges in the ordinary set-0 ABI."""
    data = open(path, 'rb').read()
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
    open(path, 'wb').write(struct.pack('<%dI' % len(w), *w))


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

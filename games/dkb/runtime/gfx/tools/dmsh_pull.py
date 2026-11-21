# Pulling vertex inputs (<key>_bp): the vertex shader with no [[stage_in]], reading its inputs itself from the game's vertex streams through a per-draw layout block
# (dmsh_pull.metal, at [[buffer(16)]]). A pipeline made with it has no vertex descriptor, so it serves every vertex layout the game binds with that pair of shaders: the host draws
# with it while the pipeline for the draw's own layout is being built (nvn_mtl.m, hybrid pipelines), and so never waits for the Metal compiler on a draw. dm_fetch decodes the
# NVN attribute format at run time into the 32-bit register words Maxwell's fetch gives the shader, bit-identical to Metal's vertex-descriptor fetch (t_pull.m), and the input
# takes them with as_type, so the body runs unchanged. Entry points main0 / main0_z (the depth modes); the others (transform feedback, geometry capture, layer) are left out, and
# their pipelines keep the vertex descriptor. Built offline into a library of its own beside <key>_b, with the same argument buffer and the same compile flags (-fpreserve-invariance:
# the position is [[invariant]] there, so both pipelines of a draw compute it alike). Usage: variant(msl) → (new MSL, {'entry', 'entry_z', 'slot', 'inputs'}) or (None, reason)
import os, re

SLOT = 16   # the layout block's buffer index: the vertex buffers' first slot (nvn_mtl.m VB_SLOT), which a pipeline with no vertex descriptor does not use
FETCH = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dmsh_pull.metal')).read()

def _drop_entries(msl, keep):
    """msl without its top-level vertex functions other than those named in keep (a function runs from a line 'vertex ...' to the next line '}')"""
    out, lines, i = [], msl.split('\n'), 0
    while i < len(lines):
        m = re.match(r'vertex \S+ (\w+)\(', lines[i])
        if m and m.group(1) not in keep:
            j = i
            while j < len(lines) and lines[j] != '}': j += 1
            i = j + 1; continue
        out.append(lines[i]); i += 1
    return '\n'.join(out)

def variant(msl):
    m = re.search(r'\nstruct main0_in\n\{\n(.*?)\n\};', msl, re.S)
    if not m: return None, 'no stage input'   # reads no vertex attribute: its pipelines have no vertex descriptor to vary
    if f'[[buffer({SLOT})]]' in msl: return None, f'buffer {SLOT} is taken'
    members = []
    for ln in m.group(1).split('\n'):
        mm = re.fullmatch(r'    (float4|uint4|int4) (m_\d+(?:_\d+)*) \[\[attribute\((\d+)\)\]\];', ln)   # m_28_0: an input array split per element
        if not mm: return None, 'an input declaration not understood: ' + ln.strip()[:60]
        members.append((mm.group(1), mm.group(2), int(mm.group(3))))
    if any(l >= 16 for _, _, l in members): return None, 'an input at attribute 16 or above'
    head = msl[:m.start()] + '\nstruct main0_in\n{\n' + '\n'.join(f'    {t} {n};' for t, n, _ in members) + '\n};'
    body = _drop_entries(msl[m.end():], ('main0', 'main0_z'))
    entries = {}
    for entry in ('main0', 'main0_z'):
        hdr = re.search(r'\nvertex main0_out %s\(main0_in in \[\[stage_in\]\]((?:, [^\n]*)?)\)\n\{\n' % entry, body)
        if not hdr:
            if entry == 'main0': return None, 'main0 signature not recognized'
            continue
        rest = hdr.group(1)
        vid = re.search(r'uint (\w+) \[\[vertex_id\]\]', rest); iid = re.search(r'uint (\w+) \[\[instance_id\]\]', rest); bi = re.search(r'uint (\w+) \[\[base_instance\]\]', rest)
        params = rest[2:].split(', ') if rest else []
        params.append(f'constant dm_vlayout& dm_lay [[buffer({SLOT})]]')
        if not vid: params.append('uint dm_vid [[vertex_id]]')
        if not iid: params.append('uint dm_iid [[instance_id]]')
        if not bi: params.append('uint dm_bi [[base_instance]]')
        vn, inn, bn = vid.group(1) if vid else 'dm_vid', iid.group(1) if iid else 'dm_iid', bi.group(1) if bi else 'dm_bi'
        pro = '    main0_in in;\n' + ''.join(f'    in.{n} = as_type<{t}>(dm_fetch(dm_lay, {l}u, {vn}, {inn}, {bn}));\n' for t, n, l in members)
        body = body[:hdr.start()] + f'\nvertex main0_out {entry}(' + ', '.join(params) + ')\n{\n' + pro + body[hdr.end():]
        entries[entry] = 1
    ui = head.find('using namespace metal;')
    if ui < 0: return None, 'no "using namespace metal;"'
    ui += len('using namespace metal;')
    head = head[:ui] + '\n\n' + FETCH + head[ui:]
    return head + body, {'entry': 'main0', 'entry_z': 'main0_z' if 'main0_z' in entries else None, 'slot': SLOT, 'inputs': [l for _, _, l in members]}

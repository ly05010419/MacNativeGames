"""Passthrough geometry as one Metal mesh shader (<vertex key>_bm<geometry key>).

A lean passthrough kernel (dmsh_geometry.lean) only decides, per input primitive, whether
the rasterizer gets it. A mesh threadgroup takes PRIMS triangles of a list draw: each thread
runs the vertex shader's pulling entry (dmsh_pull) for one index position and writes the
vertex out, keeps the words the kernel reads in threadgroup memory, and the first PRIMS
threads run the kernel body on them and cull their triangle when viewport 0's mask bit is
clear. No capture pass, no compute pass, no index compaction, no render pass break.

The kernel's own storage writes (and the subgroup operations that order them) run as in the
compute kernel: one triangle per thread, 32 consecutive triangles per SIMD group.
"""
import re
import dmsh_geometry, dmsh_pull

PRIMS = 32
DRAW_SLOT, GS_SLOT = 12, 13   # argument-table slots the vertex stage leaves free in a pulling pipeline
_BUILTIN_V = {'vertex_id': 'dm_vid', 'instance_id': 'dm_iid', 'base_instance': 'dm_bi', 'base_vertex': 'dm_bv'}
_BUILTIN_G = {'thread_position_in_grid': 'uint3(dm_p0 + dm_t, 0u, 0u)', 'thread_index_in_simdgroup': 'dm_sg'}


def _params(s):
    """Split a parameter list at top-level commas."""
    out, depth, cur = [], 0, ''
    for ch in s:
        depth += ch in '<([' ; depth -= ch in '>)]'
        if ch == ',' and not depth: out.append(cur.strip()); cur = ''
        else: cur += ch
    return out + ([cur.strip()] if cur.strip() else [])


def _strip_head(src):
    keep = [l for l in src.split('\n') if not re.match(r'\s*#include\b|\s*using namespace metal;', l)]
    return '\n'.join(keep)


def _entry(src, kind, name):
    m = re.search(r'\n%s (\w+) %s\(([^\n]*)\)\n\{\n' % (kind, name), src)
    if not m: raise ValueError(f'mesh: no {kind} {name}')
    return m


def compose(vs_src, gs_src, reads):
    """vs_src: the vertex shader's library source (<key>_b.metal); gs_src: the lean kernel
    (<geometry key>_bl3.metal) and the capture words it reads, in its record order."""
    if 2 in reads or not reads: raise ValueError('mesh: the kernel reads clip z or nothing')
    pulled, info = dmsh_pull.variant(vs_src)   # keeps main0 / main0_z only
    if pulled is None or not isinstance(info, dict): raise ValueError(f'mesh: no pulling entry: {info}')
    words = dmsh_geometry.output_words(pulled)
    pulled = pulled.replace(' [[point_size]];', ';')   # a triangle mesh has no point size
    # The vertex entries as functions; their resource parameters become the mesh entry's
    entries, res = {}, None
    for name in [info['entry']] + ([info['entry_z']] if info.get('entry_z') else []):
        m = _entry(pulled, 'vertex', name)
        args, fparams, rparams = [], [], []
        for p in _params(m[2]):
            a = re.fullmatch(r'(.+?) (\w+) \[\[(\w+)(?:\((\d+)\))?\]\]', p)
            if not a: raise ValueError('mesh: vertex parameter ' + p)
            fparams.append(f'{a[1]} {a[2]}')
            if a[3] in _BUILTIN_V: args.append(_BUILTIN_V[a[3]])
            elif a[3] == 'buffer' and int(a[4]) not in (DRAW_SLOT, GS_SLOT): args.append(a[2]); rparams.append(p)
            else: raise ValueError('mesh: vertex parameter ' + p)
        if res is not None and res != rparams: raise ValueError('mesh: entries differ in resources')
        res = rparams
        pulled = pulled[:m.start()] + f'\nstatic inline main0_out dm_vs_{name}(' + ', '.join(fparams) + ')\n{\n' + pulled[m.end():]
        entries[name] = args
    # The kernel as a function: capture reads from threadgroup memory, the topology map as the identity
    # (triangle t's vertices are 3t..3t+2), the mask store into dm_mask, index stores dropped
    k = _entry(gs_src, 'kernel', 'main0')
    gargs = []
    for p in _params(k[2]):
        a = re.fullmatch(r'(.+?) (\w+) \[\[(\w+)(?:\((\d+)\))?\]\]', p)
        if a and a[3] in _BUILTIN_G: gargs.append(_BUILTIN_G[a[3]])
        elif a and a[0] == 'constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]]': gargs.append('dm_gset')
        else: raise ValueError('mesh: kernel parameter ' + p)
    fparams = [re.sub(r' \[\[.*\]\]$', '', p) for p in _params(k[2])]
    body = gs_src[k.end():]
    if re.search(r'\bthreadgroup\b|\bthreadgroup_barrier\b', body): raise ValueError('mesh: kernel uses threadgroup memory')
    # GeometryToCompute uses GlobalId.x both for the guest PrimitiveId and for
    # addressing its capture. Keep that ID draw-wide; only the capture address
    # is relative to this mesh group's records. Replacing GlobalId with the
    # local lane silently repeats guest IDs every PRIMS primitives.
    body = re.sub(r'\(\*spvDescriptorSet0\.vertex_output\)\._m0\[(\w+)\]',
                  r'dm_rec[uint(\1) - dm_capture_base]', body)
    prefix = 'spvDescriptorSet0.trb_data['
    while prefix in body:
        s = body.index(prefix); at = e = s + len(prefix); depth = 1
        while depth:
            depth += (body[e] == '[') - (body[e] == ']'); e += 1
        body = body[:s] + 'uint(' + body[at:e - 1] + ')' + body[e:]
    body, n = re.subn(r'\(\*spvDescriptorSet0\.geometry_vb_output\)\._m0\[\w+\] = (\w+);', r'dm_mask = as_type<uint>(\1);', body)
    if not n: raise ValueError('mesh: no mask store')
    body = re.sub(r'\n\s*\(\*spvDescriptorSet0\.geometry_ib_output\)\._m0\[\w+\] = [^;]+;', '', body)
    if re.search(r'vertex_output|geometry_vb_output|geometry_ib_output|trb_data\[', body): raise ValueError('mesh: kernel output not understood')
    gfn = gs_src[:k.start()] + '\nstatic inline void dm_gs(' + ', '.join(fparams + ['threadgroup float* dm_rec', 'uint dm_capture_base', 'thread uint& dm_mask']) + ')\n{\n' + body
    # The words the kernel reads, from the vertex's outputs (missing ones as the capture leaves them: 0, w and point size 1)
    def word(w):
        if w in words: return 'dm_o.' + words[w][4:]
        return '0x3f800000u' if w in (3, 4) or (w >= 19 and w % 4 == 3) else '0u'
    n = len(reads)
    stores = ' '.join(f'dm_rec[dm_t * {n}u + {i}u] = as_type<float>({word(w)});' for i, w in enumerate(reads))
    out = ['#include <metal_stdlib>', '#include <simd/simd.h>', '#include <metal_atomic>', 'using namespace metal;',
           'namespace dm_v {', _strip_head(pulled), '}', 'namespace dm_g {', _strip_head(gfn), '}', 'using namespace dm_v;',
           'struct dm_mesh_draw { device const uchar* ib; uint itype; uint first; uint count; int base_vertex; uint base_instance; };',
           'struct dm_mesh_prim { bool culled [[primitive_culled]]; };']
    if res is None: raise ValueError('mesh: no vertex entries')
    for name, args in entries.items():
        out.append(f'''[[mesh]] void dm_mesh{name[5:]}(metal::mesh<main0_out, dm_mesh_prim, {3 * PRIMS}, {PRIMS}, metal::topology::triangle> dm_m, {', '.join(res)},
    constant dm_mesh_draw& dm_d [[buffer({DRAW_SLOT})]], constant dm_g::spvDescriptorSetBuffer0& dm_gset [[buffer({GS_SLOT})]],
    uint dm_t [[thread_index_in_threadgroup]], uint dm_tg [[threadgroup_position_in_grid]], uint dm_sg [[thread_index_in_simdgroup]])
{{
    threadgroup float dm_rec[{3 * PRIMS * n}];
    uint dm_p0 = dm_tg * {PRIMS}u, dm_np = min({PRIMS}u, dm_d.count / 3u - dm_p0);
    if (dm_t < dm_np * 3u) {{
        uint dm_i = dm_d.first + dm_p0 * 3u + dm_t;
        uint dm_ix = dm_d.itype == 0u ? dm_i : dm_d.itype == 1u ? uint(dm_d.ib[dm_i]) : dm_d.itype == 2u ? uint(((device const ushort*)dm_d.ib)[dm_i]) : ((device const uint*)dm_d.ib)[dm_i];
        uint dm_vid = dm_ix + uint(dm_d.base_vertex), dm_bv = uint(dm_d.base_vertex), dm_bi = dm_d.base_instance, dm_iid = dm_bi;
        main0_out dm_o = dm_vs_{name}({', '.join(args)});
        dm_m.set_vertex(dm_t, dm_o);
        {stores}
    }}
    if (dm_t == 0u) dm_m.set_primitive_count(dm_np);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (dm_t < dm_np) {{
        uint dm_mask = 0u;
        dm_g::dm_gs({', '.join(gargs)}, dm_rec, dm_p0 * {3 * n}u, dm_mask);
        dm_m.set_index(dm_t * 3u, dm_t * 3u); dm_m.set_index(dm_t * 3u + 1u, dm_t * 3u + 1u); dm_m.set_index(dm_t * 3u + 2u, dm_t * 3u + 2u);
        dm_m.set_primitive(dm_t, dm_mesh_prim{{(dm_mask & 1u) == 0u}});
    }}
}}''')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    import json, sys
    vs, gs, out = sys.argv[1:4]
    reads = json.load(open(gs[:-len('.metal')] + '.json'))['geometry']['capture_reads']
    open(out, 'w').write(compose(open(vs).read(), open(gs).read(), reads))

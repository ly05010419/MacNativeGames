# Copyright (c) 2026 seedds. SPDX-License-Identifier: MIT
# Vendored from nxnative PR #983, commit b124b24; license: ../third_party/d2r_gfx/LICENSE.
"""NVN2 task and mesh shaders (the Switch 2's Ampere has them; Mario Kart World draws its grass with them, nvnCommandBufferDrawMeshTasks) as Metal object and mesh functions.

The translator reads them as compute shaders (dmsh_build.py job_for, the job's mesh_layout): ISBERD reads the stage input and the stage output store (0x927) writes the
outputs, both in shared memory at the offsets of LAYOUT (InstEmitAmpereMem.cs AIsberd, AStageOut). This wraps the translated kernel in the Metal stage function:

  task (object function, a workgroup of 32): word 0 holds its task number (the draw's first task plus the threadgroup), the kernel writes its payload at OUT (the number
  of mesh workgroups to launch at payload +4, the records the mesh workgroups read from +0x10), and the payload goes to the mesh stage with that many mesh threadgroups.
  mesh: word 0 holds its number among its task's launches, the payload is copied to ISBE; the kernel writes its vertex attributes at OUT, component by component (0x80
  bytes a component: 32 vertices of a block, the blocks one after another), and its primitives at PRIM (the primitive count byte at +3, three index bytes a triangle
  from +4). The vertices and triangles go to the Metal mesh from there.

The vertex outputs follow the shader's SPH (its OMAP): the position (byte 0x35, bits 4..7 x, y, z, w) and the generic attributes (bytes 0x36 on, a nibble an attribute,
x, y, z, w), in that order in the attribute area; generic attribute n is [[user(locn<n>)]], as the translator names a pixel shader's inputs. Measured on Mario Kart World's
twelve mesh shaders: the highest attribute offset each writes is the last component its OMAP enables (0x280 for position and two components). A mesh shader has two
entry points, as vertex shaders do (dmsh_build.py vs_depth): main0 with NVN's GL depth (near = -w) as Metal's, z' = z / 2 + w / 2, and main0_z with z as it is."""
import re

LAYOUT = (0, 0x40, 0x900)   # byte offsets in the shared array: the workgroup's number, the stage input (payload), the outputs; the primitive bytes follow the outputs


def prim_sph(ctl, code):
    """A mesh shader's per-primitive outputs: a second SPH after its instructions, at the code area offset the control section gives at +0x64 when +0x68 bit 7 is set
    (its OMAP: generic attributes only). Mario Kart World's grass: ddc034e8e009ebfc's (generic 0 .x) is the flat locn0 its pixel shader 3cc3501255bb2d74 reads"""
    if len(ctl) < 0x6c or not ctl[0x68] & 0x80: return None
    o = int.from_bytes(ctl[0x64:0x68], 'little')
    return code[o:o + 0x80] if 0 < o and o + 0x80 <= len(code) else None


def sph_outputs(sph):
    """The SoA components a mesh shader writes, in order: [(None, c)] for the position's, [(n, c)] for generic attribute n's"""
    out = [(None, c) for c in range(4) if sph[0x35] >> (4 + c) & 1]
    for n in range(32):
        nib = sph[0x36 + n // 2] >> (4 * (n % 2)) & 0xf
        out += [(n, c) for c in range(4) if nib >> c & 1]
    return out


def layout(kind, max_v, comps, payload, max_p=0, pcomps=()):
    """(mesh_layout for the translator, shared array words): the outputs hold a task's payload, or a mesh shader's attribute blocks (32 vertices and 32 primitives
    a block: its vertex components, then its primitive components, 0x80 bytes each); the primitive bytes follow"""
    sys_, isbe, out = LAYOUT
    out = max(out, isbe + (payload + 0xf & ~0xf))   # a payload past 0x8c0 bytes would reach the outputs
    if kind == 'ts': return [sys_, isbe, out, out + payload], (out + payload + 0xf) // 4 + 4
    blocks = max((max_v + 31) // 32, (max_p + 31) // 32, 1)
    prim = out + blocks * max(len(comps) + len(pcomps), 1) * 0x80
    return [sys_, isbe, out, prim], (prim + 4 + 3 * max(max_p, 1) + 0xf) // 4 + 4


_ENTRY = re.compile(r'kernel\s+void\s+main0\s*\(((?:[^()]|\([^()]*\))*)\)\s*\{\s*threadgroup\s+spvUnsafeArray<uint,\s*(\d+)>\s+(\w+);\n')


def _split_params(params):
    out, depth, cur = [], 0, ''
    for ch in params:
        if ch == ',' and depth == 0: out.append(cur.strip()); cur = ''; continue
        depth += ch in '([<'; depth -= ch in ')]>'; cur += ch
    if cur.strip(): out.append(cur.strip())
    return out


def to_stage(msl, kind, info):
    """The translated kernel (msl) as the object (kind 'ts') or mesh ('ms') function main0 (and main0_z); info: threads, payload (bytes), mesh_layout, and for a
    mesh shader max_v, max_p, comps (sph_outputs)"""
    if info.get('own_shared'): raise ValueError(f'{kind}: shared memory of its own (LDS / STS / ATOMS) would share the stage memory at 0; not supported')
    m = _ENTRY.search(msl)
    if not m: raise ValueError(f'{kind}: no kernel main0 with its threadgroup array')
    params, words, arr = _split_params(m.group(1)), int(m.group(2)), m.group(3)
    names = [re.match(r'.*?(\w+)\s*(\[\[.*\]\])?$', p).group(1) for p in params]
    plain = [re.sub(r'\s*\[\[.*\]\]$', '', p) for p in params]
    body = f'static inline __attribute__((always_inline)) void dm_stage_body({", ".join(plain + [f"threadgroup spvUnsafeArray<uint, {words}>& {arr}"])})\n{{\n'
    src = msl[:m.start()] + body + msl[m.end():]
    call = f'dm_stage_body({", ".join(names + [arr])});'
    lay, thr, payw = info['mesh_layout'], info['threads'], (info['payload'] + 3) // 4
    sys_w, isbe_w, out_w, prim_b = lay[0] // 4, lay[1] // 4, lay[2] // 4, lay[3]
    # The thread's and the threadgroup's positions: the kernel's own parameters when it has them (an attribute may be given once), else dm_lid / dm_tg of their own
    own = {}
    for p in params:
        a = re.match(r'(uint3?)\s+(\w+)\s*\[\[(thread_position_in_threadgroup|threadgroup_position_in_grid)\]\]$', p)
        if a: own[a.group(3)] = a.group(2) + ('.x' if a.group(1) == 'uint3' else '')
    pos = ''.join(f', uint {v} [[{a}]]' for a, v in (('threadgroup_position_in_grid', 'dm_tg'), ('thread_position_in_threadgroup', 'dm_lid')) if a not in own)
    alias = ''.join(f'    uint {v} = {own[a]};\n' for a, v in (('threadgroup_position_in_grid', 'dm_tg'), ('thread_position_in_threadgroup', 'dm_lid')) if a in own)
    start = (alias + f'    threadgroup spvUnsafeArray<uint, {words}> {arr};\n'
             f'    for (uint i = dm_lid; i < {words}u; i += {thr}u) {arr}[i] = 0u;\n'
             f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n')
    head = f'struct dm_payload_t {{ uint w[{max(payw, 1)}]; }};\n'
    if kind == 'ts':
        return src + '\n' + head + (
            f'[[object]] void main0({", ".join(params)}, object_data dm_payload_t& dm_pl [[payload]], mesh_grid_properties dm_mgp'
            f'{pos}, constant uint* dm_first [[buffer(29)]])\n{{\n' + start +
            f'    if (dm_lid == 0u) {arr}[{sys_w}] = dm_first[0] + dm_tg;\n'
            f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n    {call}\n'
            f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
            f'    for (uint i = dm_lid; i < {payw}u; i += {thr}u) dm_pl.w[i] = {arr}[{out_w}u + i];\n'
            f'    if (dm_lid == 0u) dm_mgp.set_threadgroups_per_grid(uint3(min({arr}[{out_w + 1}], 1024u), 1u, 1u));\n}}\n')
    comps, max_v, max_p, pcomps = info['comps'], info['max_v'], info['max_p'], [tuple(c) for c in info.get('pcomps', [])]
    gens = sorted({n for n, _ in comps if n is not None}); pgens = sorted({n for n, _ in pcomps})
    stride = max(len(comps) + len(pcomps), 1) * 32   # words a block of 32 vertices and 32 primitives
    sets = ''
    for n in [None] + gens:
        lanes = [f'as_type<float>({arr}[b + {comps.index((n, c)) * 32}u])' if (n, c) in comps else ('1.0' if c == 3 else '0.0') for c in range(4)]
        sets += f'        o.{"position" if n is None else f"a{n}"} = float4({", ".join(lanes)});\n'
    psets = ''
    for n in pgens:
        lanes = [f'as_type<float>({arr}[b + {(len(comps) + pcomps.index((n, c))) * 32}u])' if (n, c) in pcomps else ('1.0' if c == 3 else '0.0') for c in range(4)]
        psets += f'        q.p{n} = float4({", ".join(lanes)});\n'
    head += ('struct dm_vertex_t {\n    float4 position [[position]];\n' + ''.join(f'    float4 a{n} [[user(locn{n})]];\n' for n in gens) + '};\n' +
             ('struct dm_prim_t {\n' + ''.join(f'    float4 p{n} [[user(locn{n})]];\n' for n in pgens) + '};\n' if pgens else '') +
             f'using dm_mesh_t = metal::mesh<dm_vertex_t, {"dm_prim_t" if pgens else "void"}, {max_v}, {max_p}, metal::topology::triangle>;\n')

    task = info.get('task', True)

    def entry(name, gl):
        # With a task stage the mesh shader reads its number among the task's launches as payload word 0 (ISBERD mode 0x38 at 0: the task writes from +4), and the
        # payload from +0x10; with none (control +0x5e clear) it reads its number with mode 0x78, the draw's first group (buffer 29) added
        return (f'[[mesh]] void {name}({", ".join(params)}, dm_mesh_t dm_out, ' + ('const object_data dm_payload_t& dm_pl [[payload]], ' if task else '') +
                f'{pos[2:]}{", " if pos else ""}constant uint* dm_first [[buffer(29)]])\n{{\n' + start +
                (f'    for (uint i = dm_lid; i < {payw}u; i += {thr}u) {arr}[{isbe_w}u + i] = dm_pl.w[i];\n'
                 f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
                 f'    if (dm_lid == 0u) {{ {arr}[{sys_w}] = dm_tg; {arr}[{isbe_w}] = dm_tg; }}\n' if task else
                 f'    if (dm_lid == 0u) {arr}[{sys_w}] = dm_first[0] + dm_tg;\n') +
                f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n    {call}\n'
                f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
                f'    uint np = min(({arr}[{prim_b // 4}] >> 24) & 0xffu, {max_p}u);\n'
                f'    if (dm_lid == 0u) dm_out.set_primitive_count(np);\n'
                f'    for (uint v = dm_lid; v < {max_v}u; v += {thr}u)\n    {{\n'
                f'        uint b = {out_w}u + (v / 32u) * {stride}u + (v % 32u);\n        dm_vertex_t o;\n' + sets +
                ('        o.position.z = fma(o.position.z, 0.5, o.position.w * 0.5);\n' if gl else '') +
                f'        dm_out.set_vertex(v, o);\n    }}\n' +
                (f'    for (uint p = dm_lid; p < np; p += {thr}u)\n    {{\n        uint b = {out_w}u + (p / 32u) * {stride}u + (p % 32u);\n        dm_prim_t q;\n' + psets +
                 f'        dm_out.set_primitive(p, q);\n    }}\n' if pgens else '') +
                f'    for (uint i = dm_lid; i < np * 3u; i += {thr}u) {{ uint a = {prim_b + 4}u + i; dm_out.set_index(i, ({arr}[a / 4u] >> ((a % 4u) * 8u)) & 0xffu); }}\n}}\n')
    return src + '\n' + head + entry('main0', True) + '\n' + entry('main0_z', False)

#!/usr/bin/env python3
"""Generate synthetic task/mesh stage-memory kernels and wrap them for GPU testing."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_meshstage as ms

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
head = '''#include <metal_stdlib>
using namespace metal;
template<typename T, size_t N> struct spvUnsafeArray {
 T elements[N];
 thread T& operator[](size_t p) thread { return elements[p]; }
 threadgroup T& operator[](size_t p) threadgroup { return elements[p]; }
};
'''

def kernel(words, body):
    return head + f'''kernel void main0(uint3 lid [[thread_position_in_threadgroup]])
{{
    threadgroup spvUnsafeArray<uint, {words}> stage;
    {body}
}}
'''

task_layout, task_words = ms.layout('ts', 0, [], 48)
task_info = dict(threads=32, payload=48, mesh_layout=task_layout)
task = kernel(task_words, f'''if (lid.x == 0) {{
 stage[{task_layout[2] // 4 + 1}] = 1u;
 stage[{task_layout[2] // 4 + 4}] = as_type<uint>(float(stage[0]) / 16.0);
}}''')
(out / 'task.metal').write_text(ms.to_stage(task, 'ts', task_info))

comps = [(None, c) for c in range(4)] + [(0, 0)]
layout, words = ms.layout('ms', 3, comps, 48, 1)
info = dict(threads=32, payload=48, mesh_layout=layout, max_v=3, max_p=1, comps=comps, task=True)
base, prim = layout[2] // 4, layout[3] // 4
mesh = kernel(words, f'''if (lid.x < 3u) {{
 uint v = lid.x;
 stage[{base} + v] = as_type<uint>(v == 1u ? 3.0 : -1.0);
 stage[{base + 32} + v] = as_type<uint>(v == 2u ? 3.0 : -1.0);
 stage[{base + 64} + v] = as_type<uint>(0.0);
 stage[{base + 96} + v] = as_type<uint>(1.0);
 stage[{base + 128} + v] = stage[{layout[1] // 4 + 4}];
}}
if (lid.x == 0u) {{ stage[{prim}] = 0x01000000u; stage[{prim + 1}] = 0x00020100u; }}''')
src = ms.to_stage(mesh, 'ms', info)
src += '\nfragment float4 pixel(dm_vertex_t in [[stage_in]]) { return float4(in.a0.x, 0.5, 0.75, 1.0); }\n'
(out / 'mesh.metal').write_text(src)
no_task = mesh.replace(f'stage[{layout[1] // 4 + 4}];', 'as_type<uint>(float(stage[0]) / 16.0);')
no_task = ms.to_stage(no_task, 'ms', dict(info, task=False))
no_task += '\nfragment float4 pixel(dm_vertex_t in [[stage_in]]) { return float4(in.a0.x, 0.5, 0.75, 1.0); }\n'
(out / 'mesh-no-task.metal').write_text(no_task)
print(out)

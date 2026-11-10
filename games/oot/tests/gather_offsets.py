#!/usr/bin/env python3
"""GatherOffsets.cs: textureGatherOffsets with its four offsets written out (rule from seedds/nxnative gather_offsets, MIT).

spirv-cross's spvGatherConstOffsets loops over coffsets[i]; Metal takes a gather's offset only as a constant expression and
reads other texels without an error. A call whose offsets form a 2x2 square becomes one gather at the square's corner with
the swizzle that puts its texels in the offsets' order; any other set becomes four gathers. A source without the call is
unchanged. The rewritten source compiles where xcrun is.
Usage: gather_offsets.py --service <totk_xlate> --work <folder outside the checkout>
"""
import argparse
from pathlib import Path
import shutil
import subprocess

MSL = '''#include <metal_stdlib>
using namespace metal;

constant spvUnsafeArray<int2, 4> _100 = spvUnsafeArray<int2, 4>({ int2(0), int2(1, 0), int2(0, 1), int2(1) });
constant spvUnsafeArray<int2, 4> _101 = spvUnsafeArray<int2, 4>({ int2(-1, 0), int2(0, -1), int2(1, 0), int2(0, 1) });
constant spvUnsafeArray<int2, 4> _102 = spvUnsafeArray<int2, 4>({ int2(3, 2), int2(2, 2), int2(3, 3), int2(2, 3) });

kernel void main0(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], texture2d<float, access::write> o [[texture(1)]], uint3 id [[thread_position_in_grid]])
{
    float2 _20 = float2(id.xy) / 64.0;
    float4 _30 = spvGatherConstOffsets(t, s, _100, component::x, select(_20, float2(0.0), isnan(_20))).xyzw;
    float4 _31 = spvGatherConstOffsets(t, s, _101, component::y, _20).xyzw;
    float4 _32 = spvGatherConstOffsets(t, s, _102, component::x, _20).xyzw;
    o.write(_30 + _31 + _32, id.xy);
}
'''
SQUARE = 'float4 _30 = (t.gather(s, select(_20, float2(0.0), isnan(_20)), int2(0, 0), component::x).wzxy).xyzw;'
CROSS = ('float4 _31 = dm_gather4(t.gather(s, _20, int2(-1, 0), component::y).w, t.gather(s, _20, int2(0, -1), component::y).w, '
         't.gather(s, _20, int2(1, 0), component::y).w, t.gather(s, _20, int2(0, 1), component::y).w).xyzw;')
SHIFTED = 'float4 _32 = (t.gather(s, _20, int2(2, 2), component::x).zwyx).xyzw;'   # (1,0) z, (0,0) w, (1,1) y, (0,1) x


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--service', type=Path, required=True)
    a = ap.parse_args()
    work = a.work.resolve()
    root = Path(__file__).resolve().parents[3]
    if work == root or root in work.parents:
        raise ValueError('generated fixtures must remain outside the checkout')
    work.mkdir(parents=True, exist_ok=True)

    def rewrite(name, msl):
        src = work / (name + '.in.metal')
        src.write_text(msl)
        return subprocess.run([str(a.service), '--gather-offsets', str(src)], capture_output=True, text=True, check=True).stdout

    failures = []

    def check(what, ok):
        print(('ok      ' if ok else 'FAILED  ') + what)
        if not ok:
            failures.append(what)

    out = rewrite('gather', MSL)
    check('a 2x2 square is one gather with the swizzle of its offsets', SQUARE in out)
    check('a square away from the origin, offsets in another order', SHIFTED in out)
    check('other offsets are four gathers', CROSS in out)
    check('no call is left', 'spvGatherConstOffsets(' not in out)
    check('the four-gather helper once, after the namespace line', out.count('dm_gather4(T a') == 1 and out.index('dm_gather4(T a') > out.index('using namespace metal;'))
    plain = MSL.replace('spvGatherConstOffsets', 'someOtherCall')
    check('a source without the call is unchanged', rewrite('plain', plain) == plain)
    if shutil.which('xcrun'):
        src = work / 'gather.out.metal'
        src.write_text(out.replace('constant spvUnsafeArray<int2, 4>', '// constant spvUnsafeArray<int2, 4>'))   # the arrays' template comes with spirv-cross's prologue, which this fixture leaves out
        r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-fmetal-math-mode=safe', '-fpreserve-invariance', '-fwrapv', str(src), '-o', str(work / 'gather.air')], capture_output=True, text=True)
        check('compiles', r.returncode == 0)
        if r.returncode:
            print(r.stderr[-800:])
    if failures:
        raise SystemExit(f'FAILED: {len(failures)} checks')
    print('PASSED')


if __name__ == '__main__':
    main()

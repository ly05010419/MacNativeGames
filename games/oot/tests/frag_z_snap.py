#!/usr/bin/env python3
"""FragZSnap.cs on MSL in the translator's shapes (ported with the rule from seedds/nxnative's test_dmsh_zsnap.py, MIT).

A gl_FragCoord.z read of main0 that meets a value read from a texture in one comparison (Ocarina of Time's mist: z > d after
the denormal flush; its foliage: z * k > d * k) takes dm_zsnap(gl_FragCoord.z, d, dm_zw), with dm_zw = fwidth(gl_FragCoord.z)
first in the body and the helper once. z against constants or uniforms, a texture value read after z or in a block z does
not see, and a shader without gl_FragCoord.z stay as they were. The rewritten sources compile where xcrun is.
Usage: frag_z_snap.py --service <totk_xlate> --work <folder outside the checkout>
"""
import argparse
from pathlib import Path
import shutil
import subprocess

HEAD = '''#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct spvDescriptorSetBuffer0
{
    texture2d<float> fp_t_tcb_A [[id(0)]];
    sampler fp_t_tcb_ASmplr [[id(1)]];
    constant float4* fp_c3 [[id(2)]];
};

struct main0_out
{
    float4 m_16 [[color(0)]];
};
'''
READ = 'float _112 = spvDescriptorSet0.fp_t_tcb_A.sample(spvDescriptorSet0.fp_t_tcb_ASmplr, _7217, level(0.0)).x;'
MIST = HEAD + '''
fragment main0_out main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    float2 _7217 = float2(gl_FragCoord.x / 768.0, gl_FragCoord.y / 432.0);
    ''' + READ + '''
    float _114 = gl_FragCoord.z;
    float _124 = abs(_114);
    bool _128 = _124 < 1.1754943508222875079687365372222e-38;
    float _130 = _128 ? 0.0 : _114;
    float _132 = abs(_112);
    bool _134 = _132 < 1.1754943508222875079687365372222e-38;
    float _136 = _134 ? 0.0 : _112;
    bool _138 = _130 > _136;
    if (_138)
    {
        discard_fragment();
    }
    out.m_16 = float4(1.0);
    return out;
}
'''
SCALED = HEAD + '''
fragment main0_out main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    float2 _432 = float2(gl_FragCoord.x / 768.0, gl_FragCoord.y / 432.0);
    float _71 = spvDescriptorSet0.fp_t_tcb_A.sample(spvDescriptorSet0.fp_t_tcb_ASmplr, _432).x;
    float _308 = gl_FragCoord.z;
    float _310 = _71 * spvDescriptorSet0.fp_c3[188].w;
    float _312 = _308 * spvDescriptorSet0.fp_c3[188].w;
    bool _326 = _312 > _310;
    if (_326)
    {
        out.m_16 = float4(1.0);
        return out;
    }
    out.m_16 = float4(0.5);
    return out;
}
'''
HELPER_HEAD = 'static inline __attribute__((always_inline)) float dm_zsnap(float z, float d, float zw)'


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
        r = subprocess.run([str(a.service), '--frag-z-snap', str(src)], capture_output=True, text=True, check=True)
        return r.stdout, int(r.stderr.strip())

    failures = []

    def check(what, ok):
        print(('ok      ' if ok else 'FAILED  ') + what)
        if not ok:
            failures.append(what)

    out, n = rewrite('mist', MIST)
    check('mist: one read rewritten', n == 1 and 'float _114 = dm_zsnap(gl_FragCoord.z, _112, dm_zw);' in out)
    check('mist: the width first in the body', '{\n    float dm_zw = fwidth(gl_FragCoord.z);\n    main0_out out = {};' in out)
    check('mist: the helper once', out.count(HELPER_HEAD) == 1)
    helper = out[out.index(HELPER_HEAD):out.index('}\n', out.index(HELPER_HEAD)) + 2]
    plain = out.replace(helper, '').replace('\n    float dm_zw = fwidth(gl_FragCoord.z);', '').replace('dm_zsnap(gl_FragCoord.z, _112, dm_zw)', 'gl_FragCoord.z')
    check('mist: nothing else changes', plain == MIST.replace('using namespace metal;\n', 'using namespace metal;\n\n'))
    compiled = {'mist': out}
    out, n = rewrite('scaled', SCALED)
    check('scaled on both sides: one read rewritten', n == 1 and 'float _308 = dm_zsnap(gl_FragCoord.z, _71, dm_zw);' in out)
    compiled['scaled'] = out
    alone = {
        'a uniform': MIST.replace(READ, 'float _112 = spvDescriptorSet0.fp_c3[3].x;'),
        'a constant': MIST.replace('bool _138 = _130 > _136;', 'bool _138 = _130 > 0.5;'),
        'a texture value read after z': MIST.replace('    ' + READ + '\n    float _114 = gl_FragCoord.z;\n', '    float _114 = gl_FragCoord.z;\n    ' + READ + '\n'),
        'a texture value read in a block z does not see': MIST.replace('    ' + READ + '\n    float _114 = gl_FragCoord.z;\n', '    float _112 = 0.0;\n    if (_7217.x > 0.5)\n    {\n        float _113 = spvDescriptorSet0.fp_t_tcb_A.sample(spvDescriptorSet0.fp_t_tcb_ASmplr, _7217, level(0.0)).x;\n        bool _139 = _113 > 2.0;\n    }\n    float _114 = gl_FragCoord.z;\n'),
        'a texture value with z in it': MIST.replace('float _136 = _134 ? 0.0 : _112;', 'float _136 = _134 ? 0.0 : (_112 + _114);'),
        'no gl_FragCoord.z': MIST.replace('gl_FragCoord.z', 'gl_FragCoord.w'),
    }
    for why, msl in alone.items():
        out, n = rewrite('alone', msl)
        check('left alone: ' + why, n == 0 and out == msl)
    if shutil.which('xcrun'):
        for name, msl in compiled.items():
            src = work / (name + '.out.metal')
            src.write_text(msl)
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-fmetal-math-mode=safe', '-fpreserve-invariance', '-fwrapv', str(src), '-o', str(work / (name + '.air'))], capture_output=True, text=True)
            check('compiles: ' + name, r.returncode == 0)
            if r.returncode:
                print(r.stderr[-800:])
    if failures:
        raise SystemExit(f'FAILED: {len(failures)} checks')
    print('PASSED')


if __name__ == '__main__':
    main()

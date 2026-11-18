"""tools/c1_bounds.py: the offline proof that a shader's c1 reads stay in its shared span (Ocarina of Time memory,
2026-11-16). Wrong answers draw with another shader's constants, so the unsound cases must stay unproven."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import c1_bounds as cb  # noqa: E402

# (body lines, index expression, the range proven, or None for unbounded)
CASES = [
    (['int _1 = as_type<int>(in.m_1.x) + (-100);', 'uint _2 = min(uint(_1), 4u);', 'int _3 = int(_2) << 2;',
      'uint _4 = uint(int(uint(_3) >> uint(2)));', 'uint _5 = uint(int(uint(int(_4)) >> uint(2)));'], 'int(_5)', (0, 1)),
    (['int _1 = as_type<int>(in.m_1.x);', 'int _2 = min(_1, 4);'], '_2', None),                 # may be negative
    (['int _1 = as_type<int>(in.m_1.x) & 15;'], '_1', (0, 15)),
    (['int _1 = as_type<int>(in.m_1.x) % 8;'], '_1', None),                                       # negative remainder
    (['uint _1 = as_type<uint>(in.m_1.x) % 8u;'], 'int(_1)', (0, 7)),
    (['int _1 = 3;', '_1 = 70000;'], '_1', (3, 70000)),                                           # reassigned: the union
    (['int _1 = 3;', '_1 += 1;'], '_1', None),                                                   # compound write
    (['int _1;', 'for (int _2 = 0; _2 < 4; _2++)', '_1 = _2;'], '_1', None),                    # a loop variable
    (['int _1 = 0;', 'int _2 = _1 + 1;', '_1 = min(_2, 3);'], '_1', None),                      # loop-carried
    (['uint _1 = 4u - 5u;'], 'int(_1)', None),                                                    # unsigned wrap
    (['int _1 = 2147483647;', 'int _2 = _1 + 1;'], '_2', None),                                   # signed overflow
    (['float _1 = in.m_1.x;', 'int _2 = int(_1);'], '_2', None),                                  # from a float
    (['int _1 = as_type<int>((*spvDescriptorSet0.fp_c1)._m0[0].x) >> 28;'], '_1', None),          # arithmetic shift
    (['uint _1 = as_type<uint>((*spvDescriptorSet0.fp_c1)._m0[0].x) >> 28u;'], 'int(_1)', (0, 15)),
    (['int _1 = clamp(as_type<int>(in.m_1.x), 0, 7);'], '_1', (0, 7)),
    (['int _1 = clamp(as_type<int>(in.m_1.x), 9, 7);'], '_1', None),                              # min > max
    (['int _1 = 1;', 'foo(_1);', 'int foo(thread int& _9)'], '_1', None),                         # passed by reference
    (['int _1 = 5;', 'bool _2 = _1 > 3;', 'int _3 = _2 ? 7 : _1;'], '_3', (5, 7)),
    (['uint _1 = as_type<uint>(in.m_1.x) >> 32u;'], 'int(_1)', None),                              # shift of 32
    (['int _1 = 2;', 'int _2 = _1;', 'int _2 = 9;'], '_2', None),                                 # declared twice
    (['int _1 = 2;', '_1++;'], '_1', None),
    (['int _1 = 2;', 'bar(&_1);'], '_1', None),                                                   # address taken
]

PROVEN = """struct spvDescriptorSetBuffer0
{
    constant _16* fp_c1 [[id(2)]];
};
fragment main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    uint _2 = min(as_type<uint>(in.m_1.x), 4u);
    int _3 = int(_2 >> 1u);
    uint _4 = as_type<uint>(in.m_1.y) & 3u;
    float _5 = (*spvDescriptorSet0.fp_c1)._m0[int(_3)][_4];
}
"""


def seed_file(folder, key, msl, c1_bytes=256):
    (folder / f'{key}_b.metal').write_text(msl)
    (folder / f'{key}_b.json').write_text(json.dumps({'c1': '00' * c1_bytes}))


class Values(unittest.TestCase):
    def test_cases(self):
        for body, index, want in CASES:
            with self.subTest(body=body, index=index):
                shader = cb.Shader(body)
                v = shader.eval(shader.parse(index))
                got = (v.lo, v.hi) if v.kind in ('int', 'uint') and v.lo >= 0 and v.hi < cb.TYPE_RANGE[v.kind][1] else None
                self.assertEqual(got, want)


class Shaders(unittest.TestCase):
    def test_a_masked_index_is_proven(self):
        self.assertTrue(cb.proven(PROVEN, 256))

    def test_an_index_past_the_span_is_not(self):
        # 256 bytes of data + 256 zero bytes = 32 float4: index [0, 2] fits, [0, 40] does not
        self.assertFalse(cb.proven(PROVEN.replace('min(as_type<uint>(in.m_1.x), 4u)', 'min(as_type<uint>(in.m_1.x), 80u)'), 256))

    def test_a_component_index_past_w_is_not(self):
        self.assertFalse(cb.proven(PROVEN.replace('& 3u', '& 7u'), 256))

    def test_c1_read_another_way_is_not(self):
        self.assertFalse(cb.proven(PROVEN + 'float4 _9 = spvDescriptorSet0.fp_c1->_m0[0];\n', 256))

    def test_the_runtime_rule_is_left_to_the_runtime(self):
        literal = PROVEN.replace('[int(_3)][_4]', '[3u].x')
        self.assertTrue(cb.literal_rule(literal, 256))
        self.assertFalse(cb.literal_rule(PROVEN, 256))


class List(unittest.TestCase):
    def test_list_names_the_proven_shader_and_is_kept_for_the_same_seed(self):
        with tempfile.TemporaryDirectory() as tmp:
            seed = Path(tmp) / 'seed'; seed.mkdir()
            seed_file(seed, 'aaaa-chain', PROVEN)
            seed_file(seed, 'bbbb-chain', PROVEN.replace('4u)', '80u)'))                      # not proven
            seed_file(seed, 'cccc-chain', PROVEN.replace('[int(_3)][_4]', '[0u].x'))         # the runtime proves it
            seed_file(seed, 'dddd-chain', PROVEN, c1_bytes=0)                               # no c1 data
            out = Path(tmp) / 'c1_bounded.tsv'
            self.assertEqual(cb.write_list(seed, out, 'one', workers=2), (1, False))
            rows = [line.split('\t') for line in out.read_text().splitlines() if not line.startswith('#')]
            self.assertEqual(rows, [['aaaa-chain', str(len(PROVEN.encode()))]])
            self.assertEqual(cb.write_list(seed, out, 'one', workers=2), (1, True))          # same seed: kept
            seed_file(seed, 'eeee-chain', PROVEN)
            self.assertEqual(cb.write_list(seed, out, 'two', workers=2), (2, False))         # another seed: made again


if __name__ == '__main__':
    unittest.main()

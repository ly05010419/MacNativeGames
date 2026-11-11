#!/usr/bin/env python3
"""Ampere memory instructions through the offline translator (dmsh_xlate, arch sm86): SUATOM (0x593), the image atomic Donkey Kong Bananza's agl
technique compute shaders (agl_technique_pfx) take an append counter with; shared memory's scaled address, IMAD.HI and ATOMS.CAS. Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, re, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
EXIT = (0x000fea0003800000, 0x000000000000794d)   # hi, lo: EXIT
# SUATOM.ADD.U32 R12, [R3], R12 on the 1D image whose handle is word 12 of c0 (AMapC0: image slot 1), as the game encodes it but unpredicated (PT)
SUATOM = (0x002ea200001ee000, 0x00000c0c030c7593)


def inst(hi, lo): return struct.pack('<QQ', lo, hi)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class Translated(unittest.TestCase):
    def translate(self, code, **extra):
        with tempfile.TemporaryDirectory(prefix='ampere_mem_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code)
            # c0 layout (QueryAmpereComputeC0Layout): no uniform or storage buffers, 3 texture handles from 0x10, image handles from 0x28 (word 12 = image slot 1), 0x40 bytes
            job = [dict(key='k', stage='cs', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, nvn2_cs=[0, 0, 3, 0x40, 0], **extra)]
            (work / 'jobs.json').write_text(json.dumps(job))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue((work / 'k.spv').exists(), r.stdout + r.stderr)
            sys.path.insert(0, str(TOOLS)); import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-texture-buffer-native',
                                str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return r.stdout


class Suatom(Translated):
    def test_add_u32_on_a_1d_image(self):
        msl = self.translate(inst(*SUATOM) + inst(*EXIT))
        self.assertIn('atomic_fetch_add', msl)   # native (the build's options) or through spirv-cross's buffer alias (these defaults)
        self.assertIn('cp_i_tcb_4A_r32ui', msl)   # image slot 1 (c2 0x128), a 32-bit unsigned image

    def test_result_kept(self):   # the old value is the shader's (it shuffles it to the warp): the atomic's result is used, not discarded
        msl = self.translate(inst(*SUATOM) + inst(*EXIT))
        self.assertRegex(msl, r'uint \w+ = [^;]*atomic_fetch_add')


class MufuF16(Translated):
    """MUFU.F16 (Splatoon Raiders' upscaler network): the source is the half bits 60..61 select (bit 62 its abs, as in every B slot), the result the low half"""
    LOG2_HIGH = (0x000fe60000000e00, 0x5000000c000c7308)   # MUFU.F16.LOG2 R12, |R12.H1|: the game's encoding (c1b8edc09e3313b1 +0x5970) on the register the atomic reads
    LOG2_LOW = (0x0002a20000000e00, 0x4000000c000c7308)    # MUFU.F16.LOG2 R12, |R12.H0| (+0x59a0)

    def msl(self, mufu):
        return self.translate(inst(*mufu) + inst(*SUATOM) + inst(*EXIT))

    def test_high_half(self):
        msl = self.msl(self.LOG2_HIGH)
        self.assertRegex(msl, r'as_type<half2>\(.*\)\)\.y;')
        self.assertIn('log2(abs', re.sub(r'float _\d+ = abs\((_\d+)\);\s*float _\d+ = log2\(_\d+\);', 'log2(abs', msl))

    def test_result_is_the_low_half(self):   # the bits of a half pair, its high half zero: not the float's own bits
        self.assertRegex(self.msl(self.LOG2_HIGH), r'as_type<uint>\(half2\(float2\(\w+, 0\.0\)\)\)')

    def test_low_half_translates_apart(self):   # R12 is a constant here, so the low half folds away; what is left must not read the high one
        self.assertNotRegex(self.msl(self.LOG2_LOW), r'as_type<half2>\(.*\)\)\.y;')


def mov(rd, value):   # MOV Rd, imm32: values the tests can see in the output
    return inst(0x000fc60000000f00, value << 32 | rd << 16 | 0x7802)


def sts(offset, rb):   # STS.U32 [RZ + offset], Rb: keeps its result
    return inst(0x000fe80000000800, offset << 40 | rb << 32 | 0xff << 24 | 0x7388)


class Shared(Translated):
    """Fire Emblem Fortune's Weave's Lighting compute shaders: the exposure histogram and its 64-bit average, in the game's own encodings"""
    STS_SCALED_4 = 0x000fe80000004800, 0x0000000302027388   # STS.U32 [R2 * 4], R3 (6d0f6db3f0d396a3 +0x80 with a value): R2 counts words. Read unscaled, bin b came back at about 4b
    STS_SCALED_8 = 0x000fe80000008a00, 0x0000000302027388   # STS.U64 [R2 * 8], R3:R4 (d5bc99ffd183fa20 +0xa0)
    STS_BYTES = 0x000fe80000000800, 0x0000000302027388      # STS.U32 [R2], R3: a byte address
    IMAD_HI = 0x000fc80007820008, 0x0000000b07087227        # IMAD.HI R8, R7, R11, {R9:R8} (08faccfa2eaa7e01 +0xa80)
    ATOMS_CAS = 0x000e640001800001, 0x00000400ff01738d      # ATOMS.CAS R1, [RZ + 4], R0, R1 (f4aa178bc67c9793 +0x200)

    def msl(self, *code):
        msl = self.translate(b''.join(code) + inst(*EXIT), shared=0x400)
        return msl[msl.index('kernel void'):]

    def test_a_32_bit_access_scales_its_register_by_4(self):   # Element 3, not byte 3 (which is element 0)
        self.assertIn('[3] = 77u', self.msl(mov(2, 3), mov(3, 77), inst(*self.STS_SCALED_4)))

    def test_a_64_bit_access_scales_its_register_by_8(self):   # Element 3 of 8 bytes, word 6
        msl = self.msl(mov(2, 3), mov(3, 77), mov(4, 88), inst(*self.STS_SCALED_8))
        self.assertIn('[6] = 77u', msl)
        self.assertIn('[7] = 88u', msl)

    def test_no_scale_keeps_the_byte_address(self):   # Byte 12 is word 3
        self.assertIn('[3] = 77u', self.msl(mov(2, 12), mov(3, 77), inst(*self.STS_BYTES)))

    def test_imad_hi_adds_the_pair_to_the_whole_product(self):   # Without the upper word and the low word's carry, the shader's 64-bit division gave 893011 for 218
        # 0xffffffff * 1 + {4000:1}: the low word carries, so the result is the product's high word + 4000 + 1 (these constants fold down to that sum)
        msl = self.msl(mov(7, 0xffffffff), mov(11, 1), mov(8, 1), mov(9, 4000), inst(*self.IMAD_HI), sts(0x20, 8))
        self.assertIn('._m1', msl)
        self.assertRegex(msl, r'\+ 4000\b')
        self.assertRegex(msl, r'\+ 1\b')

    # 08faccfa2eaa7e01: ISETP.GE P0, R2, 0x20 sets the carry, then IMAD.X adds it. Dropped, the shader's 64-bit division gave 2 for 340 / 1
    ISETP_P0 = 0x000fe20003f06070, 0x000000200200780c
    IMAD_X = 0x000fe400000e0605, 0x000000ffff057224       # IMAD.X R5, RZ, RZ, R5 + P0 (+0xc10)
    IMAD_X_NEG = 0x000fe400000e0e07, 0x000000ffff067224   # IMAD.X R6, RZ, RZ, -R7 + P0 (+0xb20): with .X a negated addend is its bitwise not

    def test_imad_x_adds_its_carry(self):
        self.assertIn('[8] = 101u', self.msl(mov(2, 0x40), mov(5, 100), inst(*self.ISETP_P0), inst(*self.IMAD_X), sts(0x20, 5)))

    def test_imad_x_without_the_carry_adds_nothing(self):
        self.assertIn('[8] = 100u', self.msl(mov(2, 1), mov(5, 100), inst(*self.ISETP_P0), inst(*self.IMAD_X), sts(0x20, 5)))

    def test_imad_x_negates_by_bitwise_not(self):   # ~100 + 1 is -100; a two's complement negation would give -99
        self.assertIn('[8] = 4294967196u', self.msl(mov(2, 0x40), mov(7, 100), inst(*self.ISETP_P0), inst(*self.IMAD_X_NEG), sts(0x20, 6)))

    def test_atoms_cas_takes_its_expected_value_from_rb_and_its_new_value_from_rc(self):
        msl = self.msl(mov(0, 5), mov(1, 9), inst(*self.ATOMS_CAS), sts(0x20, 1))
        self.assertIn('atomic_compare_exchange', msl)
        self.assertRegex(msl, r'= 5u;')
        self.assertRegex(msl, r', 9u,')


if __name__ == '__main__':
    unittest.main()

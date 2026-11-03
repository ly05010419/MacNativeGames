#!/usr/bin/env python3
"""Ampere instructions through the offline translator (dmsh_xlate, arch sm86): SUATOM (0x593), the image atomic Donkey Kong Bananza's agl technique compute shaders (agl_technique_pfx) take an
append counter with; shared memory's scaled address, IMAD.HI and ATOMS.CAS; and the instructions tested on the atomic's result below. Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
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


class BarrierMoves(Translated):
    """BMOV around warp shuffles (Hyrule Warriors: Age of Imprisonment's pixel shaders 02e49c189b769074 and 49241cf469c76830): convergence barrier B15 to and
    from R26, in the game's encodings. Per thread they do nothing, as the barriers they save do"""
    SAVE = (0x000fe20000000000, 0x000000001a0f7f55)      # +0x25c0
    RESTORE = (0x000fe40000100000, 0x000000001a0f7f56)   # +0x25d0
    RESTORE_PLAIN = (0x000fe40000000000, 0x000000001a0f7f56)

    def test_no_effect_on_the_shader(self):
        plain = self.translate(inst(*SUATOM) + inst(*EXIT))
        moved = self.translate(inst(*self.SAVE) + inst(*self.RESTORE) + inst(*SUATOM) + inst(*self.RESTORE_PLAIN) + inst(*EXIT))
        self.assertEqual(moved, plain)


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

    # 3,312 of Pokemon Pokopia's shared accesses have stride 3, on 4, 8 and 16 bytes alike (cs_rdb 990's STS.U32 [R2 * 16]): the field is a scale, not the access size
    STS_SCALED_16 = 0x0001e4000000c800, 0x0000000302027388

    def test_stride_3_scales_its_register_by_16(self):   # Element 3 of 16 bytes, word 12
        self.assertIn('[12] = 77u', self.msl(mov(2, 3), mov(3, 77), inst(*self.STS_SCALED_16)))

    # LDSM keeps its mode at 78..79, where LDS has its stride: 1 is a transposed read (MT88), not a scale. Donkey Kong Bananza's upscaler 7629d857a0a93833 +0x1ce0, on R3: scaled, its picture was garbage
    LDSM_MT88 = 0x000fe20000004200, 0x000000000314783b   # LDSM.16.MT88.4 R20, [R3], with no write barrier for the store to wait on

    def test_ldsm_mode_is_not_a_scale(self):
        msl = self.msl(mov(3, 0x40), inst(*self.LDSM_MT88), sts(0x20, 20))
        self.assertIn('spvSubgroupShuffle(8.96831', msl)   # the address 0x40 as a float's bits; scaled, it would be 0x100 (3.5873e-43)


class Pokopia(Translated):
    """Pokemon Pokopia's encodings, on the register the atomic reads: F2FP.MERGE_C (22,886 shaders), F2I from a half (1,067), MEMBAR with ERRBAR (144)"""
    MERGE_FIRST = (0x000fe200000040ff, 0x00c00600ff0c7a3e)   # F2FP.MERGE_C R12, RZ, c[3][word 6], RZ
    MERGE_SECOND = (0x000fe4000000400c, 0x00c00500ff0c7a3e)   # F2FP.MERGE_C R12, RZ, c[3][word 5], R12
    F2I_LOW = (0x000fe2000010f100, 0x0000000c000c7305)   # F2I.S32.F16.TRUNC R12, R12.H0
    F2I_HIGH = (0x000fe2000010f100, 0x1000000c000c7305)   # F2I.S32.F16.TRUNC R12, R12.H1
    # TLD.1D.LZ R12, R12, texture slot 0 (c0 word 4): 80,220 shaders, KTGL's light and decal lists in texture buffers. The game's words set a write barrier its next instructions wait on;
    # this one sets none, so the atomic sees its result
    TLD_1D = (0x000fe200009e01ff, 0x100004ff0c0c7b66)
    ATOMS_ADD = (0x004e680000000000, 0x0007fc0cff0c738c)   # ATOMS.ADD.U32 R12, [RZ + 0x7fc], R12, under PT
    MEMBAR = (0x000fec0000005000, 0x0000000000007992)
    ERRBAR = (0x000fc00000000000, 0x00000000000079ab)

    def msl(self, *code):
        return self.translate(b''.join(inst(*c) for c in code) + inst(*SUATOM) + inst(*EXIT))

    def test_merge_shifts_c_into_the_high_half(self):   # word 6, converted first, ends in the high half; word 5 in the low one
        msl = self.msl(self.MERGE_FIRST, self.MERGE_SECOND)
        high = re.search(r'uint (_\d+) = as_type<uint>\(half2\(float2\(\(\*spvDescriptorSet0\.cp_c3\)\._m0\[1\]\.z, 0\.0\)\)\);', msl)
        low = re.search(r'uint (_\d+) = as_type<uint>\(half2\(float2\(\(\*spvDescriptorSet0\.cp_c3\)\._m0\[1\]\.y, 0\.0\)\)\);', msl)
        self.assertTrue(high and low, msl)
        shifted = re.search(rf'int (_\d+) = int\({high.group(1)}\) << 16;', msl)
        self.assertTrue(shifted, msl)
        self.assertRegex(msl, rf'{shifted.group(1)} \| int\({low.group(1)}\)')

    def test_merge_with_rz_in_c_is_the_low_half_alone(self):
        msl = self.msl(self.MERGE_FIRST)
        self.assertRegex(msl, r'as_type<uint>\(half2\(float2\(\(\*spvDescriptorSet0\.cp_c3\)\._m0\[1\]\.z, 0\.0\)\)\)')
        self.assertNotIn('<< 16', msl)

    def test_f2i_reads_the_selected_half(self):   # the merge left its float in the low half and zero in the high one; the translator folds the pack and unpack
        self.assertIn('trunc(0.0)', self.msl(self.MERGE_FIRST, self.F2I_HIGH))
        self.assertRegex(self.msl(self.MERGE_FIRST, self.F2I_LOW), r'trunc\(\(\*spvDescriptorSet0\.cp_c3\)\._m0\[1\]\.z\)')

    def test_1d_fetch_wraps_into_rows(self):   # a texture buffer in the slot is bound as rows of 16384 texels (nvn_mtl.m tbuf_as_2d); a 1D texture, one row, reads the same texel
        msl = self.msl(self.MERGE_FIRST, self.TLD_1D)
        self.assertRegex(msl, r'& 16383')
        self.assertRegex(msl, r'>> uint\(14\)')
        self.assertRegex(msl, r'cp_t_tcb_8\.read\(uint2\(')   # slot 0's handle, c2 word 8, as a 2D texture
        self.assertNotIn('texture1d', msl)

    def test_atoms_add_on_shared_memory(self):   # the offset is in bytes: 0x7fc is word 511 of the workgroup's 0x800 bytes
        msl = self.translate(inst(*self.ATOMS_ADD) + inst(*SUATOM) + inst(*EXIT), shared=0x800)
        self.assertRegex(msl, r'atomic_fetch_add_explicit\(\(threadgroup atomic_uint\*\)&_\d+\[511\]')

    # HADD2 / HMUL2 with an immediate (form 2): the second source is the immediate, not the unused register field at 64 (R0, the blur's texture coordinate, in the shader that hung the GPU)
    HADD2_IMM = (0x000fe20000000900, 0x3e003e00ff0c7430)   # HADD2 R12, RZ, (1.5, 1.5)
    HADD2_NEG_IMM = (0x000fe20000000900, 0xb800b800ff0c7430)   # HADD2 R12, RZ, (-0.5, -0.5): bits 63 and 62 belong to the constant, they are not modifiers
    HMUL2_IMM = (0x000fe2000000080c, 0x400040000c0c7432)   # HMUL2 R12, R12, (2.0, 2.0)

    def test_half_add_takes_the_immediate(self):
        msl = self.msl(self.HADD2_IMM)
        self.assertIn('1.5', msl)
        self.assertNotRegex(msl, r'as_type<half2>\(\s*_?\w*r0')

    def test_half_immediate_keeps_its_sign(self):
        self.assertIn('-0.5', self.msl(self.HADD2_NEG_IMM))

    def test_half_multiply_takes_the_immediate(self):
        self.assertIn('2.0', self.msl(self.MERGE_FIRST, self.HMUL2_IMM))

    # HFMA2 .H0_NH1 (bit 86): the low lane multiplies by B's high half negated, the high lane by B's low half. R12 holds (c3[5], c3[6]) from the two merges
    HFMA2_NH1 = (0x000fe200004000ff, 0x0000000c0c0c7231)   # HFMA2 R12, R12, R12.H0_NH1, RZ
    HFMA2_PLAIN = (0x000fe200000000ff, 0x0000000c0c0c7231)   # HFMA2 R12, R12, R12, RZ

    def hfma2_lanes(self, hfma2):   # the generated code, and the names of A.lo, A.hi, B.lo, B.hi, unpacked in that order
        msl = self.msl(self.MERGE_FIRST, self.MERGE_SECOND, hfma2)
        lanes = re.findall(r'float (_\d+) = float2\(as_type<half2>\(uint\(_\d+\)\)\)\.[xy];', msl)
        self.assertEqual(len(lanes), 4, msl)
        return (msl, *lanes)

    def test_hfma2_nh1_rotates_b(self):
        msl, a_lo, a_hi, b_lo, b_hi = self.hfma2_lanes(self.HFMA2_NH1)
        negated = re.search(rf'float (_\d+) = -{b_hi};', msl)
        self.assertTrue(negated, msl)
        self.assertIn(f'fma({a_lo}, {negated.group(1)}, 0.0)', msl)   # low lane: A.lo * -B.hi
        self.assertIn(f'fma({a_hi}, {b_lo}, 0.0)', msl)               # high lane: A.hi * B.lo

    def test_hfma2_without_nh1_keeps_the_lanes(self):
        msl, a_lo, a_hi, b_lo, b_hi = self.hfma2_lanes(self.HFMA2_PLAIN)
        self.assertIn(f'fma({a_lo}, {b_lo}, 0.0)', msl)
        self.assertIn(f'fma({a_hi}, {b_hi}, 0.0)', msl)

    # FSEL keeps each operand's modifiers (161,748 of the game's register forms negate B): "x or -x" from one register is how its character and prop lighting picks a sign
    FSEL_NEG_B = (0x000fe40000000000, 0x8000000c0c0c7208)   # FSEL R12, P0, R12, -R12

    def test_fsel_negates_b(self):
        msl = self.msl(self.MERGE_FIRST, self.MERGE_SECOND, self.FSEL_NEG_B)
        self.assertRegex(msl, r'float _\d+ = -')

    def test_up2ur_reads_up0(self):   # cs_main's encoding: UR6 = UP0 ? 1 : 0
        self.msl((0x000fc60008000000, 0x000000013f067883))

    # Shared memory with a stride of 1: the address is Ra * 4 + offset. RZ would fold the scale away, so the index comes from F2I
    F2I_R3 = (0x000e24000021f000, 0x0000000c00037305)   # F2I.S32 R3, R12
    HIST_INC = (0x0011e4000d00403f, 0x0000000003ff7f8c)   # 0xf8c: the luminance histogram's hist[R3]++
    LDS_SCALED = (0x000fe20000004800, 0x00000000030c7984)   # LDS R12, [R3 * 4]
    LDS_PLAIN = (0x000fe20000000800, 0x00000000030c7984)   # LDS R12, [R3]

    def shared_msl(self, *code):
        return self.translate(b''.join(inst(*c) for c in (self.MERGE_FIRST, self.F2I_R3) + code) + inst(*SUATOM) + inst(*EXIT), shared=0x800)

    def test_histogram_increment_adds_one_at_the_scaled_index(self):
        msl = self.shared_msl(self.HIST_INC)
        self.assertRegex(msl, r'atomic_fetch_add_explicit\(\(threadgroup atomic_uint\*\)&_\d+\[[^\]]*\], 1u')
        self.assertRegex(msl, r'<< 2')

    def test_shared_load_scales_only_with_bit_78(self):
        self.assertRegex(self.shared_msl(self.LDS_SCALED), r'<< 2')
        self.assertNotRegex(self.shared_msl(self.LDS_PLAIN), r'<< 2')

    def test_membar_orders_memory(self):
        self.assertRegex(self.msl(self.MEMBAR, self.ERRBAR), r'threadgroup_barrier|atomic_thread_fence')


if __name__ == '__main__':
    unittest.main()

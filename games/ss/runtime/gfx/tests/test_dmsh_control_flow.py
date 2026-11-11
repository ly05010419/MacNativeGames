#!/usr/bin/env python3
"""Control flow through the offline translator (dmsh_xlate, arch sm86): forward jumps must not come out as loops, and a jump table must stop where the next one starts.
A loop the program does not have costs Metal's compilers dearly: Splatoon Raiders' uber shaders, with neither fix, took hours in the front end and crashed the GPU compiler.
Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, re, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
SIZE = 16   # an instruction's bytes
PT = 7      # the predicate that is always true
EXIT = (0x000fea0003800000, 0x000000000000794d)   # hi, lo
# SUATOM.ADD.U32 R12, [R3], R12: a statement that cannot be optimized away (test_dmsh_ampere_mem.py), and that says which block it is in by nothing but its place
WORK = (0x002ea200001ee000, 0x00000c0c030c7593)


def bits(*fields):
    v = 0
    for lo, width, value in fields:
        v |= (value & ((1 << width) - 1)) << lo
    return (v >> 64) & (2 ** 64 - 1), v & (2 ** 64 - 1)


def branch(opcode, offset, pred=PT, ra=255):   # offset: bytes from the next instruction. Bits 87..90: BRA's condition predicate, always true here
    return bits((0, 12, opcode), (12, 3, pred), (24, 8, ra), (34, 48, offset // 4), (87, 3, PT))


def bra(at, to, pred=PT):
    return branch(0x947, to * SIZE - (at + 1) * SIZE, pred)


def ldc(rd, ra, offset):   # LDC Rd, c1[Ra + offset], 32 bits
    return bits((0, 12, 0xb82), (12, 3, PT), (16, 8, rd), (24, 8, ra), (38, 16, offset), (54, 5, 1), (73, 3, 4))


def assemble(insts):
    return b''.join(struct.pack('<QQ', lo, hi) for hi, lo in insts)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class ControlFlow(unittest.TestCase):
    def translate(self, insts, c1=b''):
        with tempfile.TemporaryDirectory(prefix='control_flow_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(assemble(insts))
            job = [dict(key='k', stage='cs', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, nvn2_cs=[0, 0, 3, 0x40, 0], c1=c1.hex())]
            (work / 'jobs.json').write_text(json.dumps(job))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue((work / 'k.spv').exists(), r.stdout + r.stderr)
            sys.path.insert(0, str(TOOLS)); import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return r.stdout

    def assert_no_loop(self, msl):
        self.assertNotRegex(msl[msl.index('kernel void main0'):], r'\bdo\b|\bwhile\b|\bfor\b')

    def test_jump_into_what_follows_the_if(self):
        # if (!P0) { if (P1) goto M; work; goto N; }  L: work;  M: work;  N: exit. The goto to N moves out of the if, and an else around L and M would leave the goto to M only a loop
        program = [bra(0, 5, pred=0), bra(1, 6, pred=1), WORK, WORK, bra(4, 8), WORK, WORK, WORK, EXIT]
        msl = self.translate(program)
        self.assertEqual(msl.count('atomic_fetch_add'), 5, msl)   # every statement is still there
        self.assert_no_loop(msl)

    def test_switches_one_after_another(self):
        # The same jump out of each case, again and again: each else took the rest of the program, and each loop closed at its end
        program, at, n = [], 0, 6
        for _ in range(n):
            program += [bra(at, at + 5, pred=0), bra(at + 1, at + 6, pred=1), WORK, bra(at + 3, at + 7), WORK, WORK, WORK]
            at += 7
        program.append(EXIT)
        self.assert_no_loop(self.translate(program))

    def test_real_loop_stays(self):
        msl = self.translate([WORK, bra(1, 0, pred=0), EXIT])
        self.assertRegex(msl[msl.index('kernel void main0'):], r'\bwhile\b|\bfor\b')

    def two_switches(self, second_extra=()):
        # 0: LDC R1, c1[R2 + 0]; 1: BRX R1 to 2 or 3 (each goes on to the join at 4); 4: LDC R1, c1[R2 + 8]; 5: BRX R1 to 6 or 7; 8: exit.
        # The game's table words are offsets from one anchor, the instruction after the first BRX, and each BRX's immediate leads there, so one switch can read another's words as targets of its own
        anchor = 2 * SIZE
        program = [ldc(1, 2, 0), branch(0x949, 0, ra=1), bra(2, 4), WORK, ldc(1, 2, 8), branch(0x949, anchor - 6 * SIZE, ra=1), bra(6, 8), WORK, EXIT]
        words = [t * SIZE - anchor for t in (2, 3, 6, 7, *second_extra)]
        return program, struct.pack(f'<{len(words)}i', *words)

    def test_table_ends_at_the_next_table(self):
        # The second BRX is reached only through the first one's targets. Read at once, the first table takes the second's two targets as its own, so it compares with three addresses, not one
        msl = self.translate(*self.two_switches())
        self.assertEqual(len(re.findall(r'== (?:\w+\()?\d+', msl)), 2, msl)

    def test_table_loaded_before_a_join(self):
        # 0: LDC R1, c1[R2 + 0]; 1, 2: two branches to 4; 3: work; 4: BRX R1 to 5 or 6; 5: jump to 7; 6: work; 7: exit. The register is loaded before the join the BRX comes after
        anchor = 5 * SIZE
        program = [ldc(1, 2, 0), bra(1, 4, pred=0), bra(2, 4, pred=1), WORK, branch(0x949, 0, ra=1), bra(5, 7), WORK, EXIT]
        words = [t * SIZE - anchor for t in (5, 6)]
        msl = self.translate(program, struct.pack('<2i', *words))
        self.assertEqual(len(re.findall(r'== (?:\w+\()?\d+', msl)), 1, msl)

    def test_entry_for_program_start_is_no_target(self):
        # Words that lead to address 0 fill a table's unused cases: taken as targets, they put the whole program in a loop
        msl = self.translate(*self.two_switches(second_extra=(0, 0, 0)))
        self.assert_no_loop(msl)
        self.assertEqual(len(re.findall(r'== (?:\w+\()?\d+', msl)), 2, msl)


if __name__ == '__main__':
    unittest.main()

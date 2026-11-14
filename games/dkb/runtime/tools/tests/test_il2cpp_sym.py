"""IL2CPP names and branch targets use module memory offsets, never stored NSO offsets."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from dm.tools.tests.test_nso import make_nro, make_nso

TOOL = Path(__file__).resolve().parents[1] / 'il2cpp_sym.py'
# .eh_frame functions: the aliases, a native function no method starts, Target, Tail, then a gap and another native function
FUNCTIONS = [(0x20, 0x40), (0x40, 0x80), (0x80, 0x90), (0x90, 0xa0), (0x100, 0x140)]


def unwind(text, functions, mod0=0xe00):
    """MOD0 and an .eh_frame_hdr in text describing the functions, as test_nso_match.image lays them out (a zR CIE, pcrel/sdata4 PCs)"""
    ehs, cie = mod0 + 0x40, mod0 + 0x100
    struct.pack_into('<I', text, 4, mod0)
    text[mod0:mod0 + 4] = b'MOD0'
    struct.pack_into('<ii', text, mod0 + 0x10, ehs - mod0, ehs + 12 + len(functions) * 8 - mod0)
    text[ehs:ehs + 4] = bytes((1, 0x1b, 3, 0x3b))
    struct.pack_into('<iI', text, ehs + 4, cie - (ehs + 4), len(functions))
    struct.pack_into('<II', text, cie, 16, 0)
    text[cie + 8:cie + 17] = b'\x01zR\0\x04\x78\x1e\x01\x1b'
    for i, (start, end) in enumerate(functions):
        fde = cie + 0x40 + i * 24
        struct.pack_into('<ii', text, ehs + 12 + i * 8, start - ehs, fde - ehs)
        struct.pack_into('<IIiI', text, fde, 16, fde + 4 - cie, start - (fde + 8), end - start)


class Il2CppSymbols(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.module = Path(self.tmp.name) / 'main'
        self.script = Path(self.tmp.name) / 'script.json'
        self.text = bytearray(struct.pack('<I', 0xd503201f) * 1024)
        struct.pack_into('<4I', self.text, 0x20, 0x94000018, 0x1400001b, 0x940007f6, 0xd61f0000)
        struct.pack_into('<I', self.text, 0x30, 0xffffffff)
        struct.pack_into('<I', self.text, 0x34, 0x17fffffc)   # b 0x24: interior of both aliases at 0x20
        struct.pack_into('<2I', self.text, 0x38, 0x94000002, 0x94000031)   # bl 0x40, bl 0x100: native functions no method starts
        unwind(self.text, FUNCTIONS)
        self.module.write_bytes(make_nso((self.text, b'ro', b'data' * 64)))
        self.rows = [{'Address': a, 'Name': n, 'Signature': 'void f();', 'TypeSignature': 'v'}
                     for a, n in [(0x90, 'Tail'), (0x20, 'Zed'), (0x80, 'Target'), (0x20, 'Alpha'), (0x20, 'Alpha')]]
        self.write_rows(self.rows)

    def write_rows(self, rows):
        self.script.write_text(json.dumps({'ScriptMethod': rows}))

    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(TOOL), str(self.module), str(self.script), *args],
                              capture_output=True, text=True, timeout=10)

    def assert_error(self, result):
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('[RED]', result.stderr)
        self.assertNotIn('Traceback', result.stderr)

    def test_lookup_before_first_nearest_and_sorted_aliases(self):
        r = self.run_tool('4', '0x20', '84')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, '0x4 unknown\n0x20 Alpha | Zed\n0x84 Target+0x4\n')
        self.write_rows(list(reversed(self.rows)))
        self.assertEqual(self.run_tool('0x20').stdout, '0x20 Alpha | Zed\n')

    def test_interior_lookup_groups_aliases_before_offset(self):
        r = self.run_tool('24')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, '0x24 (Alpha | Zed)+0x4\n')

    def test_direct_branch_groups_target_aliases_before_offset(self):
        r = self.run_tool('--dis', '34', '--count', '1')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('; (Alpha | Zed)+0x4', r.stdout)

    def test_names_stay_within_the_eh_frame_function(self):
        r = self.run_tool('9c', 'a4', 'fc', '40', '44', '7c', '104')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, '0x9c Tail+0xc\n0xa4 unknown\n0xfc unknown\n0x40 unknown (function 0x40)\n'
                                   '0x44 unknown (function 0x40+0x4)\n0x7c unknown (function 0x40+0x3c)\n0x104 unknown (function 0x100+0x4)\n')

    def test_direct_branch_to_a_function_without_a_method(self):
        r = self.run_tool('--dis', '38:40')
        self.assertEqual(r.returncode, 0, r.stderr)
        lines = r.stdout.splitlines()
        self.assertTrue(lines[0].endswith('; unknown (function 0x40)'), lines[0])
        self.assertTrue(lines[1].endswith('; unknown (function 0x100)'), lines[1])

    def test_module_without_function_ranges_is_an_error(self):
        no_mod0, no_functions = bytearray(self.text), bytearray(self.text)
        struct.pack_into('<I', no_mod0, 4, 0); unwind(no_functions, [])
        for what, text in [('no MOD0', no_mod0), ('an empty search table', no_functions)]:
            with self.subTest(what):
                self.module.write_bytes(make_nso((bytes(text), b'ro', b'data' * 64)))
                r = self.run_tool('20'); self.assert_error(r); self.assertIn('.eh_frame', r.stderr)

    def test_nro_uses_the_same_addresses(self):
        data, _ = make_nro((self.text, b'ro', b'data'))
        struct.pack_into('<I', data, 0x1c, 0)   # NRO flags replace the synthetic NOP word.
        self.module.write_bytes(data)
        r = self.run_tool('0x84', '--dis', '0x80', '--count', '1')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('0x84 Target+0x4', r.stdout)
        self.assertIn('nop', r.stdout)

    def test_disassembly_uses_loaded_text_and_annotates_only_direct_branches(self):
        r = self.run_tool('--dis', '0x20:0x30')
        self.assertEqual(r.returncode, 0, r.stderr)
        lines = r.stdout.splitlines()
        self.assertEqual(len(lines), 4)
        self.assertIn('bl', lines[0]); self.assertIn('; Target', lines[0])
        self.assertIn('b ', lines[1]); self.assertIn('; Tail', lines[1])
        self.assertIn('0x2000', lines[2]); self.assertIn('; unknown', lines[2])
        self.assertNotIn(';', lines[3])
        r = self.run_tool('--dis', '20', '--count', '2')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(r.stdout.splitlines()), 2)

    def test_inputs_outside_text_and_invalid_ranges_are_errors(self):
        for args in [('1000',), ('7100000020',), ('-1',), ('xyz',),
                     ('--dis', '1000'), ('--dis', '24:20'), ('--dis', '21:28'),
                     ('--dis', 'ffc', '--count', '2'), ('--dis', '20', '--count', '0'),
                     ('--dis', '20:28', '--count', '1'), ('--count', '2')]:
            with self.subTest(args=args): self.assert_error(self.run_tool(*args))

    def test_malformed_script_is_an_error(self):
        for value in [[], {}, {'ScriptMethod': []}, {'ScriptMethod': None},
                      {'ScriptMethod': [None]}, {'ScriptMethod': [{'Address': True, 'Name': 'X'}]},
                      {'ScriptMethod': [{'Address': '0x20', 'Name': 'X'}]},
                      {'ScriptMethod': [{'Address': 0x20, 'Name': ''}]},
                      {'ScriptMethod': [{'Address': 0x20, 'Name': None}]}]:
            with self.subTest(value=value):
                self.script.write_text(json.dumps(value)); self.assert_error(self.run_tool('20'))
        self.script.write_text('{'); self.assert_error(self.run_tool('20'))

    def test_nonrelative_or_noncode_script_addresses_are_rejected(self):
        for address in [0x7100000020, 0x1000, -1, 0x21]:
            with self.subTest(address=address):
                self.write_rows([{'Address': address, 'Name': 'WrongBase'}])
                r = self.run_tool('20'); self.assert_error(r); self.assertIn('text', r.stderr)

    def test_damaged_module_is_rejected_by_shared_loader(self):
        data = make_nso((self.text, b'ro', b'data'), compress=(0, 0, 0))
        data[0x180] ^= 1; self.module.write_bytes(data)
        r = self.run_tool('20'); self.assert_error(r); self.assertIn('SHA-256', r.stderr)
        self.module.write_bytes(b'NSO0'); self.assert_error(self.run_tool('20'))

    def test_missing_files_and_undecodable_instructions_are_errors(self):
        self.assert_error(self.run_tool('--dis', '30', '--count', '1'))
        self.script.unlink(); self.assert_error(self.run_tool('20'))
        self.write_rows(self.rows); self.module.unlink(); self.assert_error(self.run_tool('20'))

    def test_help_and_missing_arguments(self):
        self.assert_error(self.run_tool())
        r = subprocess.run([sys.executable, str(TOOL), '--help'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('module-relative', r.stdout)


if __name__ == '__main__':
    unittest.main()

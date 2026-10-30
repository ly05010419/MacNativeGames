"""The override audit: does each replaced SDK function answer in the original's type (Q69)."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from types import SimpleNamespace

GAME = Path(__file__).resolve().parents[1]   # games/ssbu
SCRIPT = GAME/'scripts/override_audit.py'
spec = importlib.util.spec_from_file_location('override_audit', SCRIPT)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
VENV = GAME/'SSBU_build/venv/bin/python'


def ins(mnemonic, op_str, address=0):
    return SimpleNamespace(mnemonic=mnemonic, op_str=op_str, address=address)


class Verdict(unittest.TestCase):
    def test_result_codes_where_the_sdk_returns_a_bool(self):
        # CalculateSpanBetween before Q66: 0 on success, R_ codes on failure, the game reads a bool
        self.assertEqual(audit.verdict('u32', 'bool', ['R_INVALID', 'R_SOURCE_MISMATCH', '0']), 'MISMATCH')
        self.assertEqual(audit.verdict('bool', 'bool', ['r == 0']), 'ok')

    def test_one_and_zero_in_32_bits_answer_a_bool(self):
        self.assertEqual(audit.verdict('u32', 'bool', ['1', '0']), 'ok')
        self.assertEqual(audit.verdict('u32', 'bool', ['0']), 'CHECK')     # always false: meant so?

    def test_void_where_the_game_reads_a_value(self):
        self.assertEqual(audit.verdict('void', 'result', []), 'MISMATCH')
        self.assertEqual(audit.verdict('void', 'bool', []), 'MISMATCH')
        self.assertEqual(audit.verdict('void', 'void', []), 'ok')

    def test_width(self):
        self.assertEqual(audit.verdict('u32', 'u64', ['0']), 'MISMATCH')
        self.assertEqual(audit.verdict('u64', 'u64', ['x']), 'ok')
        self.assertEqual(audit.verdict('u32', 'result', ['R_INVALID', '0']), 'ok')


class SdkSide(unittest.TestCase):
    def test_classes(self):
        self.assertEqual(audit.sdk_class({('const', 0), ('const', 1)}), 'bool')
        self.assertEqual(audit.sdk_class({('const', 0), ('const', 0x74)}), 'result')
        self.assertEqual(audit.sdk_class({('bool',)}), 'bool')
        self.assertEqual(audit.sdk_class({('u32',), ('const', 0)}), 'r32')
        self.assertEqual(audit.sdk_class({('none',)}), 'void')
        self.assertEqual(audit.sdk_class(set()), 'unknown')

    def test_register_sources(self):
        self.assertEqual(audit.reg_source(ins('mov', 'w0, #1'), 0), ('const', 1))
        self.assertEqual(audit.reg_source(ins('mov', 'w0, wzr'), 0), ('const', 0))
        self.assertEqual(audit.reg_source(ins('cset', 'w0, eq'), 0), ('bool',))
        self.assertEqual(audit.reg_source(ins('and', 'x0, x0, #0xffffffff'), 0), ('u32',))
        self.assertEqual(audit.reg_source(ins('mov', 'x0, x19'), 0), ('copy', 19))
        self.assertEqual(audit.reg_source(ins('csel', 'x0, xzr, x20, eq'), 0), ('fork', [None, 20]))
        self.assertIsNone(audit.reg_source(ins('str', 'x0, [sp, #8]'), 0))
        self.assertIsNone(audit.reg_source(ins('ldp', 'x29, x30, [sp, #0x10]'), 0))
        self.assertEqual(audit.reg_source(ins('bl', '#0x1000'), 0), ('call', 0x1000))
        self.assertIsNone(audit.reg_source(ins('bl', '#0x1000'), 19))       # x19 survives a call
        self.assertEqual(audit.reg_source(ins('bl', '#0x1000'), 8), ('clobbered',))


class HostSide(unittest.TestCase):
    def test_types_and_returns_from_the_sources(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d)/'host.c'
            f.write_text('static void nop(void){} static uint32_t ok(void) { return 0; }\n'
                         'static unsigned span(int64_t *o) {\n    if (!o) return R_INVALID;\n    return 0;\n}\n'
                         'static uint8_t yes(void){return 1;}\n'
                         'void *runtime_x_override(const char *name) {\n'
                         '    static const struct { const char *name; void *fn; } e[] = {{"_ZN2nn1a1bEv", nop}, {"_ZN2nn1a1cEv", span}};\n'
                         '}\n')
            self.assertEqual(audit.host_type('nop', [f])[0], 'void')
            self.assertEqual(audit.host_type('ok', [f])[:2], ('u32', 'uint32_t'))
            self.assertEqual(audit.host_type('span', [f])[2], ['R_INVALID', '0'])
            self.assertEqual(audit.host_type('yes', [f])[0], 'bool')
            self.assertEqual([e[:2] for e in audit.host_entries(Path(d))], [('_ZN2nn1a1bEv', 'nop'), ('_ZN2nn1a1cEv', 'span')])
            self.assertEqual(audit.host_entries(Path(d))[0][3], 'runtime_x_override')


@unittest.skipUnless(VENV.exists() and audit.SDK_IMAGE.exists(), 'needs the Smash work folder with its venv (capstone)')
class Real(unittest.TestCase):
    def test_the_spirit_board_case_is_found_and_its_fix_passes(self):
        with tempfile.TemporaryDirectory() as d:
            rt = Path(d)/'runtime'
            rt.mkdir()
            (rt/'time_host.c').write_text(
                'static unsigned span_between(int64_t *o, const void *a, const void *b) { if (!o) return R_INVALID; return 0; }\n'
                'void *runtime_time_override(const char *name) {\n static const struct { const char *name; void *fn; } e[] = {\n'
                ' {"_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_", span_between}};\n}\n')
            out = subprocess.run([str(VENV), '-I', str(SCRIPT), '--runtime', str(rt)], capture_output=True, text=True).stdout
            self.assertIn('MISMATCH nn::time::CalculateSpanBetween', out)
            (rt/'time_host.c').write_text((rt/'time_host.c').read_text().replace('static unsigned span_between', 'static uint8_t span_between')
                                          .replace('return R_INVALID; return 0;', 'return 0; return 1;'))
            out = subprocess.run([str(VENV), '-I', str(SCRIPT), '--runtime', str(rt)], capture_output=True, text=True).stdout
            self.assertIn('ok       nn::time::CalculateSpanBetween', out)


if __name__ == '__main__':
    unittest.main()

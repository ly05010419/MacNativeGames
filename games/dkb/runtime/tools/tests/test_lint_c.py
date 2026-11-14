"""The pure-Python parts of dm/tools/lint_c.py: the baseline and its ratchet (a new finding fails, a fixed one still listed fails until --update shrinks it, and the
baseline may not grow over the base's), the compile database's rule for a source several Makefiles compile, and clang-tidy's output. No LLVM, Xcode or game files needed."""
import contextlib
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lint_c as lint

REPO = lint.REPO
BASELINE = '''# clang-tidy findings of dm/host and dm/gfx per file and check
# python3 dm/tools/lint_c.py tidy --update. Made with LLVM 23.1.4.
# file\tcheck\tfindings
dm/gfx/vk/hle_vk.m\tclang-analyzer-unix.Malloc\t1
dm/host/hle_hid.c\tclang-analyzer-core.BitwiseShift\t2
'''
HID = ('dm/host/hle_hid.c', 'clang-analyzer-core.BitwiseShift')
VK = ('dm/gfx/vk/hle_vk.m', 'clang-analyzer-unix.Malloc')


class Baseline(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(dict(lint.parse_baseline(BASELINE)), {VK: 1, HID: 2})
        self.assertEqual(lint.baseline_llvm(BASELINE), '23.1.4')
        self.assertIsNone(lint.baseline_llvm('# no version\n'))

    def test_written_baseline_reads_back(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.object(lint, 'BASELINE', Path(d) / 'b.tsv'):
            lint.write_baseline({HID: 2, VK: 1}, 'LLVM 23.1.4')
            self.assertEqual(dict(lint.read_baseline()), {VK: 1, HID: 2})
            self.assertEqual(lint.baseline_llvm(lint.BASELINE.read_text()), '23.1.4')

    def test_same_counts_pass(self):
        self.assertEqual(lint.compare({HID: 2, VK: 1}, lint.parse_baseline(BASELINE)), ([], []))

    def test_growth_fails(self):
        base = lint.parse_baseline(BASELINE)
        worse, better = lint.compare({HID: 3, VK: 1}, base)
        self.assertEqual(worse, [(HID, 3, 2)]); self.assertEqual(better, [])
        new = ('dm/host/hle_os.c', 'clang-analyzer-core.NullDereference')   # a file and check the baseline does not list
        worse, _ = lint.compare({HID: 2, VK: 1, new: 1}, base)
        self.assertEqual(worse, [(new, 1, 0)])

    def test_shrink_is_reported(self):
        worse, better = lint.compare({HID: 1}, lint.parse_baseline(BASELINE))
        self.assertEqual(worse, [])
        self.assertEqual(better, [(VK, 0, 1), (HID, 1, 2)])

    def test_update_only_shrinks(self):
        base = lint.parse_baseline(BASELINE)
        new = ('dm/host/hle_os.c', 'clang-analyzer-core.NullDereference')
        self.assertEqual(dict(lint.shrink(base, {HID: 1, VK: 3, new: 1})), {HID: 1, VK: 1})   # lowered, never raised, nothing added
        self.assertEqual(dict(lint.shrink(base, {HID: 2})), {HID: 2})   # a fixed entry is dropped

    def test_partial_run_finds_nothing_fixed(self):
        # a file's findings also come from the sources that include it: checking some sources cannot tell a fixed one
        self.assertEqual(lint.compare({HID: 1}, lint.parse_baseline(BASELINE), partial=True), ([], []))


class Tidy(unittest.TestCase):
    """tidy's exit status and messages, with clang-tidy's output stubbed."""
    def run_tidy(self, findings, files=()):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 'compile_commands.json').write_text(json.dumps([{'file': str(REPO / 'dm/host/hle_hid.c')}, {'file': str(REPO / 'dm/gfx/vk/hle_vk.m')}]))
            (d / 'b.tsv').write_text(BASELINE)
            version = mock.Mock(stdout='LLVM version 23.1.4\n')
            args = mock.Mock(files=list(files), update=False, rebaseline=False, against=None, report=None, jobs=2)
            with mock.patch.multiple(lint, DB_DIR=d, BASELINE=d / 'b.tsv', llvm_tool=lambda name: name, tidy_one=lambda tool, f: (f, findings.get(lint.rel(f), []))), \
                 mock.patch.object(lint.subprocess, 'run', return_value=version), mock.patch.dict(lint.os.environ, {'GITHUB_STEP_SUMMARY': ''}), \
                 contextlib.redirect_stdout(io.StringIO()) as out:
                return lint.tidy(args), out.getvalue()

    def hid(self, n):
        return [('dm/host/hle_hid.c', 230 + i, 5, HID[1], 'Right operand is negative in left shift') for i in range(n)]

    def test_baseline_counts_pass(self):
        rc, out = self.run_tidy({'dm/host/hle_hid.c': self.hid(2), 'dm/gfx/vk/hle_vk.m': [('dm/gfx/vk/hle_vk.m', 991, 137, VK[1], 'Use of memory after it is released')]})
        self.assertEqual(rc, 0, out)
        self.assertIn('0 file/check pairs above it, 0 below it', out)

    def test_new_finding_fails_and_is_listed_with_its_line(self):
        rc, out = self.run_tidy({'dm/host/hle_hid.c': self.hid(3), 'dm/gfx/vk/hle_vk.m': [('dm/gfx/vk/hle_vk.m', 991, 137, VK[1], 'Use of memory after it is released')]})
        self.assertEqual(rc, 1)
        self.assertIn(f'dm/host/hle_hid.c: {HID[1]}: 3 findings, the baseline has 2', out)
        self.assertIn(f'::error file=dm/host/hle_hid.c,line=232,col=5::Right operand is negative in left shift [{HID[1]}] (3 in this file, the baseline has 2)', out)

    def test_fixed_finding_still_listed_fails(self):
        rc, out = self.run_tidy({'dm/host/hle_hid.c': self.hid(2)})
        self.assertEqual(rc, 1)
        self.assertIn(f'::error file=dm/gfx/vk/hle_vk.m::{VK[1]}: 0 findings, the baseline still has 1: shrink it', out)

    def test_named_sources_report_nothing_fixed(self):
        rc, out = self.run_tidy({'dm/host/hle_hid.c': self.hid(1)}, files=['dm/host/hle_hid.c'])
        self.assertEqual(rc, 0)
        self.assertNotIn('baseline still has', out)


class ClangTidyOutput(unittest.TestCase):
    def test_findings_in_linted_files_per_check(self):
        out = '\n'.join([
            f'{REPO}/dm/gfx/nvn/hle_nvn.c:1695:39: warning: Division by zero [clang-analyzer-core.DivideZero]',
            f'{REPO}/dm/gfx/nvn/hle_nvn.c:1690:5: note: Calling \'fps_step_cap\'',
            f'{REPO}/dm/host/hle_hid.c:303:54: warning: \'signed char\' to \'int\' conversion [bugprone-signed-char-misuse,cert-str34-c]',
            f'{REPO}/dm/host/hle_hid.c:10:1: warning: unused variable \'x\' [clang-diagnostic-unused-variable]',
            f'{REPO}/dm/host/hle_hid.c:11:1: error: unknown type name \'foo\' [clang-diagnostic-error]',
            f'{REPO}/dm/third_party/libvpx/vpx/vpx_decoder.h:5:1: warning: Dereference of null pointer [clang-analyzer-core.NullDereference]',
            '/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include/stdio.h:1:1: warning: x [bugprone-foo]',
        ])
        self.assertEqual(lint.parse_diags(out), [
            ('dm/gfx/nvn/hle_nvn.c', 1695, 39, 'clang-analyzer-core.DivideZero', 'Division by zero'),
            ('dm/host/hle_hid.c', 303, 54, 'bugprone-signed-char-misuse', "'signed char' to 'int' conversion"),
            ('dm/host/hle_hid.c', 303, 54, 'cert-str34-c', "'signed char' to 'int' conversion"),
            ('dm/host/hle_hid.c', 11, 1, 'clang-diagnostic-error', "unknown type name 'foo'"),   # a parse error stays: the file was not fully checked
        ])


class CompileDatabase(unittest.TestCase):
    def test_compile_line(self):
        line = ('clang -O1 -g -Wall -Werror -fsanitize=thread -I../gfx/nvn/include -D DM_X=1 -isysroot /sdk -framework Metal -arch arm64 -c hle_hid.c ../gfx/vk/hle_vk.m '
                '-o dmhost -lz -L/opt/lib -Wl,-dead_strip')
        (e1, e2) = lint.entries(REPO / 'dm/host', line, '/other-sdk')
        self.assertEqual(e1['file'], str(REPO / 'dm/host/hle_hid.c'))
        self.assertEqual(e1['directory'], str(REPO / 'dm/host'))
        self.assertEqual(e1['arguments'], ['clang', '-O1', '-g', '-Wall', '-I../gfx/nvn/include', '-D', 'DM_X=1', '-isysroot', '/sdk', '-c', str(REPO / 'dm/host/hle_hid.c')])
        self.assertEqual(e2['file'], str(REPO / 'dm/gfx/vk/hle_vk.m'))
        self.assertEqual(lint.entries(REPO / 'dm/host', 'ar rcs libx.a a.o', ''), [])
        self.assertEqual(lint.entries(REPO / 'dm/host', 'clang -c x.c', '/sdk')[0]['arguments'][:3], ['clang', '-isysroot', '/sdk'])   # the SDK added when make gives none

    def test_first_command_of_a_source_wins_and_third_party_is_left_out(self):
        def e(f, flag): return {'directory': str(REPO), 'file': str(REPO / f), 'arguments': ['clang', flag, '-c', str(REPO / f)]}
        db = lint.merge_entries([e('dm/host/loader.c', '-DHOST'), e('dm/gfx/vk/hle_vk.m', '-DHOST'), e('dm/host/loader.c', '-DTEST'),
                                 e('dm/third_party/libvpx/vpx_dsp/x.c', '-DHOST'), e('dm/host/gen/profile.c', '-DHOST'), e('dm/host/tests/t_fs.c', '-DTEST')])
        self.assertEqual([(lint.rel(x['file']), x['arguments'][1]) for x in db],
                         [('dm/gfx/vk/hle_vk.m', '-DHOST'), ('dm/host/loader.c', '-DHOST'), ('dm/host/tests/t_fs.c', '-DTEST')])


class Against(unittest.TestCase):
    """--against: the baseline may not grow over the base's, unless LLVM_VERSION was raised."""
    WF = 'env:\n  LLVM_VERSION: {}\n'

    def run_against(self, base, old_baseline, old_llvm='23.1.4', new_llvm='23.1.4'):
        def git(*a):
            path = a[1].split(':', 1)[1]
            text = old_baseline if path.endswith('.tsv') else self.WF.format(old_llvm)
            return mock.Mock(returncode=0 if text is not None else 128, stdout=text or '', stderr='')
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'wf.yml').write_text(self.WF.format(new_llvm))
            with mock.patch.multiple(lint, git=git, REPO=Path(d), BASELINE=Path(d) / 'b.tsv', WORKFLOW='wf.yml'), contextlib.redirect_stdout(io.StringIO()) as out:
                return lint.against('base', base), out.getvalue()

    def test_same_or_smaller_passes(self):
        self.assertEqual(self.run_against({HID: 2, VK: 1}, BASELINE)[0], 0)
        self.assertEqual(self.run_against({HID: 1}, BASELINE)[0], 0)

    def test_growth_fails(self):
        rc, out = self.run_against({HID: 3, VK: 1}, BASELINE)
        self.assertEqual(rc, 1)
        self.assertIn(f'baseline grew: dm/host/hle_hid.c {HID[1]} 2 -> 3', out)
        self.assertEqual(self.run_against({HID: 2, VK: 1, ('dm/host/hle_os.c', 'x'): 1}, BASELINE)[0], 1)   # a new entry

    def test_raised_llvm_may_rewrite(self):
        self.assertEqual(self.run_against({HID: 9}, BASELINE, new_llvm='24.1.0')[0], 0)
        self.assertEqual(self.run_against({HID: 9}, BASELINE, old_llvm='24.1.0')[0], 1)   # a downgrade does not

    def test_no_baseline_at_base(self):
        self.assertEqual(self.run_against({HID: 9}, None)[0], 0)


class Toolchain(unittest.TestCase):
    def test_ci_downloads_the_llvm_the_baseline_was_made_with(self):
        wf = (REPO / '.github/workflows/lint-c.yml').read_text()
        m = re.search(r'^\s+LLVM_VERSION: (\d+\.\d+\.\d+)$', wf, re.M)
        if m is None: self.fail('lint-c.yml: no LLVM_VERSION')
        self.assertRegex(wf, r'(?m)^\s+LLVM_SHA256: [0-9a-f]{64}$')
        self.assertEqual(lint.baseline_llvm(lint.BASELINE.read_text()), m.group(1),
                         'dm/tools/lint_c_baseline.tsv was made with another LLVM than lint-c.yml downloads: run lint_c.py tidy --update with that release')


if __name__ == '__main__':
    unittest.main()

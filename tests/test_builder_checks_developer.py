"""tools/builder/checks_system.py (S1.2a): command_line_tools, cxx_headers and metal_toolchain, which depend on the
effective developer folder, and the guarantee that no xcrun runs without a valid one."""
import errno
from pathlib import Path
import tempfile
import unittest

from builder_fakes import SELECTION, FakeRunner, fail, ok
from tools.builder.checks import CheckContext, Status, run_checks
from tools.builder.checks_system import SYSTEM_CHECKS, CommandLineTools, CxxHeaders, MetalToolchain
from tools.builder.ports import Outcome

ORDINARY_FAILURES = (Outcome.NOT_FOUND, Outcome.NOT_EXECUTABLE, Outcome.NONZERO, Outcome.TIMEOUT)


def find_metal(folder):
    return ('/usr/bin/env', f'DEVELOPER_DIR={folder}', '/usr/bin/xcrun', '--find', 'metal')


def metal_version(folder, metal):
    return ('/usr/bin/env', f'DEVELOPER_DIR={folder}', metal, '--version')


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.clt = self.developer_dir('CommandLineTools')
        self.apps = self.tmp / 'Applications'
        self.apps.mkdir()

    def developer_dir(self, name):
        folder = self.tmp / name
        (folder / 'usr/bin').mkdir(parents=True)
        return folder

    def xcode(self, name='Xcode.app'):
        folder = self.apps / name / 'Contents/Developer'
        (folder / 'usr/bin').mkdir(parents=True)
        return folder

    def ctx(self, runner, environ=None):
        return CheckContext(runner=runner, environ=environ or {}, applications_dirs=(self.apps,),
                            tool_paths=(), brew_prefix=self.tmp / 'brew', mac_version=lambda: '26.0')


class CommandLineToolsCheck(Base):
    def result(self, selection, environ=None):
        runner = FakeRunner({SELECTION: selection})
        return CommandLineTools().run(self.ctx(runner, environ)), runner

    def test_valid_selection_without_override(self):
        r, runner = self.result(ok(f'{self.clt}\n'))
        self.assertEqual((r.status, r.detail), (Status.OK, f'{self.clt} (from xcode-select)'))
        self.assertEqual(runner.calls, [SELECTION])     # the selection is read without DEVELOPER_DIR

    def test_valid_selection_with_an_invalid_override(self):      # review point 4
        r, _ = self.result(ok(f'{self.clt}\n'), {'DEVELOPER_DIR': str(self.tmp / 'gone')})
        self.assertEqual((r.status, r.message_id, r.fix), (Status.BLOCK, 'developer_dir_override_invalid', 'unset DEVELOPER_DIR'))
        self.assertIn(str(self.clt), r.detail)

    def test_override_to_an_existing_folder_without_usr_bin_is_invalid(self):
        (self.tmp / 'empty').mkdir()
        r, _ = self.result(ok(f'{self.clt}\n'), {'DEVELOPER_DIR': str(self.tmp / 'empty')})
        self.assertEqual(r.message_id, 'developer_dir_override_invalid')

    def test_invalid_selection_with_a_valid_override(self):
        r, _ = self.result(ok('/nowhere\n'), {'DEVELOPER_DIR': str(self.clt)})
        self.assertEqual((r.status, r.detail), (Status.OK, f'{self.clt} (from DEVELOPER_DIR)'))

    def test_empty_override_is_ignored(self):
        r, _ = self.result(ok(f'{self.clt}\n'), {'DEVELOPER_DIR': ''})
        self.assertEqual(r.detail, f'{self.clt} (from xcode-select)')

    def test_nothing_selected(self):
        for answer in (fail(Outcome.NONZERO, code=2), ok('')):
            with self.subTest(answer=answer):
                r, _ = self.result(answer)
                self.assertEqual((r.message_id, r.fix), ('command_line_tools_missing', 'xcode-select --install'))

    def test_selection_without_usr_bin(self):
        (self.tmp / 'odd').mkdir()
        r, _ = self.result(ok(f'{self.tmp / "odd"}\n'))
        self.assertEqual((r.message_id, r.fix), ('developer_dir_selection_invalid', 'sudo xcode-select --reset'))

    def test_runner_failures(self):
        for outcome in (Outcome.NOT_FOUND, Outcome.NOT_EXECUTABLE, Outcome.TIMEOUT):
            with self.subTest(outcome=outcome):
                self.assertEqual(self.result(fail(outcome))[0].message_id, 'command_failed')


class NoXcrunWithoutAValidFolder(Base):
    def test_full_run_never_calls_xcrun_when_command_line_tools_blocks(self):
        cases = [({}, fail(Outcome.NONZERO, code=2)),                                        # nothing selected
                 ({}, ok(f'{self.tmp / "odd"}\n')),                                          # invalid selection
                 ({'DEVELOPER_DIR': str(self.tmp / 'gone')}, ok(f'{self.clt}\n'))]           # invalid override
        for environ, selection in cases:
            with self.subTest(environ=environ, selection=selection):
                runner = FakeRunner({SELECTION: selection, lambda argv: 'sysctl' in argv[0]: ok('1\n')})
                results = {r.id: r for r in run_checks(SYSTEM_CHECKS, self.ctx(runner, environ))}
                self.assertEqual(results['command_line_tools'].status, Status.BLOCK)
                self.assertEqual((results['cxx_headers'].status, results['metal_toolchain'].status), (Status.SKIP, Status.SKIP))
                self.assertEqual(runner.ran('xcrun'), [])


class CxxHeadersCheck(Base):
    def run_with(self, answer):
        seen = {}

        def clangxx(argv):
            probe = Path(argv[-1])
            seen['probe'], seen['text'] = probe, probe.read_text()
            if isinstance(answer, BaseException):
                raise answer
            return answer
        runner = FakeRunner({SELECTION: ok(f'{self.clt}\n'), lambda argv: 'clang++' in argv: clangxx})
        return runner, seen

    def test_ok_and_probe_content(self):
        runner, seen = self.run_with(ok())
        self.assertEqual(CxxHeaders().run(self.ctx(runner)).status, Status.OK)
        self.assertEqual(seen['text'], '#include <string>\n')
        self.assertEqual(runner.ran('clang++')[0][:4], ('/usr/bin/xcrun', 'clang++', '-std=c++11', '-fsyntax-only'))
        self.assertFalse(seen['probe'].exists())

    def test_failure_needs_diagnosis_and_offers_no_command(self):
        runner, _ = self.run_with(fail(Outcome.NONZERO, "fatal error: 'string' file not found"))
        r = CxxHeaders().run(self.ctx(runner))
        self.assertEqual((r.status, r.message_id, r.fix), (Status.BLOCK, 'cxx_headers_needs_diagnosis', None))
        self.assertIn(str(self.clt), r.detail)
        self.assertIn("'string' file not found", r.detail)

    def test_probe_removed_when_the_runner_raises(self):
        runner, seen = self.run_with(OSError(errno.EMFILE, 'Too many open files'))
        with self.assertRaises(OSError):
            CxxHeaders().run(self.ctx(runner))
        self.assertFalse(seen['probe'].exists())

    def test_runner_failures(self):
        for outcome in (Outcome.NOT_FOUND, Outcome.NOT_EXECUTABLE, Outcome.TIMEOUT):
            with self.subTest(outcome=outcome):
                runner, _ = self.run_with(fail(outcome))
                self.assertEqual(CxxHeaders().run(self.ctx(runner)).message_id, 'command_failed')


class MetalToolchainCheck(Base):
    def runner(self, answers, environ_selection=None):
        script = {SELECTION: ok(f'{environ_selection or self.clt}\n')}
        script.update(answers)
        return FakeRunner(script)

    def test_found_in_the_first_candidate_and_later_ones_not_asked(self):
        xcode = self.xcode()
        runner = self.runner({find_metal(self.clt): ok('/m/metal\n'), metal_version(self.clt, '/m/metal'): ok('Apple metal 1\nx\n')})
        r = MetalToolchain().run(self.ctx(runner))
        self.assertEqual((r.status, r.detail), (Status.OK, f'{self.clt}: Apple metal 1'))
        self.assertEqual(runner.ran(str(xcode)), [])

    def test_found_in_an_xcode_in_applications(self):
        xcode = self.xcode()
        runner = self.runner({find_metal(self.clt): fail(Outcome.NONZERO), find_metal(xcode): ok('/x/metal\n'),
                              metal_version(xcode, '/x/metal'): ok('Apple metal 2\n')})
        self.assertEqual(MetalToolchain().run(self.ctx(runner)).detail, f'{xcode}: Apple metal 2')

    def test_every_ordinary_failure_gives_warn_with_the_folders_tried(self):
        xcode = self.xcode()
        for outcome in ORDINARY_FAILURES:
            with self.subTest(outcome=outcome):
                runner = self.runner({find_metal(self.clt): fail(outcome), find_metal(xcode): fail(outcome)})
                r = MetalToolchain().run(self.ctx(runner))
                self.assertEqual((r.status, r.message_id, r.fix),
                                 (Status.WARN, 'metal_toolchain_not_found', 'xcodebuild -downloadComponent MetalToolchain'))
                self.assertIn(f'tried {self.clt}, {xcode}', r.detail)

    def test_found_but_version_fails_moves_on(self):
        runner = self.runner({find_metal(self.clt): ok('/m/metal\n'), metal_version(self.clt, '/m/metal'): fail(Outcome.NONZERO)})
        self.assertEqual(MetalToolchain().run(self.ctx(runner)).status, Status.WARN)

    def test_system_error_is_block_through_run_checks(self):        # review point 3: WARN vs BLOCK
        runner = self.runner({find_metal(self.clt): OSError(errno.EMFILE, 'Too many open files')})
        with self.assertRaises(OSError):
            MetalToolchain().run(self.ctx(runner))
        result = run_checks([CommandLineTools(), MetalToolchain()], self.ctx(runner))[1]
        self.assertEqual((result.status, result.message_id), (Status.BLOCK, 'check_crashed'))

    def test_duplicate_candidate_is_asked_once(self):
        runner = self.runner({find_metal(self.clt): fail(Outcome.NONZERO)})
        MetalToolchain().run(self.ctx(runner, {'DEVELOPER_DIR': str(self.clt)}))
        self.assertEqual(len(runner.ran('--find')), 1)

    def test_candidate_order(self):
        other = self.developer_dir('Other')
        b, a = self.xcode('Xcode-beta.app'), self.xcode('Xcode.app')
        runner = self.runner({})
        # sorted(), as build_app.py's developer_dirs: 'Xcode-beta.app' comes before 'Xcode.app' ('-' < '.')
        self.assertEqual(MetalToolchain.candidates(self.ctx(runner, {'DEVELOPER_DIR': str(other)})), [other, self.clt, b, a])


if __name__ == '__main__':
    unittest.main()

"""tools/builder/checks_system.py (S1.2a): apple_silicon, macos_version, homebrew, brew_packages, git, build_python.
The developer-folder checks are in test_builder_checks_developer.py. A fake runner answers commands; temporary
folders stand in for the tool paths and the Homebrew prefix."""
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from builder_fakes import FakeRunner, fail, ok
from tools.builder import checks_system
from tools.builder.checks import CheckContext, Status
from tools.builder.checks_system import (BREW_INSTALL, AppleSilicon, BrewPackages, BuildPython, Git, Homebrew,
                                         MacosVersion)
from tools.builder.ports import Outcome

SYSCTL = ('/usr/sbin/sysctl', '-n', 'hw.optional.arm64')
RUNNER_FAILURES = (Outcome.NOT_FOUND, Outcome.NOT_EXECUTABLE, Outcome.TIMEOUT)


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.bin = self.tmp / 'bin'
        self.brew = self.tmp / 'brew'
        self.bin.mkdir()

    def ctx(self, runner=None, **kw):
        return CheckContext(runner=runner or FakeRunner(), tool_paths=(str(self.bin),), brew_prefix=self.brew,
                            environ={}, applications_dirs=(), **kw)

    def check(self, check, runner=None, **kw):
        r = check.run(self.ctx(runner, **kw))
        return r.status, r.message_id, r.fix

    def executable(self, name):
        path = self.bin / name
        path.write_text('#!/bin/sh\n')
        path.chmod(0o755)
        return path


class AppleSiliconCheck(Base):
    def test_arm64(self):
        self.assertEqual(self.check(AppleSilicon(), FakeRunner({SYSCTL: ok('1\n')})), (Status.OK, 'ok', None))

    def test_intel(self):
        for answer in (ok('0\n'), fail(Outcome.NONZERO, 'unknown oid')):
            with self.subTest(answer=answer):
                self.assertEqual(self.check(AppleSilicon(), FakeRunner({SYSCTL: answer})), (Status.BLOCK, 'intel_mac', None))

    def test_runner_failures(self):
        for outcome in RUNNER_FAILURES:
            with self.subTest(outcome=outcome):
                r = AppleSilicon().run(self.ctx(FakeRunner({SYSCTL: fail(outcome, 'boom')})))
                self.assertEqual((r.status, r.message_id), (Status.BLOCK, 'command_failed'))
                self.assertEqual(r.detail, f'{outcome.name}: boom')


class MacosVersionCheck(Base):
    def test_versions(self):
        cases = {'15.7': 'macos_too_old', '26.0': 'ok', '27.1': 'ok', '': 'macos_unknown', 'x.y': 'macos_unknown'}
        for version, message in cases.items():
            with self.subTest(version=version):
                r = MacosVersion().run(self.ctx(mac_version=lambda: version))
                self.assertEqual(r.message_id, message)
                self.assertEqual(r.status, Status.OK if message == 'ok' else Status.BLOCK)


class HomebrewChecks(Base):
    def test_homebrew(self):
        self.assertEqual(self.check(Homebrew()), (Status.BLOCK, 'homebrew_missing', BREW_INSTALL))
        (self.brew / 'bin').mkdir(parents=True)
        (self.brew / 'bin/brew').write_text('')
        self.assertEqual(self.check(Homebrew()), (Status.OK, 'ok', None))

    def test_packages(self):
        self.assertEqual(self.check(BrewPackages()), (Status.BLOCK, 'brew_packages_missing', 'brew install opus libvpx'))
        (self.brew / 'opt/opus/lib').mkdir(parents=True)
        self.assertEqual(self.check(BrewPackages()), (Status.BLOCK, 'brew_packages_missing', 'brew install libvpx'))
        (self.brew / 'opt/libvpx/lib').mkdir(parents=True)
        self.assertEqual(self.check(BrewPackages()), (Status.OK, 'ok', None))

    def test_a_package_without_lib_folder_is_missing(self):
        (self.brew / 'opt/opus').mkdir(parents=True)
        (self.brew / 'opt/libvpx/lib').mkdir(parents=True)
        self.assertEqual(self.check(BrewPackages())[2], 'brew install opus')


class GitCheck(Base):
    def test_found_in_tool_paths(self):
        git = self.executable('git')
        self.assertEqual(Git().run(self.ctx()).detail, str(git))

    def test_searches_tool_paths_then_usr_bin(self):
        with patch.object(checks_system.shutil, 'which', return_value=None) as which:
            self.assertEqual(self.check(Git()), (Status.BLOCK, 'git_missing', 'xcode-select --install'))
        self.assertEqual(which.call_args.kwargs['path'], os.pathsep.join([str(self.bin), '/usr/bin']))


class BuildPythonCheck(Base):
    def machine(self, python, answer):
        return FakeRunner({(str(python), '-c', 'import platform; print(platform.machine())'): answer})

    def test_missing(self):
        self.assertEqual(self.check(BuildPython()), (Status.BLOCK, 'build_python_missing', 'brew install python@3.14'))

    def test_arm64(self):
        python = self.executable('python3.14')
        r = BuildPython().run(self.ctx(self.machine(python, ok('arm64\n'))))
        self.assertEqual((r.status, r.detail), (Status.OK, str(python)))

    def test_python_3_15_only(self):
        python = self.executable('python3.15')
        self.assertEqual(BuildPython().run(self.ctx(self.machine(python, ok('arm64\n')))).detail, str(python))

    def test_3_14_preferred_over_3_15(self):
        python = self.executable('python3.14')
        self.executable('python3.15')
        self.assertEqual(BuildPython().run(self.ctx(self.machine(python, ok('arm64\n')))).detail, str(python))

    def test_intel_python(self):
        python = self.executable('python3.14')
        r = BuildPython().run(self.ctx(self.machine(python, ok('x86_64\n'))))
        self.assertEqual((r.status, r.message_id, r.detail), (Status.BLOCK, 'python_not_arm64', f'{python}: x86_64'))

    def test_runner_failures_including_nonzero(self):
        python = self.executable('python3.14')
        for outcome in RUNNER_FAILURES + (Outcome.NONZERO,):
            with self.subTest(outcome=outcome):
                r = BuildPython().run(self.ctx(self.machine(python, fail(outcome))))
                self.assertEqual((r.status, r.message_id), (Status.BLOCK, 'command_failed'))


if __name__ == '__main__':
    unittest.main()

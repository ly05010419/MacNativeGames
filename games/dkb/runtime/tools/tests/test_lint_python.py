"""The lint baseline may grow only for a checker whose pinned version increased.
Uses temporary Git trees; no linters, game files or keys needed.
"""
import collections
import contextlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lint_python as lint

REQS = 'ruff==0.17.2\npyright==1.1.414\nlz4==4.4.5\n'
RUFF = ('ruff', 'scripts/example.py', 'F821', 'Undefined name `missing`')
PYRIGHT = ('pyright', 'scripts/example.py', 'reportUndefinedVariable', '"missing" is not defined')


class BaselineGuard(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name).resolve()
        self.baseline = self.root / 'dm/tools/lint_python_baseline.tsv'
        self.baseline.parent.mkdir(parents=True)
        self.reqs = self.root / lint.REQS
        self.reqs.parent.mkdir(parents=True)
        self.reqs.write_text(REQS)
        patch = mock.patch.multiple(lint, REPO=self.root, BASELINE=self.baseline)
        patch.start(); self.addCleanup(patch.stop)
        self.base: collections.Counter[tuple[str, str, str, str]] = collections.Counter({RUFF: 1, PYRIGHT: 1})
        lint.write(self.base)
        self.git('init', '-q')
        self.git('add', '.')
        self.rev = self.git('write-tree')

    def git(self, *args):
        return subprocess.run(['git', *args], cwd=self.root, capture_output=True, text=True, check=True).stdout.strip()

    def compare(self, base):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            rc = lint.against(self.rev, base)
        return rc, out.getvalue()

    def grown(self, tool):
        base = self.base.copy()
        base[RUFF if tool == 'ruff' else PYRIGHT] += 1
        return base

    def test_unchanged_and_shrunk_baselines_pass(self):
        for base in (self.base, collections.Counter({RUFF: 1}), collections.Counter()):
            with self.subTest(base=base): self.assertEqual(self.compare(base)[0], 0)

    def test_new_entry_and_increased_count_fail_without_upgrade(self):
        added = self.base.copy()
        added[('ruff', 'scripts/another.py', 'F821', 'Undefined name `missing`')] = 1
        for base in (added, self.grown('ruff'), self.grown('pyright')):
            with self.subTest(base=base): self.assertEqual(self.compare(base)[0], 1)

    def test_comments_whitespace_and_dependencies_do_not_allow_growth(self):
        edits = (REQS + '# comment only\n', REQS.replace('ruff==', 'ruff == ') + '\n',
                 REQS.replace('4.4.5', '4.4.6'), REQS + 'capstone==5.0.9\n')
        for reqs in edits:
            self.reqs.write_text(reqs)
            with self.subTest(reqs=reqs): self.assertEqual(self.compare(self.base)[0], 0)
            for tool in ('ruff', 'pyright'):
                with self.subTest(reqs=reqs, tool=tool): self.assertEqual(self.compare(self.grown(tool))[0], 1)

    def test_downgrades_do_not_allow_growth(self):
        for old, new in (('0.17.2', '0.17.1'), ('1.1.414', '1.1.413')):
            self.reqs.write_text(REQS.replace(old, new))
            with self.subTest(version=new): self.assertEqual(self.compare(self.base)[0], 0)
            for tool in ('ruff', 'pyright'):
                with self.subTest(version=new, tool=tool): self.assertEqual(self.compare(self.grown(tool))[0], 1)

    def test_upgrade_allows_only_that_checkers_growth(self):
        for upgraded, old, new, other in (('ruff', '0.17.2', '0.18.0', 'pyright'), ('pyright', '1.1.414', '1.1.415', 'ruff')):
            self.reqs.write_text(REQS.replace(old, new))
            with self.subTest(tool=upgraded):
                self.assertEqual(self.compare(self.grown(upgraded))[0], 0)
                self.assertEqual(self.compare(self.grown(other))[0], 1)
                both = self.grown(upgraded); both.update({RUFF if other == 'ruff' else PYRIGHT: 1})
                self.assertEqual(self.compare(both)[0], 1)

    def test_version_comparison_is_numeric(self):
        self.reqs.write_text(REQS.replace('0.17.2', '0.9.0'))
        self.assertEqual(self.compare(self.grown('ruff'))[0], 1)
        self.reqs.write_text(REQS.replace('0.17.2', '0.100.0'))
        self.assertEqual(self.compare(self.grown('ruff'))[0], 0)

    def test_both_upgrades_allow_both_checkers_growth(self):
        self.reqs.write_text(REQS.replace('0.17.2', '0.18.0').replace('1.1.414', '1.1.415'))
        self.assertEqual(self.compare(collections.Counter({RUFF: 2, PYRIGHT: 2}))[0], 0)

    def test_missing_or_invalid_pins_fail_closed(self):
        edits = (REQS.replace('ruff==0.17.2\n', ''), REQS.replace('ruff==0.17.2', 'ruff>=0.18.0'),
                 REQS.replace('0.17.2', 'invalid'), REQS + 'ruff==0.18.0\n')
        for reqs in edits:
            self.reqs.write_text(reqs)
            with self.subTest(reqs=reqs): self.assertEqual(self.compare(self.grown('ruff'))[0], 1)

    def test_missing_base_requirements_fail_closed(self):
        self.git('rm', '--cached', '-q', lint.REQS)
        self.rev = self.git('write-tree')
        self.reqs.write_text(REQS)
        self.assertEqual(self.compare(self.grown('ruff'))[0], 1)

    def test_first_baseline_needs_no_previous_requirements(self):
        self.git('rm', '--cached', '-q', lint.rel(self.baseline), lint.REQS)
        self.rev = self.git('write-tree')
        self.reqs.write_text(REQS)
        self.assertEqual(self.compare(self.base)[0], 0)


if __name__ == '__main__':
    unittest.main()

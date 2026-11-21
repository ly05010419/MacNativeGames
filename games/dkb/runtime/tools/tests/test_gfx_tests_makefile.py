"""dm/gfx/tests keeps no hand-kept lists: `make run` runs every t_*.c / t_*.m test but those its Makefile's NOT_RUN names, and .gitignore ignores the tests' binaries by
pattern. This checks what those two rely on: every test source has a build rule (or `make run` stops at it), NOT_RUN names only sources that exist, the binary every build rule
makes (tests and tools) and its .dSYM are ignored, and no tracked file of the folder is. No compiler, GPU or game files needed."""
from pathlib import Path
import re
import subprocess
import unittest

REPO = Path(__file__).resolve().parents[3]
TESTS = REPO / 'dm/gfx/tests'


def makefile():
    return (TESTS / 'Makefile').read_text()


def rules():   # the test binaries the Makefile has build rules for
    return set(re.findall(r'^(t_[a-z0-9_]+):', makefile(), re.M))


def built():   # every binary the Makefile compiles from a source here: the tests and the tools beside them (nvn_replay)
    return set(re.findall(r'^([a-z][a-z0-9_]*):\s*\S+\.[cm]\b', makefile(), re.M)) | rules()


def not_run():
    m = re.search(r'^NOT_RUN\s*=\s*(.*)$', makefile(), re.M)
    return m.group(1).split() if m else []


def sources():
    return sorted({p.stem for p in TESTS.glob('t_*.c')} | {p.stem for p in TESTS.glob('t_*.m')})


def ignored(paths):   # the paths .gitignore ignores, tracked or not
    r = subprocess.run(['git', 'check-ignore', '--no-index', '--stdin'], cwd=REPO, input='\n'.join(paths) + '\n', capture_output=True, text=True)
    return set(r.stdout.split())


class RunList(unittest.TestCase):
    def test_every_test_in_the_run_list_has_a_build_rule(self):
        missing = [s for s in sources() if s not in not_run() and s not in rules()]
        self.assertEqual(missing, [], 'make run runs these, but the Makefile has no build rule for them (add one, or name them in NOT_RUN with the reason)')

    def test_not_run_names_existing_sources(self):
        self.assertTrue(not_run(), 'no NOT_RUN line in dm/gfx/tests/Makefile')
        gone = [t for t in not_run() if t not in sources()]
        self.assertEqual(gone, [], 'NOT_RUN names tests that have no source any more')

    def test_the_run_target_takes_the_list_from_the_sources(self):
        self.assertRegex(makefile(), r'(?m)^RUN_TESTS\s*=.*\$\(wildcard t_\*\.c t_\*\.m\)', 'the run list is not taken from the test sources')
        self.assertNotRegex(makefile(), r'(?m)^run: t_', 'a hand-kept run list is back in dm/gfx/tests/Makefile')


class Ignore(unittest.TestCase):
    def test_binaries_and_debug_symbols_are_ignored(self):
        want = [f'dm/gfx/tests/{t}' for t in sorted(built())] + [f'dm/gfx/tests/{t}.dSYM/' for t in sorted(built())]
        got = ignored(want)
        self.assertEqual([p for p in want if p.rstrip('/') not in {g.rstrip('/') for g in got}], [], 'not ignored by dm/gfx/tests/.gitignore')

    def test_no_tracked_file_is_ignored(self):
        tracked = subprocess.run(['git', 'ls-files', 'dm/gfx/tests'], cwd=REPO, capture_output=True, text=True, check=True).stdout.split()
        self.assertTrue(tracked)
        self.assertEqual(sorted(ignored(tracked)), [], 'tracked files that dm/gfx/tests/.gitignore would ignore')


if __name__ == '__main__':
    unittest.main()

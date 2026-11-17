"""Include TMNT regressions in ordinary repository unittest discovery, in a process of their own (after PR #2125).

They import their own scripts by module name (extract_exefs, package_private, ...), as Tears of the Kingdom's tests do with
theirs: loaded in the same process, the first copy wins (tests/test_game_containers.py loads games/totk/scripts/extract_exefs,
then games/tmnt/tests/test_package_selection.py's `from extract_exefs import TITLE` fails). So the root discovery runs
`unittest discover -s games/tmnt/tests` as a child and passes when it does, with its output on failure."""
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TmntTests(unittest.TestCase):
    def test_tmnt_regressions_pass_in_their_own_process(self):
        tests = ROOT / 'games/tmnt/tests'
        run = subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', str(tests), '-t', str(tests)],
                             cwd=ROOT, capture_output=True, text=True, timeout=900)
        self.assertEqual(run.returncode, 0, run.stdout[-4000:] + run.stderr[-8000:])
        self.assertRegex(run.stderr, r'Ran [1-9]\d* tests?')   # something ran, not an empty discovery

    def test_the_same_names_clash_in_one_process(self):
        """Why the child: both games ship an extract_exefs module, and only TMNT's has these names."""
        totk = (ROOT / 'games/totk/scripts/extract_exefs.py').read_text()
        tmnt = (ROOT / 'games/tmnt/scripts/extract_exefs.py').read_text()
        self.assertIn('def validate_private_output', tmnt)
        self.assertNotIn('def validate_private_output', totk)


if __name__ == '__main__':
    unittest.main()

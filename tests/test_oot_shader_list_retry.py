"""games/oot/scripts/shader_list.py: a warm row refused because its bind state was unknown goes in once a later source of
the same merge brings that bind state (2026-11-11: 202 inbox rows were refused before the issues' V rows came in)."""
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'games/oot/scripts/shader_list.py'
spec = importlib.util.spec_from_file_location('oot_shader_list_retry', SCRIPT)
sl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sl)

KEY = 'ab' * 32
DEPS = '{"tex":{"0":"sampler2D"}}'


class RetryRefused(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.suffix = sl.answers_suffix(sl.listed_answers(DEPS))
        self.assertNotEqual(self.suffix, sl.DEFAULT_SUFFIX, 'the test needs a bind state other than the default')
        self.warm = f'C\t{KEY}-{self.suffix}_b\n'
        self.vrow = f'V\t{KEY}\t{DEPS}\n'
        self.list = self.tmp / 'shader_list.tsv'
        self.list.write_text(sl.HEADERS['oot'])

    def source(self, name, text):
        path = self.tmp / name
        path.write_text(text)
        return path

    def test_a_later_source_brings_the_bind_state(self):
        lst = sl.ShaderList.load(self.list)
        lst.add_rows(self.source('first.txt', self.warm), 'm')
        self.assertEqual((lst.refused, len(lst.warm)), (1, 0))
        lst.add_rows(self.source('second.txt', self.vrow), 'm')
        self.assertEqual(lst.retry_refused(), 1)
        self.assertEqual(lst.refused, 0)
        self.assertIn(('C', f'{KEY}-{self.suffix}_b'), lst.warm)

    def test_without_the_bind_state_it_stays_out(self):
        lst = sl.ShaderList.load(self.list)
        lst.add_rows(self.source('first.txt', self.warm), 'm')
        self.assertEqual(lst.retry_refused(), 0)
        self.assertEqual((lst.refused, len(lst.warm)), (1, 0))

    def test_the_merge_command_tries_again_after_every_source(self):
        run = subprocess.run([sys.executable, str(SCRIPT), 'merge', str(self.list),
                              str(self.source('first.txt', self.warm)), str(self.source('second.txt', self.vrow))],
                             capture_output=True, text=True, check=True)
        self.assertNotIn('refused', run.stdout)
        self.assertIn(f'C\t{KEY}-{self.suffix}_b', self.list.read_text())


if __name__ == '__main__':
    unittest.main()

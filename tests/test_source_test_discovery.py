"""Importing a shared fixture must not redirect unrelated test discovery."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SourceTestDiscovery(unittest.TestCase):
    def test_mesh_fixture_does_not_shadow_root_shader_seed_tests(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tests = root / 'tests'
            fixtures = root / 'games/oot/tests'
            tests.mkdir()
            fixtures.mkdir(parents=True)
            shutil.copy2(ROOT / 'tests/test_oot_mesh_output_layout.py', tests)
            shutil.copy2(ROOT / 'games/oot/tests/ampere_scoreboard_gpu.py', fixtures)
            for directory, name in ((tests, 'RootSeed'), (fixtures, 'GameSeed')):
                (directory / 'test_shader_seed.py').write_text(
                    f'import unittest\nclass {name}(unittest.TestCase):\n'
                    '    def test_selected(self): pass\n')
            probe = '''
from pathlib import Path
import sys
import unittest
root = Path(sys.argv[1])
suite = unittest.TestLoader().discover(str(root / 'tests'))
assert suite.countTestCases() == 2, suite
assert Path(sys.modules['test_shader_seed'].__file__) == root / 'tests/test_shader_seed.py'
assert str(root / 'games/oot/tests') not in sys.path
from test_oot_mesh_output_layout import inst, mov
assert len(inst(0x802)) == 16
assert mov(1, 2) == inst(0x802, 1, b=2, c=0xf00)
print('source discovery: root shader seed and mesh fixture preserved')
'''
            result = subprocess.run([sys.executable, '-I', '-c', probe, str(root)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('root shader seed and mesh fixture preserved', result.stdout)


if __name__ == '__main__':
    unittest.main()

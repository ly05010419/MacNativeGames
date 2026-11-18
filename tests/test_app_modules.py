"""build_app.py app_modules: an app carries the rewritten modules its version's verified values name, never a fixed list
(2026-11-12: Bananza has no subsdk0, and its app build stopped at copying one)."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_app', ROOT / 'build_app.py')
build_app = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_app)


class AppModules(unittest.TestCase):
    def test_bananza_has_no_subsdk0(self):
        self.assertEqual(build_app.app_modules('dkb'), ['rewritten/dkb/main.patched.bin', 'rewritten/dkb/sdk.patched.bin'])

    def test_every_version_carries_what_its_values_name(self):
        for version, expected in build_app.VERSIONS.items():
            with self.subTest(version=version):
                self.assertEqual(sorted(build_app.app_modules(version)),
                                 sorted(f'rewritten/{version}/{n}' for n in expected['rewritten_sha256']))

    def test_tears_of_the_kingdom_keeps_its_three(self):
        self.assertEqual([m.rsplit('/', 1)[1] for m in build_app.app_modules('v143')],
                         ['main.patched.bin', 'sdk.patched.bin', 'subsdk0.patched.bin'])


if __name__ == '__main__':
    unittest.main()

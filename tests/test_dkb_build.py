"""Donkey Kong Bananza target (games/dkb, Q150): the nxnative copy matches its import manifest, carries no other game's profile,
its local changes are exactly the patch, and build_dkb.py refuses inputs that would put game content into the checkout."""
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/dkb'
MANIFEST = GAME / 'third_party/nxnative/import-manifest.json'
BUILD = GAME / 'scripts/build_dkb.py'


def mismatches(game, manifest):
    """Files whose bytes differ from the manifest, files the manifest lists that are missing, and copied files it does not list."""
    listed = {e['destination']: e['sha256'] for e in manifest['files']}
    bad = [d for d, h in listed.items() if not (game / d).is_file() or hashlib.sha256((game / d).read_bytes()).hexdigest() != h]
    on_disk = {str(p.relative_to(game)) for base in ('runtime', 'third_party/nxnative') for p in (game / base).rglob('*')
               if p.is_file() and '__pycache__' not in p.parts}
    extra = sorted(on_disk - set(listed) - {'third_party/nxnative/import-manifest.json', 'third_party/nxnative/local-changes.patch'})
    return bad, extra


class ImportTest(unittest.TestCase):
    def setUp(self):
        self.m = json.loads(MANIFEST.read_text())

    def test_copy_matches_manifest(self):
        bad, extra = mismatches(GAME, self.m)
        self.assertEqual(bad, [])
        self.assertEqual(extra, [])
        self.assertGreater(len(self.m['files']), 1000)   # the whole dm/ tree, not a stub

    def test_tampered_copy_is_found(self):
        """Positive control for the check above: one changed byte in a copy is reported."""
        with tempfile.TemporaryDirectory() as t:
            copy = Path(t) / 'dkb'
            shutil.copytree(GAME / 'third_party', copy / 'third_party')
            (copy / 'runtime').mkdir()
            e = next(e for e in self.m['files'] if e['destination'].startswith('third_party/'))
            p = copy / e['destination']
            p.write_bytes(p.read_bytes() + b'x')
            bad, _ = mismatches(copy, {'files': [e]})
            self.assertEqual(bad, [e['destination']])

    def test_only_this_game_profile(self):
        profiles = sorted(p.name for p in (GAME / 'runtime/games').iterdir() if p.is_dir() and len(p.name) == 16)
        self.assertEqual(profiles, ['040074A01BF12000'])
        self.assertGreater(len(self.m['left_out_other_profiles']), 0)

    def test_local_changes_are_the_patch(self):
        changed = sorted(e['source'] for e in self.m['files'] if e['changed_here'])
        patch = (GAME / 'third_party/nxnative/local-changes.patch').read_text()
        in_patch = sorted(line.split(' b/')[1] for line in patch.splitlines() if line.startswith('diff --git '))
        self.assertEqual(changed, in_patch)

    def test_converter_matches_the_chain_lock(self):
        """The carried SPIR-V to Metal converter is the one the shader chain was fingerprinted with (another one is another chain)."""
        exe = GAME / 'runtime/gfx/tools/spirv_cross_bin/spirv-cross'
        want = next(l.split('\t')[1] for l in (GAME / 'runtime/gfx/tools/dmsh_chain.lock').read_text().splitlines()
                    if l.startswith('spirv-cross\t'))
        self.assertEqual(hashlib.sha256(exe.read_bytes()).hexdigest()[:len(want)], want)
        self.assertTrue(exe.stat().st_mode & 0o111)
        self.assertTrue((GAME / 'runtime/gfx/tools/spirv_cross_bin/LICENSE').is_file())


class BuildGuardTest(unittest.TestCase):
    def run_build(self, *args):
        return subprocess.run([sys.executable, str(BUILD), *args], capture_output=True, text=True)

    def test_output_inside_checkout_refused(self):
        with tempfile.TemporaryDirectory() as t:
            (Path(t) / 'base').mkdir()
            r = self.run_build(t, '--work-dir', str(Path(t) / 'w'), '--output', str(ROOT / 'x.app'))
            self.assertEqual(r.returncode, 2)
            self.assertIn('outside the checkout', r.stderr)

    def test_dump_without_base_refused(self):
        with tempfile.TemporaryDirectory() as t:
            r = self.run_build(t, '--work-dir', str(Path(t) / 'w'), '--output', str(Path(t) / 'out.app'))
            self.assertEqual(r.returncode, 2)
            self.assertIn('no base/', r.stderr)

    def test_existing_output_refused(self):
        with tempfile.TemporaryDirectory() as t:
            (Path(t) / 'base').mkdir()
            (Path(t) / 'out.app').mkdir()
            r = self.run_build(t, '--work-dir', str(Path(t) / 'w'), '--output', str(Path(t) / 'out.app'))
            self.assertEqual(r.returncode, 2)
            self.assertIn('already exists', r.stderr)


if __name__ == '__main__':
    unittest.main()

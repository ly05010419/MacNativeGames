"""Settings schema 2 (picture v2): fresh defaults, env mapping, compatibility picture, v1 migration,
capability gates for 4K, restart/pending/unavailable adapter results, corruption recovery and agent isolation.

The Objective-C probe is linked with the real runtime/settings.m and stub adapter functions, so no game data
or GPU is involved.
"""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
LOCKED_ENV = ('TOTK_OUTPUT_RES', 'TOTK_MFX_UPSCALE', 'TOTK_MFX_OUTPUT', 'TOTK_RENDER_RES',
              'TOTK_SLOT_OWNER', 'TOTK_SETTINGS_DIR', 'TOTK_ROMFS_PATCHES_OUT',
              'PROBE_MODE_RESULT', 'PROBE_EXPECT_OUTPUT', 'PROBE_EXPECT_NATIVE4K',
              'PROBE_EXPECT_MFX4K', 'PROBE_EXPECT_UPSCALER', 'PROBE_EXPECT_MFX',
              'PROBE_EXPECT_CAP')
RES_1080, RES_1440, RES_2160 = 1080, 1440, 2160
UPSCALE_NATIVE, UPSCALE_METALFX = 0, 1


@unittest.skipUnless(sys.platform == 'darwin', 'settings.m uses Foundation')
class SettingsSchemaV2(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.root = Path(cls.directory.name)
        compiler = shutil.which('clang')
        if not compiler:
            raise unittest.SkipTest('clang is required')
        cls.probe = cls.root / 'settings-v2-probe'
        command = [compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-Wno-deprecated-declarations', '-fobjc-arc',
                   '-I' + str(ROOT / 'games/totk/runtime'),
                   str(ROOT / 'tests/settings_v2_probe.m'),
                   str(ROOT / 'games/totk/runtime/settings.m'),
                   '-framework', 'Foundation', '-o', str(cls.probe)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if build.returncode != 0:
            raise AssertionError(build.stdout + build.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def env(self, directory, extra=None):
        env = {k: v for k, v in os.environ.items() if k not in LOCKED_ENV}
        env['TOTK_SETTINGS_DIR'] = str(directory)
        env['TOTK_ROMFS_PATCHES_OUT'] = str(directory / 'overlays')
        (directory / 'overlays').mkdir(exist_ok=True)
        if extra:
            env.update(extra)
        return env

    def run_probe(self, action, directory, extra=None):
        run = subprocess.run([str(self.probe), action], env=self.env(directory, extra),
                             capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0,
                         'action %s failed:\n%s\n%s' % (action, run.stdout, run.stderr))
        self.assertIn('pass:', run.stdout)
        return run.stdout

    @staticmethod
    def write_settings(directory, document):
        (directory / 'settings.json').write_text(json.dumps(document))

    @classmethod
    def write_v1(cls, directory):
        cls.write_settings(directory, {
            'schemaVersion': 1,
            'settings': {'picture': 1, 'interp': 0},
        })
        (directory / 'keymaps.json').write_text(json.dumps({
            'schemaVersion': 1,
            'activeProfile': 0,
            'profiles': [{'id': 'legacy', 'name': 'Legacy', 'builtin': 1, 'keys': {'0': 1}}],
        }))

    @staticmethod
    def overlay(directory, tier):
        path = directory / 'overlays' / tier / 'manifest.tsv'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('schema\t1\n')

    def test_fresh_and_compat(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.run_probe('fresh', Path(tmp))
            self.run_probe('compat', Path(tmp))

    def test_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.run_probe('select', Path(tmp))
            # A second process must read the persisted v2 triple back.
            self.run_probe('verify', Path(tmp))

    def test_v1_migration(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.write_v1(directory)
            self.run_probe('migrate', directory)

    def test_env_lock(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.write_v1(directory)
            self.run_probe('envlock', directory, {
                'TOTK_OUTPUT_RES': '3840x2160',
                'TOTK_MFX_UPSCALE': '0',
                'TOTK_MFX_OUTPUT': '0',
                'TOTK_RENDER_RES': '1920x1080',
            })

    def test_restart_pending_unavailable(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.overlay(directory, '3840x2160')
            self.run_probe('restart', directory)
        for action in ('pending', 'unavailable'):
            with self.subTest(action=action), tempfile.TemporaryDirectory() as tmp:
                self.run_probe(action, Path(tmp))

    def test_capability_gates(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            absent = {'PROBE_EXPECT_NATIVE4K': '0', 'PROBE_EXPECT_MFX4K': '0'}
            self.run_probe('capable', directory, absent)
            self.overlay(directory, '2560x1440')
            self.run_probe('capable', directory, absent)
            self.overlay(directory, '3840x2160')
            # Both 4K modes are prepared by the next start from the overlay;
            # they do not require this 2K process to already be at 4K.
            self.run_probe('capable', directory, {
                'PROBE_EXPECT_NATIVE4K': '1', 'PROBE_EXPECT_MFX4K': '1'})

    def test_native_4k_incapable_is_not_persisted(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.run_probe('native-incapable', Path(tmp))

    def test_mfx_4k_gate(self):
        # Without the 4K overlay the mode is unavailable and not persisted.
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.run_probe('mfx4k', directory, {'PROBE_EXPECT_MFX4K': '0'})
        # With it, a 2K process reports restart and persists the 4K start
        # environment; a 4K process then applies it live.
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.overlay(directory, '3840x2160')
            self.run_probe('mfx4k', directory, {'PROBE_EXPECT_MFX4K': '1'})

    def test_4k_startup_fallback_and_restore(self):
        def stored(directory, upscaler):
            self.write_settings(directory, {
                'schemaVersion': 2,
                'settings': {'outputRes': RES_2160, 'upscaler': upscaler},
            })

        # outputRes stays 2160 in every case; only the startup environment
        # changes.  A 4K MetalFX start is TOTK_OUTPUT_RES=3840x2160 with both
        # MetalFX flags; a missing overlay falls back to a native start.
        cases = (
            (UPSCALE_NATIVE, None, '0', '0', '0'),
            (UPSCALE_NATIVE, '2560x1440', '2560x1440', '0', '0'),
            (UPSCALE_NATIVE, '3840x2160', '3840x2160', '0', '1'),
            (UPSCALE_METALFX, None, '0', '0', '0'),
            (UPSCALE_METALFX, '2560x1440', '2560x1440', '0', '0'),
            (UPSCALE_METALFX, '3840x2160', '3840x2160', '1', '1'),
        )
        for upscaler, tier, expected, mfx, capable in cases:
            with self.subTest(upscaler=upscaler, tier=tier), tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                stored(directory, upscaler)
                if tier:
                    self.overlay(directory, tier)
                self.run_probe('fallback', directory, {
                    'PROBE_EXPECT_OUTPUT': expected,
                    'PROBE_EXPECT_MFX': mfx,
                    'PROBE_EXPECT_UPSCALER': str(upscaler),
                    'PROBE_EXPECT_CAP': capable,
                })

    def test_legacy_compact_output_res_encoding(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.write_settings(directory, {
                'schemaVersion': 2,
                'settings': {'outputRes': 2, 'upscaler': UPSCALE_NATIVE},
            })
            self.run_probe('legacy', directory)
            document = json.loads((directory / 'settings.json').read_text())
            self.assertEqual(document['settings']['outputRes'], RES_2160)

    def test_corrupt_settings_restores_backup(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'settings.json').write_text('{not json')
            (directory / 'settings.json.bak').write_text(json.dumps({
                'schemaVersion': 1,
                'settings': {'picture': 2},
            }))
            self.run_probe('corrupt', directory)
            corrupt = list(directory.glob('settings.json.corrupt-*'))
            self.assertTrue(corrupt, 'the unreadable main file must be archived')
            document = json.loads((directory / 'settings.json').read_text())
            self.assertEqual(document['schemaVersion'], 2)
            self.assertEqual(document['settings']['outputRes'], RES_1440)
            self.assertEqual(document['settings']['upscaler'], UPSCALE_NATIVE)

    def test_agent_isolation(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.run_probe('isolation', directory, {'TOTK_SLOT_OWNER': 'probe-agent'})
            self.assertTrue((directory / 'agent-settings/probe-agent/settings.json').is_file())
            self.assertFalse((directory / 'settings.json').exists())


if __name__ == '__main__':
    unittest.main()

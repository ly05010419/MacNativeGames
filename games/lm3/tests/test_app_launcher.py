"""Exercise the actual native launcher with an isolated mock runtime."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'native Mac app launcher')
class AppLauncher(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='lm3-launcher-build-')
        cls.executable = Path(cls.work.name) / 'LM3'
        subprocess.run(['xcrun', 'clang', '-O2', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'build'), str(ROOT / 'scripts/lm3_app_launcher.m'),
                        '-framework', 'Foundation', '-o', str(cls.executable)],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='lm3 portable 中文 ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.app = self.root / '游戏 Portable.app'
        self.game = self.app / 'Contents/Resources/game'
        self.launcher = self.app / 'Contents/MacOS/LM3'
        self.launcher.parent.mkdir(parents=True)
        shutil.copy2(self.executable, self.launcher)
        self.native = self.game / 'build/lm3-native'
        self.native.parent.mkdir(parents=True)
        self.native.write_text('#!' + sys.executable + '\n' +
            'import json, os, sys\n' +
            'print(json.dumps({"cwd":os.getcwd(),"argv":sys.argv[1:],"env":dict(os.environ)}))\n')
        self.native.chmod(0o755)
        for relative in ('build/shader-service/totk_xlate', 'build/spirv-cross/spirv-cross'):
            tool = self.game / relative
            tool.parent.mkdir(parents=True)
            tool.write_text('#!/bin/sh\nexit 0\n')
            tool.chmod(0o755)
        self.data = self.root / '新电脑数据'
        self.logs = self.root / '验证日志'
        self.env = dict(os.environ, LM3_APP_DATA_DIR=str(self.data), LM3_APP_LOG_DIR=str(self.logs),
                        LM3_APP_OWNER='codex')

    def run_launcher(self, *args):
        return subprocess.run([str(self.launcher), *args], cwd='/', env=self.env,
                              capture_output=True, text=True, timeout=5)

    def payload(self):
        lines = (self.logs / 'lm3.log').read_text().splitlines()
        return json.loads(lines[-1])

    def test_relocated_app_routes_resources_and_creates_empty_profile(self):
        self.assertEqual(self.run_launcher('--mode', 'probe').returncode, 0)
        result = self.payload()
        self.assertEqual(result['cwd'], str(self.game))
        self.assertEqual(result['argv'], ['rewritten/v140', 'probe'])
        env = result['env']
        self.assertEqual(env['TOTK_PROFILE_DIR'], str(self.data / 'profile'))
        self.assertEqual(env['TOTK_ROMFS'], str(self.game / 'extracted/v140/romfs'))
        self.assertEqual(env['TOTK_SHARED_FONTS'], str(self.game / 'extracted/firmware/shared-fonts'))
        self.assertEqual(env['TOTK_SHADER_SEED_DIR'], str(self.game / 'build/shader-seed'))
        self.assertEqual(env['TOTK_SHADER_SERVICE'], 'native')
        for name in ('TOTK_SHADER_CACHE', 'TOTK_GAME_SLOT_DIR', 'TOTK_ROMFS_PROOF_DIR', 'DM_MSL_DIR'):
            self.assertTrue(env[name].startswith(str(self.data) + '/'), name)
        self.assertEqual(list((self.data / 'profile').iterdir()), [])
        self.assertFalse((self.game / 'analysis').exists())
        self.assertFalse((self.game / 'build/shader-cache').exists())

    def test_inherited_development_paths_and_scripts_are_cleared(self):
        self.env.update(TOTK_PROFILE_DIR='/should-not-be-used', TOTK_INPUT_SCRIPT='bad',
                        TOTK_SHADER_XLATE='/not-in-the-app', DM_PSO_MANIFEST='/wrong',
                        TOTK_GAME_SLOT_FD='123', TOTK_HEADLESS='1', LM3_TARGET_FPS='30',
                        LM3_RENDER_SIZE='1280x720', MTL_DEBUG_LAYER='1')
        self.assertEqual(self.run_launcher().returncode, 0)
        env = self.payload()['env']
        for name in ('TOTK_INPUT_SCRIPT', 'TOTK_SHADER_XLATE', 'DM_PSO_MANIFEST', 'TOTK_GAME_SLOT_FD',
                     'TOTK_HEADLESS', 'LM3_TARGET_FPS', 'LM3_RENDER_SIZE', 'MTL_DEBUG_LAYER', 'LM3_APP_DATA_DIR'):
            self.assertNotIn(name, env)

    def test_existing_save_is_preserved_and_logs_rotate(self):
        saved = self.data / 'profile/save/existing.save'
        saved.parent.mkdir(parents=True)
        saved.write_bytes(b'existing player progress')
        self.logs.mkdir()
        (self.logs / 'lm3.log').write_text('previous run')
        self.assertEqual(self.run_launcher().returncode, 0)
        self.assertEqual(saved.read_bytes(), b'existing player progress')
        self.assertEqual((self.logs / 'lm3.previous.log').read_text(), 'previous run')

    def test_user_storage_inside_bundle_or_relative_path_is_rejected(self):
        for directory in (str(self.game / 'bad-data'), 'relative-data'):
            self.env['LM3_APP_DATA_DIR'] = directory
            result = self.run_launcher()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('absolute paths outside the app', result.stderr)
        self.assertFalse((self.game / 'bad-data').exists())

    def test_missing_helper_and_invalid_modes_fail_before_starting(self):
        for mode in ('unknown', 'probe-save', 'probe-input'):
            self.assertNotEqual(self.run_launcher('--mode', mode).returncode, 0)
        (self.game / 'build/shader-service/totk_xlate').unlink()
        result = self.run_launcher()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing executable', result.stderr)


if __name__ == '__main__':
    unittest.main()

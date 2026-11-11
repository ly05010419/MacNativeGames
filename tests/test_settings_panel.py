"""Exercise the actual Mac settings panel (key 1) without game data or GPU work."""
from pathlib import Path
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
# The shader inbox (its own project, beside this one; tests/test_shot_share.py): the 截图分享 page's pictures.
INBOX = Path(os.environ.get('SHADER_INBOX_DIR') or ROOT.parent / 'MacNativeGamesWeb/shader-inbox')
HAS_INBOX = bool(shutil.which('node')) and (INBOX / 'node_modules/@electric-sql/pglite').is_dir()


def build_probe(compiler, executable):
    """tests/settings_panel_probe.m as the base game (Tears of the Kingdom 1.2.1), with ImageIO for the shared pictures."""
    gfx = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
    return subprocess.run([compiler, '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                           '-I' + str(gfx / 'include'), '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                           str(ROOT / 'tests/settings_panel_probe.m'), str(ROOT / 'games/totk/runtime/input_prompts.m'),
                           '-framework', 'CoreText', '-framework', 'CoreGraphics', str(ROOT / 'tests/settings_output_fixture.c'),
                           *[str(gfx / f) for f in ('totk_mfx.m', 'totk_mfx_up.m', 'totk_taa.m', 'fps_overlay.c')],
                           '-framework', 'Cocoa', '-framework', 'Metal', '-framework', 'MetalFX', '-framework', 'QuartzCore',
                           '-framework', 'CoreVideo', '-framework', 'ImageIO', '-o', str(executable)],
                          capture_output=True, text=True, timeout=120)


class LocalInbox:
    """The inbox's test/local-server.js on 127.0.0.1 (in-memory database): `base` is its root."""

    def __enter__(self):
        self.server = subprocess.Popen(['node', str(INBOX / 'test/local-server.js')], cwd=INBOX, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, text=True)
        self.base = f'http://127.0.0.1:{int(self.server.stdout.readline().split()[1])}'
        return self

    def __exit__(self, *exc):
        self.server.stdin.close()
        self.server.wait(timeout=30)
        self.server.stdout.close()

    def name(self, board_id, name=None):
        """PUT /v1/name when name is given (as another player would), else GET: the nickname, or None."""
        if name is None:
            request = urllib.request.Request(f'{self.base}/v1/name?me={board_id}')
        else:
            request = urllib.request.Request(f'{self.base}/v1/name', method='PUT', headers={'content-type': 'application/json'},
                                             data=json.dumps({'board_id': board_id, 'name': name}).encode())
        with urllib.request.urlopen(request, timeout=30) as answer:
            return json.loads(answer.read())['data']['name']


@unittest.skipUnless(sys.platform == 'darwin', 'AppKit requires macOS')
class SettingsPanel(unittest.TestCase):
    def test_vr_rows_use_supported_game_features(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        gfx = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
        with tempfile.TemporaryDirectory() as directory:
            for version in ('TOTK_GAME_V143', 'GAME_BOTW160', 'GAME_BOTW190'):
                executable = Path(directory) / version
                command = [compiler, '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                           '-Wno-deprecated-declarations', '-I' + str(gfx / 'include'),
                           '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                           str(ROOT / 'tests/settings_panel_probe.m'),
                           str(ROOT / 'games/totk/runtime/input_prompts.m'),
                           '-framework', 'CoreText', '-framework', 'CoreGraphics', str(ROOT / 'tests/settings_output_fixture.c'),
                           *[str(gfx / f) for f in ('totk_mfx.m', 'totk_mfx_up.m', 'totk_taa.m', 'fps_overlay.c')],
                           '-framework', 'Cocoa', '-framework', 'Metal', '-framework', 'MetalFX',
                           '-framework', 'QuartzCore', '-framework', 'CoreVideo', '-o', str(executable),
                           '-D' + version + '=1']
                if version.startswith('GAME_BOTW'):
                    command.append('-DGAME_BOTW=1')
                result = subprocess.run(command, capture_output=True, text=True, timeout=90)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(executable), 'vr-rows'], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('pass: supported VR settings rows', result.stdout)

    @unittest.skipUnless(HAS_INBOX, 'needs node and the shader inbox (SHADER_INBOX_DIR) with npm install')
    def test_screenshots_page_with_pictures(self):
        """The 截图分享 page against the inbox's test/local-server.js: pictures shared, listed, shown, deleted.
        SHOTS_TEST_PNG=<path.png> keeps pictures of the panel (-grid, -picture, -zh)."""
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'settings-panel-shots'
            build = build_probe(compiler, executable)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            with LocalInbox() as inbox:
                settings = Path(directory) / 'saved'
                settings.mkdir()
                env = dict(os.environ, TOTK_UI_LANG='en', SETTINGS_TEST_DIR=str(settings), SHOTS_TEST_URL=inbox.base + '/v1/shots')
                run = subprocess.run([str(executable), 'shots'], env=env, capture_output=True, text=True, timeout=120)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr[-3000:])
                self.assertIn('pass: screenshots page with pictures', run.stdout)

    @unittest.skipUnless(HAS_INBOX, 'needs node and the shader inbox (SHADER_INBOX_DIR) with npm install')
    def test_nickname_with_the_server(self):
        """The nickname row (nick_row.h) against the inbox's test/local-server.js: set from the 排行榜 page by the
        controller, listed on the board and the pictures, the same on the 截图分享 page, a taken one refused, cleared.
        NICK_TEST_PNG=<path prefix> keeps pictures of both pages."""
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'settings-panel-nick'
            build = build_probe(compiler, executable)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            with LocalInbox() as inbox:
                other = str(uuid.uuid4())
                self.assertEqual(inbox.name(other, 'Zelda'), 'Zelda')   # another player holds it first
                settings = Path(directory) / 'saved'
                settings.mkdir()
                env = dict(os.environ, TOTK_UI_LANG='en', SETTINGS_TEST_DIR=str(settings), NICK_TEST_URL=inbox.base, NICK_TEST_TAKEN='Zelda')
                run = subprocess.run([str(executable), 'nick'], env=env, capture_output=True, text=True, timeout=180)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr[-3000:])
                self.assertIn('pass: nickname with the server', run.stdout)
                self.assertIn('[name] the nickname was not set (409): this nickname is taken', run.stderr)
                me = (settings / 'nick/leaderboard-id.txt').read_text().strip()   # the board's id, in the folder above the game's
                self.assertIsNone(inbox.name(me))       # cleared at the end
                self.assertEqual(inbox.name(other), 'Zelda')
                for switch in ('leaderboard.txt', 'screenshot-share.txt'):   # a nickname turns on no upload switch
                    self.assertFalse((settings / 'nick/TOTK' / switch).exists(), switch)

    @unittest.skipUnless(HAS_INBOX, 'needs node and the shader inbox (SHADER_INBOX_DIR) with npm install')
    def test_contribution_list_with_the_server(self):
        """The 着色器贡献 page's list against the inbox's test/local-server.js (user 2026-11-11): two players' rows sent,
        this Mac's under the install id the probe writes, the other with more; both listed, most first, this Mac's
        marked. CONTRIB_TEST_PNG=<path> keeps a picture of the page."""
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'settings-panel-contrib'
            build = build_probe(compiler, executable)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            with LocalInbox() as inbox:
                mine, other = '0f0f0f0f-0000-4000-8000-000000001234', str(uuid.uuid4())
                for install, count in ((mine, 3), (other, 7)):
                    rows = [f'V\t{(install[:8] + str(k)).ljust(64, "0")}\t{{"attr":{{}},"fout":{{}},"img":{{}},"tex":{{}}}}' for k in range(count)]
                    body = {'schema': 1, 'game': 'totk', 'game_version': '1.2.1', 'app_build': 'test', 'rows': rows,
                            'install': install, 'platform': 'mac', 'device': 'Mac16,1', 'os': 'macOS 26.0'}
                    request = urllib.request.Request(f'{inbox.base}/v1/contrib', method='POST', headers={'content-type': 'application/json'},
                                                     data=json.dumps(body).encode())
                    with urllib.request.urlopen(request, timeout=30) as answer:
                        self.assertEqual(answer.status, 200)
                settings = Path(directory) / 'saved'
                settings.mkdir()
                env = dict(os.environ, TOTK_UI_LANG='en', SETTINGS_TEST_DIR=str(settings), CONTRIB_TEST_URL=inbox.base)
                run = subprocess.run([str(executable), 'contrib'], env=env, capture_output=True, text=True, timeout=120)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr[-3000:])
                self.assertIn('pass: contribution page with the server', run.stdout)

    def test_update_page_never_forks(self):
        self.assertNotIn('fork(', (ROOT / 'games/totk/runtime/update_tab.h').read_text(encoding='utf-8'))   # the game's address space is huge

    def test_current_modes_and_refresh(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-version'], text=True)
        if int(sdk.split('.')[0]) < 26 or int(platform.mac_ver()[0].split('.')[0]) < 26:
            self.skipTest('The production Metal 4 mode implementations require macOS 26 and its SDK')
        gfx = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
        with tempfile.TemporaryDirectory() as directory:
            for version in ('', 'TOTK_GAME_V121', 'TOTK_GAME_V143', 'GAME_BOTW160', 'GAME_BOTW190'):
                executable = Path(directory) / ('settings-panel-' + (version or 'base'))
                command = [compiler, '-std=c11', '-fobjc-arc', '-Wall', '-Wextra',
                           '-Werror', '-Wno-deprecated-declarations', '-I' + str(gfx / 'include'),
                           '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                           str(ROOT / 'tests/settings_panel_probe.m'),
                           str(ROOT / 'games/totk/runtime/input_prompts.m'),
                           '-framework', 'CoreText', '-framework', 'CoreGraphics',
                           str(ROOT / 'tests/settings_output_fixture.c'),
                           str(gfx / 'totk_mfx.m'), str(gfx / 'totk_mfx_up.m'), str(gfx / 'totk_taa.m'),
                           str(gfx / 'fps_overlay.c'),
                           '-framework', 'Cocoa', '-framework', 'Metal',
                           '-framework', 'MetalFX', '-framework', 'QuartzCore',
                           '-framework', 'CoreVideo', '-o', str(executable)]
                if version:
                    command.append('-D' + version + '=1')
                if version.startswith('GAME_BOTW'):
                    command.append('-DGAME_BOTW=1')
                build = subprocess.run(command, capture_output=True, text=True, timeout=90)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                for layout in ('compact', 'corners'):
                    for overridden in (False, True):
                        with self.subTest(version=version, layout=layout, overridden=overridden):
                            env = os.environ.copy()
                            env.update(TOTK_UI_LANG='en', TOTK_HUD=layout,
                                       TOTK_MFX='interp' if overridden else '0',
                                       TOTK_MFX_OUTPUT='1' if overridden else '0',
                                       TOTK_MFX_UPSCALE='0', TOTK_TAA='0')
                            run = subprocess.run([str(executable)], env=env,
                                                 capture_output=True, text=True, timeout=35)
                            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                            self.assertIn('pass: settings panel', run.stdout)
                settings = Path(directory) / ('saved-' + (version or 'base'))
                settings.mkdir()
                env = {k: v for k, v in os.environ.items() if k not in ('TOTK_MFX', 'TOTK_TAA', 'TOTK_ASPECT', 'TOTK_OUTPUT_RES')}
                env.update(TOTK_UI_LANG='en', SETTINGS_TEST_DIR=str(settings))
                if version == 'TOTK_GAME_V143':
                    aa_cases = [
                        ('defaults', {}, 1, 0, 2),
                        ('stock-off', {'graphics-fxaa.txt': 'off'}, 0, 0, 2),
                        ('split-fxaa', {'graphics-fxaa.txt': 'off', 'graphics-aa-method.txt': '1',
                                        'graphics-fxaa-quality.txt': '1', 'graphics-smaa-quality.txt': '0'}, 1, 1, 0),
                        ('split-smaa', {'graphics-aa-method.txt': '2',
                                        'graphics-fxaa-quality.txt': '1', 'graphics-smaa-quality.txt': '3'}, 2, 1, 3),
                        ('split-off', {'graphics-aa-method.txt': '0',
                                       'graphics-fxaa-quality.txt': '1', 'graphics-smaa-quality.txt': '3'}, 0, 1, 3),
                        ('corrupt', {'graphics-aa-method.txt': '9', 'graphics-fxaa-quality.txt': '8',
                                     'graphics-smaa-quality.txt': '-1'}, 1, 0, 2),
                        ('corrupt-qualities', {'graphics-aa-method.txt': '2',
                                              'graphics-fxaa-quality.txt': '99', 'graphics-smaa-quality.txt': 'nan'}, 2, 0, 2),
                    ]
                    for name, files, method, fxaa, smaa in aa_cases:
                        with self.subTest(aa_preferences=name):
                            prefs = Path(directory) / name
                            prefs.mkdir()
                            for filename, value in files.items():
                                (prefs / filename).write_text(value + '\n')
                            current = dict(env, SETTINGS_TEST_DIR=str(prefs), SETTINGS_TEST_AA_METHOD=str(method),
                                           SETTINGS_TEST_FXAA_QUALITY=str(fxaa), SETTINGS_TEST_SMAA_QUALITY=str(smaa))
                            run = subprocess.run([str(executable), 'aa'], env=current, capture_output=True, text=True, timeout=10)
                            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                            self.assertIn('pass: AA preferences', run.stdout)
                run = subprocess.run([str(executable), 'update'], env=env, capture_output=True, text=True, timeout=35)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn('pass: update page', run.stdout)
                run = subprocess.run([str(executable), 'shots'], env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn('pass: screenshots page', run.stdout)
                run = subprocess.run([str(executable), 'nick'], env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn('pass: nickname', run.stdout)
                run = subprocess.run([str(executable), 'contrib'], env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn('pass: contribution page', run.stdout)   # 查看排名 on the 着色器贡献 page (user 2026-11-11)
                for action in ('save', 'restore', 'override'):
                    current = dict(env)
                    if action == 'override':
                        current.update(TOTK_MFX='interpmax', TOTK_TAA='0', TOTK_ASPECT='16:9', TOTK_OUTPUT_RES='1920x1080')
                    run = subprocess.run([str(executable), action], env=current, capture_output=True, text=True, timeout=35)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn('pass: settings persistence', run.stdout)
                (settings / 'graphics-frame.txt').write_text('lock-60\n')
                run = subprocess.run([str(executable), 'lock'], env=env, capture_output=True, text=True, timeout=35)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                for name, value in {'graphics-overlay.txt': 'invalid', 'graphics-frame.txt': 'lock-60junk',
                                    'graphics-output.txt': '999', 'graphics-aspect.txt': 'nan', 'graphics-taa.txt': '3'}.items():
                    (settings / name).write_text(value)
                run = subprocess.run([str(executable), 'corrupt'], env=env, capture_output=True, text=True, timeout=35)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

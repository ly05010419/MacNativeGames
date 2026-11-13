"""shader_update.py (user 2026-11-13: a button that checks and updates only the shaders and pipelines, from the game's
newest shared list on GitHub): what a list adds to an app is counted by the runtime's own rules, against the app's seed
and warm-up lists, and nothing is changed by a check."""
import json
from pathlib import Path
import plistlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
import shader_update as su  # noqa: E402

sl, pss = su.scripts()
KEY_A, KEY_B = 'a' * 64, 'b' * 64
COLOUR = '{"tex":{"0":"2d"}}'          # served by the seed's default variant (tex 0 any non-depth kind)
DEPTH = '{"tex":{"0":"2d_depth"}}'     # not served: the seed has no depth-bound variant of KEY_A


def name(key, deps):
    """The variant name a warm row gives a listed bind state."""
    return f'{key}-{sl.answers_suffix(sl.listed_answers(deps))}_b'


def fake_app(root, *, listed_rows=()):
    app = root / 'Tears of the Kingdom.app'
    res = app / 'Contents/Resources/totk'
    (res / 'rewritten/v143').mkdir(parents=True)
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable': 'TOTK', 'CFBundleIdentifier': 'org.macnativegames.totk'}))
    seed = res / 'build/shader-seed'
    seed.mkdir(parents=True)
    (seed / 'shader-keys.json').write_text(json.dumps({'keys': [KEY_A]}))
    (seed / f'{KEY_A}.variants').write_text(json.dumps([{'variant': f'{KEY_A}-{sl.DEFAULT_SUFFIX}', 'deps': {'tex': {'0': '*'}}}]))
    (seed / 'seed-manifest.json').write_text(json.dumps({'chain': 'test-chain'}))
    (res / 'build/pso_manifest.tsv').write_text('# exported\n' + ''.join('\t'.join(r) + '\n' for r in listed_rows))
    (res / 'build/pso_title.tsv').write_text('# exported\n')
    return app


def make_list(root, warm=()):
    path = root / 'games/totk/assets/shader_list.tsv'
    path.parent.mkdir(parents=True)
    lst = sl.ShaderList()
    for key, deps in ((KEY_A, COLOUR), (KEY_A, DEPTH), (KEY_B, DEPTH)):
        lst.add_variant(key, deps)
    for row in warm:
        lst.add_warm(row, 'm')
    lst.save(path)
    return path


class Check(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)

    def test_counts_by_the_runtime_s_pick_rule(self):
        default = ('S', f'{KEY_A}-{sl.DEFAULT_SUFFIX}_b', '0')
        waiting = ('S', name(KEY_A, DEPTH), '0')
        listfile = make_list(self.tmp, warm=(default, waiting))
        app = fake_app(self.tmp)
        before = sorted(p.read_bytes() for p in app.rglob('*') if p.is_file())
        r = su.check(app, listfile)
        self.assertEqual((r['game'], r['version']), ('totk', 'v143'))
        self.assertEqual(r['new_shaders'], 1)     # KEY_A's depth state; its colour state is served; KEY_B is another version's
        self.assertEqual(r['new_pipelines'], 1)   # the default variant's row, served by the seed, not in the app's lists
        self.assertEqual(r['waiting'], 1)         # the depth state's row waits for its shader
        self.assertEqual(sorted(p.read_bytes() for p in app.rglob('*') if p.is_file()), before)   # a check changes nothing

    def test_rows_the_app_lists_already_are_not_new(self):
        default = ('S', f'{KEY_A}-{sl.DEFAULT_SUFFIX}_b', '0')
        listfile = make_list(self.tmp, warm=(default,))
        self.assertEqual(su.check(fake_app(self.tmp, listed_rows=(default,)), listfile)['new_pipelines'], 0)

    def test_an_app_without_shader_keys_is_refused_in_words(self):
        app = fake_app(self.tmp)
        (app / 'Contents/Resources/totk/build/shader-seed/shader-keys.json').unlink()
        with self.assertRaisesRegex(RuntimeError, 'shader-keys.json'):
            su.check(app, make_list(self.tmp))


class Download(unittest.TestCase):
    def test_only_the_game_s_list_file_from_github_main(self):
        seen = []
        def fake(cmd, **kw):
            seen.append(cmd); Path(cmd[cmd.index('-o') + 1]).write_text('# list\n')
            return mock.Mock(returncode=0, stderr='')
        with tempfile.TemporaryDirectory() as d, mock.patch.object(su.subprocess, 'run', fake):
            out = su.download('botw', d)
            self.assertTrue(out.is_file())
        self.assertEqual(seen[0][0], '/usr/bin/curl')   # python.org's Python has no CA certificates
        self.assertEqual(seen[0][-1], 'https://raw.githubusercontent.com/ly05010419/MacNativeGames/main/games/botw/assets/shader_list.tsv')

    def test_a_failed_download_says_why(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.object(su.subprocess, 'run', return_value=mock.Mock(returncode=22, stderr='404')):
            with self.assertRaisesRegex(RuntimeError, '404'):
                su.download('totk', d)


class Job(unittest.TestCase):
    def test_the_launcher_s_update_shaders_runs_shader_update_in_the_app_s_work_folder(self):
        from tools.builder_jobs import build_job, repo
        plan = {'kind': 'express', 'work': '/w/totk-work', 'game': 'totk', 'reason': ''}
        with mock.patch.object(build_job, 'express_plan', return_value=plan):
            script, argv, work, app, kind, game = build_job.command({'update': '/Apps/Tears of the Kingdom.app', 'shaders': True})
        self.assertEqual((script, argv, work, kind, game), (repo.SHADER_UPDATE, ['/Apps/Tears of the Kingdom.app'], Path('/w/totk-work'), 'shaders', 'totk'))
        with mock.patch.object(build_job, 'express_plan', return_value={**plan, 'work': None, 'reason': 'disk not connected'}):
            with self.assertRaisesRegex(build_job.Refused, 'disk not connected'):
                build_job.command({'update': '/Apps/Tears of the Kingdom.app', 'shaders': True})

    def test_the_check_answers_in_one_json_object(self):
        from tools.builder_jobs import shaders
        result = {'game': 'totk', 'version': 'v143', 'new_shaders': 2, 'new_pipelines': 0, 'waiting': 1, 'listed_rows': 9}
        with mock.patch.object(su, 'identity', return_value=('totk', 'v143')), \
             mock.patch.object(su, 'download', return_value=Path('/x.tsv')), mock.patch.object(su, 'check', return_value=result):
            answer = shaders.check('/Apps/Tears of the Kingdom.app')
        self.assertTrue(answer['ok'] and answer['result']['can_update'])
        self.assertIn('2', answer['result']['line'])
        with mock.patch.object(su, 'identity', side_effect=RuntimeError('not an app')):
            self.assertEqual(shaders.check('/nowhere.app'), {'ok': False, 'error': 'not an app'})


if __name__ == '__main__':
    unittest.main()

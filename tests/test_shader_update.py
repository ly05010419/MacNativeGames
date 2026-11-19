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


class Dread(unittest.TestCase):
    """A game packaged by its own build.py: its runtime data is in Contents/Resources/<game>, its list tools are its
    own (games/dread/scripts), and shader_update.py cannot assemble an updated app for it."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.sl, self.pss = su.scripts('dread')
        self.addCleanup(su.scripts)   # put Tears of the Kingdom's tools back for the rest of the suite

    def variant(self, key, deps=None):
        answers = self.pss.answers_for(deps or {})
        return self.pss.variant_name(key, answers)

    def fake_app(self, *, listed_rows=()):
        app = self.tmp/'Metroid Dread.app'
        res = app/'Contents/Resources/dread'
        (app/'Contents/Resources/build-info.json').parent.mkdir(parents=True, exist_ok=True)
        (app/'Contents/Resources/build-info.json').write_text(json.dumps({'game': 'dread', 'version': 'v210'}))
        seed = res/'build/shader-seed'
        seed.mkdir(parents=True)
        # The inventory holds KEY_A only: a list row naming KEY_B is another version's and is passed over.
        (seed/'shader-keys.json').write_text(json.dumps({'keys': [KEY_A]}))
        (seed/f'{KEY_A}.variants').write_text(json.dumps(
            [{'variant': self.variant(KEY_A), 'deps': {'tex': {'0': '2d'}}}]))
        (seed/'seed-manifest.json').write_text(json.dumps({'chain': 'totk-native-v4-unverified'}))
        (res/'build/pso_manifest.tsv').write_text('# exported\n' + ''.join('\t'.join(r) + '\n' for r in listed_rows))
        return app

    def make_list(self, warm=()):
        path = self.tmp/'dread-shader_list.tsv'
        rows = [f'V\t{key}\t{self.sl.canonical(deps)}' for key, deps in
                ((KEY_A, {'tex': {'0': '2d'}}), (KEY_A, {'tex': {'0': '2d_depth'}}), (KEY_B, {'tex': {'0': '2d'}}))]
        rows += ['\t'.join(row) + '\tm' for row in warm]
        path.write_text(''.join(line + '\n' for line in rows))
        return path

    def test_the_check_reads_the_game_s_own_resources_folder_and_counts_with_its_own_tools(self):
        served = ('S', self.variant(KEY_A) + '_b', '0')                            # the seed carries this variant
        waiting = ('S', self.variant(KEY_A, {'tex': {'0': '2d_depth'}}) + '_b', '0')   # its shader is not translated yet
        r = su.check(self.fake_app(), self.make_list(warm=(served, waiting)))
        self.assertEqual((r['game'], r['version']), ('dread', 'v210'))
        self.assertEqual(r['new_shaders'], 1)     # KEY_A's depth state; its colour state is served, KEY_B is another version's
        self.assertEqual(r['new_pipelines'], 1)   # the served row, which the app's own list does not hold
        self.assertEqual(r['waiting'], 1)         # the depth row waits for its shader

    def test_rows_the_app_lists_already_are_not_new(self):
        served = ('S', self.variant(KEY_A) + '_b', '0')
        r = su.check(self.fake_app(listed_rows=(served,)), self.make_list(warm=(served,)))
        self.assertEqual(r['new_pipelines'], 0)

    def test_a_row_naming_another_version_s_shader_is_passed_over_not_counted_as_waiting(self):
        other = ('S', self.variant(KEY_B) + '_b', '0')   # KEY_B is not in the seed's inventory
        r = su.check(self.fake_app(), self.make_list(warm=(other,)))
        self.assertEqual((r['new_shaders'], r['new_pipelines'], r['waiting']), (1, 0, 0))

    def test_the_update_says_a_rebuild_takes_the_list_in_instead_of_half_assembling_an_app(self):
        served = ('S', self.variant(KEY_A) + '_b', '0')
        with self.assertRaisesRegex(RuntimeError, 'rebuild the app'):
            su.update(self.fake_app(), self.make_list(warm=(served,)))

    def test_the_window_is_not_offered_an_update_it_cannot_run(self):
        self.assertNotIn('dread', su.UPDATES)
        self.assertIn('dread', su.LISTS)   # but it is checked


class Window(unittest.TestCase):
    """The window draws "Check shaders" from its own list (Model.swift shaderGames) while the job reads
    shader_update.LISTS. A game in one and not the other either shows a button that cannot answer or, as Metroid Dread
    did until 2026-11-17, answers for a button that is never drawn."""

    def test_the_window_offers_the_check_for_the_games_the_job_can_check(self):
        import re
        swift = (Path(__file__).resolve().parents[1]/'tools/builder_app/Sources/MNGBuilder/Model.swift').read_text()
        line = re.search(r'static let shaderGames: Set<String> = \[([^\]]*)\]', swift).group(1)
        self.assertEqual(set(re.findall(r'"(\w+)"', line)), set(su.LISTS))

    def test_every_game_it_can_update_is_one_it_can_check(self):
        self.assertTrue(su.UPDATES <= set(su.LISTS))


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


class ExpressUpdateBringsTheNewestList(unittest.TestCase):
    """User 2026-11-17: "极速更新也要补新着色器（包含 检查着色器的功能）": update_runtime.py takes the game's newest list
    as Check shaders does, translates the shaders it adds and makes its pipelines, in the same update."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        import update_runtime
        self.ur = update_runtime

    def test_the_newest_list_from_github_else_the_repository_s_copy(self):
        got = self.tmp / 'got.tsv'
        with mock.patch.object(su, 'download', return_value=got) as download:
            self.assertEqual(self.ur.newest_shader_list('totk', self.tmp), (got, 'the newest on GitHub'))
        download.assert_called_once_with('totk', self.tmp / 'shader-list')
        with mock.patch.object(su, 'download', side_effect=RuntimeError('could not download: offline')):
            path, where = self.ur.newest_shader_list('botw', self.tmp)
        self.assertEqual(path, REPO / 'games/botw/assets/shader_list.tsv')
        self.assertIn('offline', where)   # the log says why the code's own list was used
        self.assertEqual(self.ur.newest_shader_list('oot', self.tmp, self.tmp / 'mine.tsv'),
                         ((self.tmp / 'mine.tsv').resolve(), 'given with --list'))
        self.assertEqual(self.ur.shared_list_games(), {'totk', 'botw', 'oot'})

    def test_new_shaders_are_translated_when_the_work_folder_can(self):
        app, listfile = fake_app(self.tmp), make_list(self.tmp)   # KEY_A's depth state is new
        tree, logs = self.tmp / 'totk', self.tmp / 'logs'
        with mock.patch.object(self.ur, 'work_files_from_app') as taken:
            with mock.patch.object(su, 'can_translate', return_value=''):
                self.assertTrue(self.ur.list_adds_shaders(app, 'v143', tree, listfile, logs))
            taken.assert_called_once_with(tree, app, 'v143', logs)   # what the work folder lacks, from the app first
            with mock.patch.object(su, 'can_translate', return_value='the build\'s work folder is not there'):
                self.assertFalse(self.ur.list_adds_shaders(app, 'v143', tree, listfile, logs))
            (app / 'Contents/Resources/totk/build/shader-seed/shader-keys.json').unlink()
            self.assertFalse(self.ur.list_adds_shaders(app, 'v143', tree, listfile, logs))   # its shaders stay

    def test_a_work_folder_without_its_game_files_takes_the_app_s_verified_ones(self):
        # 2026-11-17: scratch/build/botw19 held only the sources, so the 7 new shaders of BotW 1.9 were left out
        import hashlib, struct
        app = fake_app(self.tmp)
        res = app / 'Contents/Resources/totk'
        module = res / 'rewritten/v143/main.patched.bin'
        module.write_bytes(b'the rewritten main module')
        digest = hashlib.sha256(module.read_bytes()).hexdigest()
        tables = bytearray(64 + 208)
        struct.pack_into('<8s8sI', tables, 0, b'TOTKTAB1', b'v143', 1)
        struct.pack_into('<16s72s', tables, 64, b'main', digest.encode())
        (res / 'build/game-data/v143').mkdir(parents=True)
        (res / 'build/game-data/v143/runtime-tables.bin').write_bytes(bytes(tables))
        libs = self.tmp / 'ShaderData.bundle/shader-libs'
        libs.mkdir(parents=True)
        (libs / 'abc.metallib').write_bytes(b'lib')
        (res / 'build/shader-libs').symlink_to(libs)
        tree = self.tmp / 'work/totk'
        with mock.patch.object(self.ur.build_app, 'romfs_mismatch', return_value='there is none'), \
             mock.patch.object(self.ur.build_app, 'adopt_game_data') as adopt:
            self.ur.work_files_from_app(tree, app, 'v143', self.tmp / 'logs')
        self.assertEqual((tree / 'rewritten/v143/main.patched.bin').read_bytes(), b'the rewritten main module')
        adopt.assert_called_once_with(tree, app, 'v143', tree / 'extracted/v143/romfs', self.tmp / 'logs')
        self.assertEqual((tree / 'build/shader-libs/abc.metallib').read_bytes(), b'lib')
        # a module that is not the one the game data names is never taken
        module.write_bytes(b'another module')
        (tree / 'rewritten/v143/main.patched.bin').unlink()
        with mock.patch.object(self.ur.build_app, 'romfs_mismatch', return_value=None):
            self.ur.work_files_from_app(tree, app, 'v143', self.tmp / 'logs')
        self.assertFalse((tree / 'rewritten/v143/main.patched.bin').exists())

    def test_the_update_uses_that_list_for_shaders_lists_and_pipelines(self):
        text = (REPO / 'update_runtime.py').read_text()
        main = text[text.index('def main('):]
        for part in ("translate = args.seed or args.shader_libs or bool(listfile and list_adds_shaders(app, version, tree, listfile, logs))",
                     'make_seed(tree, work, game, version, logs, env, shader_list=listfile, app=app)',
                     'shaders = None if translate else build_app.app_shader_bundle(app, version)',
                     'if translate:\n            replace_seed(',
                     'prompt_app=app, shared=listfile)',
                     'if translate:\n        build_app.step_warm_metal(',
                     'return update_ocarina(app, work, remake=args.seed, shared=listfile)'):
            self.assertIn(part, main)
        oot = (REPO / 'games/oot/build.py').read_text()
        oot = oot[oot.index('def update_runtime('):oot.index('def update_shaders(')]
        self.assertIn('shader_steps(work,tree,logs,run,shared=shared,mode=mode)', oot)
        self.assertIn("package_app(work,tree,app,'library' if folder else 'app',folder,shared=shared,mode=mode)", oot)


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

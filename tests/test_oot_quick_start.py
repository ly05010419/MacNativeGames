"""Ocarina of Time's quick build (MNG Launcher 快速启动; user 2026-11-13: "就带上 菜单的 着色器就行"): the App carries only
the shaders the shared list's title rows name, the game translates the rest at first use, and 补全着色器 (update_shaders)
makes it a full App again. Checked here: the list's title shaders and its cut to them (shader_list.py title_keys / only),
the corpus cut (prepare_ns2_seed.py corpus_subset / only_keys), the seed's scope still naming every shader of the game,
the shader mode a build and an update take (games/oot/build.py shader_mode) and the capture switch that records the
title rows (nvn_mtl.m TOTK_PSO_CAPTURE)."""
import importlib.util
import os
import plistlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'
SCRIPTS = GAME / 'scripts'


class OotScripts:
    """games/oot/scripts on the path for a test, and at its end every module it loaded from there taken out again: the
    scripts import ns2_shader_corpus / shader_list, which other tests import from games/totk/scripts under the same
    names (2026-11-15: collect_shaders then took Ocarina of Time's ns2_shader_corpus and failed to import)."""

    def __enter__(self):
        sys.path.insert(0, str(SCRIPTS))
        self.before = set(sys.modules)
        return self

    def __exit__(self, *exc):
        sys.path.remove(str(SCRIPTS))
        for name in set(sys.modules) - self.before:
            where = getattr(sys.modules[name], '__file__', None) or ''
            if where.startswith(str(SCRIPTS)):
                del sys.modules[name]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def variant(n):
    return f'{n:064x}-{n:012x}_b'


def render_key(vs, fs):
    name = lambda v: v.encode().ljust(80, b'\0')
    return (name(vs) + name(fs) + bytes(544 - 160)).hex()


class QuickList(unittest.TestCase):
    """shader_list.py: the shaders of the title rows, and the list cut to some shaders."""

    def setUp(self):
        self.scripts = OotScripts().__enter__()
        self.shader_list = load('oot_shader_list_quick', SCRIPTS / 'shader_list.py')
        self.lst = self.shader_list.ShaderList()
        self.lst.add_warm(('R', render_key(variant(1), variant(2))), 't')
        self.lst.add_warm(('R', render_key(variant(3), variant(4))), 'm')
        self.lst.add_warm(('C', variant(5)), 'tm')
        self.lst.add_warm(('R', render_key(variant(1), variant(4))), 'm')   # one of its shaders is not a title one

    def tearDown(self):
        self.scripts.__exit__(None, None, None)

    def test_the_title_rows_name_the_quick_build_s_shaders(self):
        self.assertEqual(self.lst.title_keys(), {variant(n)[:64] for n in (1, 2, 5)})

    def test_a_quick_build_also_takes_the_mesh_rows_shaders(self):
        # user 2026-11-15: "快速启动 默认 预热 网格管线" -- a quick App compiled every mesh pipeline at its first draw
        # (a cold world entry: 32 of them, 75 s); their shaders (task, mesh, fragment) go into the quick seed so the
        # packaging warms them into the pipeline archive
        self.lst.add_warm(('M', variant(7), render_key(variant(8), variant(9))), 'm')
        self.lst.add_warm(('M', '-', render_key(variant(10), variant(2))), 'm')
        self.assertEqual(self.lst.quick_keys(), {variant(n)[:64] for n in (1, 2, 5, 7, 8, 9, 10)})
        self.assertEqual(self.lst.title_keys(), {variant(n)[:64] for n in (1, 2, 5)})   # the title rows alone, as before

    def test_a_quick_build_also_takes_every_compute_and_tile_read_pipeline(self):
        # user 2026-11-15: the compute (C) and tile-read (T) pipelines are warmed by default too, as the mesh ones
        self.lst.add_warm(('C', variant(11)), 'm')
        self.lst.add_warm(('T', render_key(variant(12), variant(13))), 'm')
        self.assertEqual(self.lst.quick_keys(), {variant(n)[:64] for n in (1, 2, 5, 11, 12, 13)})
        self.assertNotIn(variant(3)[:64], self.lst.quick_keys())   # an R row's shaders still only when it is a title row

    def test_a_list_without_title_rows_names_none(self):
        lst = self.shader_list.ShaderList()
        lst.add_warm(('R', render_key(variant(3), variant(4))), 'm')
        self.assertEqual(lst.title_keys(), set())

    def test_the_cut_keeps_only_rows_whose_every_shader_is_kept(self):
        keys = self.lst.title_keys()
        self.lst.add_variant(variant(1)[:64], '{"attr":{},"fout":{},"img":{},"tex":{}}')
        self.lst.add_variant(variant(3)[:64], '{"attr":{},"fout":{},"img":{},"tex":{}}')
        part = self.lst.only(keys)
        self.assertEqual(set(part.warm), {('R', render_key(variant(1), variant(2))), ('C', variant(5))})
        self.assertEqual({key for key, _ in part.variants}, {variant(1)[:64]})
        self.assertEqual(part.warm[('C', variant(5))], 'tm')   # the flags stay as listed
        self.assertEqual(len(self.lst.warm), 4)   # the list itself is not changed


class QuickCorpus(unittest.TestCase):
    """prepare_ns2_seed.py: the corpus cut to some shaders, in the corpus's own form."""

    def setUp(self):
        self.scripts = OotScripts().__enter__()
        self.seed = load('oot_ns2_seed_quick', SCRIPTS / 'prepare_ns2_seed.py')
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()
        self.scripts.__exit__(None, None, None)

    def corpus(self, n):
        records, keys = [], []
        for i in range(n):
            control, code, label = bytes([i]) * 0x48, bytes([i + 100]) * 32, f'shader{i}'.encode()
            records.append(struct.pack('<IIIH', i % 6, len(control), len(code), len(label)) + label + control + code)
            keys.append(self.seed.shader_key(control, code))
        path = self.dir / 'corpus.bin'
        path.write_bytes(self.seed.CORPUS_MAGIC + struct.pack('<I', n) + b''.join(records))
        return path, keys

    def test_the_cut_holds_the_asked_shaders_and_reads_as_a_corpus(self):
        path, keys = self.corpus(6)
        out = self.dir / 'only.bin'
        self.assertEqual(self.seed.corpus_subset(path, {keys[1], keys[4], '0' * 64}, out), 2)
        self.assertEqual(set(self.seed.corpus_inventory(out)), {keys[1], keys[4]})
        self.assertEqual(len(self.seed.corpus_inventory(path)), 6)   # the full corpus is left as it is

    def test_a_cut_with_none_of_the_corpus_shaders_stops(self):
        path, _ = self.corpus(3)
        with self.assertRaises(ValueError):
            self.seed.corpus_subset(path, {'0' * 64}, self.dir / 'only.bin')

    def test_the_only_file_holds_shader_keys(self):
        good = self.dir / 'good.txt'
        good.write_text('a' * 64 + '\n' + 'b' * 64 + '\n\n')
        self.assertEqual(self.seed.only_keys(good), {'a' * 64, 'b' * 64})
        for text in ('', 'xyz\n', 'a' * 63 + '\n'):
            bad = self.dir / 'bad.txt'
            bad.write_text(text)
            with self.assertRaises(ValueError, msg=repr(text)):
                self.seed.only_keys(bad)

    def test_a_quick_seed_s_scope_is_every_shader_of_the_game(self):
        # totk_rt_note_set writes a shader outside the scope into the player's contribution file as new: a quick seed's
        # scope of its few shaders would have sent thousands the list holds
        text = (SCRIPTS / 'prepare_ns2_seed.py').read_text()
        self.assertIn("shader_key_scope(sorted(corpus_inventory(full_corpus)) if only else keys", text)


class ShaderMode(unittest.TestCase):
    """games/oot/build.py shader_mode: what a build takes from OOT_SHADERS, what an update takes from the App."""

    def setUp(self):
        self.build = load('oot_build_quick', GAME / 'build.py')
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def app(self, shaders=None):
        app = Path(self.tmp.name) / 'Ocarina of Time.app'
        (app / 'Contents').mkdir(parents=True, exist_ok=True)
        info = {'CFBundleExecutable': 'OcarinaOfTime'}
        if shaders:
            info['MNGShaders'] = shaders
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
        return app

    def mode(self, env, app=None):
        clean = {k: v for k, v in os.environ.items() if k not in ('OOT_SHADERS', 'OOT_BUNDLE_SHADERS')}
        with mock.patch.dict(os.environ, {**clean, **env}, clear=True):
            return self.build.shader_mode(app)

    def test_a_build_carries_every_shader_unless_told(self):
        self.assertEqual(self.mode({}), 'full')
        self.assertEqual(self.mode({'OOT_SHADERS': 'title'}), 'title')
        self.assertEqual(self.mode({'OOT_BUNDLE_SHADERS': '0'}), 'none')   # the older test build's switch

    def test_an_update_keeps_what_the_app_records(self):
        self.assertEqual(self.mode({}, self.app('title')), 'title')
        self.assertEqual(self.mode({}, self.app()), 'full')   # an App from before 2026-11-13 records nothing
        self.assertEqual(self.mode({'OOT_SHADERS': 'full'}, self.app('title')), 'full')

    def test_an_unknown_mode_stops(self):
        with self.assertRaises(ValueError):
            self.mode({'OOT_SHADERS': 'some'})

    def test_a_list_without_title_rows_stops_a_quick_build(self):
        shared = Path(self.tmp.name) / 'shader_list.tsv'
        shared.write_text('# list\n')
        with self.assertRaises(ValueError) as e:
            self.build.title_shader_file(Path(self.tmp.name) / 'tree', shared)
        self.assertIn('no title rows', str(e.exception))

    def test_the_quick_seed_file_names_the_mesh_rows_shaders(self):
        name = lambda v: v.encode().ljust(80, b'\0')
        mesh_key = (name(variant(8)) + name(variant(9)) + bytes(544 - 160)).hex()
        title_key = (name(variant(1)) + name(variant(2)) + bytes(544 - 160)).hex()
        shared = Path(self.tmp.name) / 'shader_list.tsv'
        shared.write_text(f'# list\nR\t{title_key}\tt\nM\t{variant(7)}\t{mesh_key}\tm\n')
        written = self.build.title_shader_file(Path(self.tmp.name) / 'tree', shared).read_text().split()
        self.assertEqual(set(written), {variant(n)[:64] for n in (1, 2, 7, 8, 9)})

    def test_the_archive_is_built_from_the_libraries_the_app_carries(self):
        # 2026-11-15: the packaging tool loaded the full build's libraries (TOTK_NATIVE_DIR/shader-libs) for a quick App,
        # whose own are shader-libs-title: 494 of its 537 libraries differ in bytes, and the game's Metal refused 531 of
        # 602 archived pipelines ("Unable to find function main0"). The tool now takes the mode's libraries and names
        # them in its report, and packaging stops when they are not the ones the App carries.
        tree = Path(self.tmp.name)
        libraries = self.build.shader_dirs(tree, 'title')[1]
        libraries.mkdir(parents=True)
        self.build.check_archive_libraries({'libs': str(libraries)}, libraries)
        with self.assertRaisesRegex(ValueError, 'libraries'):
            self.build.check_archive_libraries({'libs': str(tree / 'build/shader-libs')}, libraries)
        with self.assertRaisesRegex(ValueError, 'libraries'):
            self.build.check_archive_libraries({}, libraries)   # a tool that does not say which ones it took
        text = (GAME / 'build.py').read_text()
        self.assertIn("TOTK_SHADER_LIBS_DIR=str(libraries)", text)
        self.assertIn("check_archive_libraries(report,libraries)", text)
        gfx = (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
        self.assertIn('getenv("TOTK_SHADER_LIBS_DIR")', gfx)
        self.assertIn('\\"libs\\": \\"%s\\"', gfx)

    def test_a_title_build_keeps_its_own_folders(self):
        # 2026-11-13: a title build in the full build's work folder deleted 3,081 pipeline archive files of the full
        # build and put all 64,607 libraries into the quick app's 2.7 GB shader bundle
        tree = Path(self.tmp.name)
        full, title = self.build.shader_dirs(tree, 'full'), self.build.shader_dirs(tree, 'title')
        self.assertEqual(full, (tree / 'build/shader-seed/oot', tree / 'build/shader-libs', ''))
        self.assertTrue(set(full[:2]).isdisjoint(title[:2]))
        self.assertEqual(title[2], '-title')
        text = (GAME / 'build.py').read_text()
        # the pipeline cache: one per kind, kept across chains (test_oot_archive_lists.CacheKeptAcrossChains)
        self.assertEqual(self.build.pipeline_cache(tree, '-title'), tree / 'archive-title')
        for used in ("cache=pipeline_cache(folder,suffix)","target=tree/f'build/pipelines{suffix}'",
                     "bundle=tree/f'build/shader-bundles/ShaderData-oot{suffix}.bundle'", "'--output',seed,", "'--out',libraries,"):
            self.assertIn(used, text)

    def test_a_title_update_needs_no_earlier_seed(self):
        # its few shaders are made again in the update (2026-11-13: the first title update of a work folder was refused
        # for want of shader-seed-title); a full update still needs the full build's seed and libraries
        work = Path(self.tmp.name) / 'work'
        tree = work / 'oot'
        (tree / 'extracted/oot/romfs').mkdir(parents=True)
        (tree / 'extracted/oot/romfs/index.json').write_text('{}')
        (tree / 'rewritten/oot').mkdir(parents=True)
        with mock.patch.object(self.build, 'sha', return_value='x'), \
                mock.patch.object(self.build.json, 'loads', return_value={'rewritten_sha256': {'main.patched.bin': 'x'}}):
            self.assertEqual(self.build.update_blocker(work, 'title'), '')
        self.assertIn('is missing', self.build.update_blocker(work, 'full'))

    def test_the_app_records_its_mode_and_shader_fill_makes_it_full(self):
        text = (GAME / 'build.py').read_text()
        self.assertIn("info['MNGShaders']=mode", text)
        self.assertIn("package_app(work,tree,app,'library' if folder else 'app',folder,shared=listfile,mode='full')", text)


class QuickAppContributesNothing(unittest.TestCase):
    def test_the_launcher_leaves_the_contribution_off_for_a_quick_app(self):
        # its lists hold only the title's rows: every pipeline would look new (2026-11-13: 165 of the list's own
        # pipelines and 120 of its variants in one session of the quick copy)
        text = (GAME / 'app/launcher.m').read_text()
        guard = text.index('BOOL quick = [shaders isKindOfClass:NSString.class] && [shaders isEqualToString:@"title"];')
        quick = text.index('unsetenv("TOTK_SHADER_CONTRIB");', guard)
        for call in ('totk_inbox_claim(', 'totk_contrib_prune(', 'contrib_ask(contrib);', 'totk_inbox_start(contrib'):
            self.assertGreater(text.index(call, guard), quick, call)   # only in the else branch, after the guard
        self.assertEqual(text.count('totk_inbox_start(contrib'), 1)

    def test_a_quick_app_does_not_turn_background_translation_on(self):
        # tried 2026-11-15 and taken back the same day: in the background its translator service crashed (user: "不用
        # 后台编译了 这个 有问题"); a quick app makes its shaders as every app does
        self.assertNotIn('TOTK_ASYNC_SHADERS', (GAME / 'app/launcher.m').read_text())


class LauncherQuickStart(unittest.TestCase):
    """MNG Launcher's 快速启动: a build request with quick makes the job's build carry only the title's shaders, only for
    Ocarina of Time; the installed list says which apps are quick (their cards offer 补全着色器)."""

    def setUp(self):
        sys.path.insert(0, str(ROOT))
        from tools.builder_jobs import build_job
        self.build_job = build_job

    def tearDown(self):
        sys.path.remove(str(ROOT))

    def test_a_quick_build_of_ocarina_of_time_carries_the_title_s_shaders(self):
        with mock.patch.dict(os.environ, {'OOT_SHADERS': ''}):
            self.assertEqual(self.build_job.job_env({'quick': True}, 'oot')['OOT_SHADERS'], 'title')
            self.assertEqual(self.build_job.job_env({}, 'oot')['OOT_SHADERS'], '')   # a normal build as before

    def test_another_game_refuses_a_quick_build(self):
        with self.assertRaises(self.build_job.Refused):
            self.build_job.job_env({'quick': True}, 'totk')

    def test_the_job_runs_the_build_with_that_environment_and_its_own_history(self):
        text = (ROOT / 'tools/builder_jobs/build_job.py').read_text()
        self.assertIn('start_new_session=True, env=env)', text)
        self.assertIn("'macos-quick' if request.get('quick')", text)

    def test_a_card_s_quick_start_is_judged_as_a_quick_build(self):
        # user 2026-11-15: the card's 快速启动 and the Build page's switch are one function; an installed app whose
        # translator changed (a full build otherwise) takes it as an update that makes only the title's shaders
        seen = {}
        def plan(app, python, work=None, env=None):
            seen['env'] = env
            return {'kind': 'express', 'work': '/w', 'game': 'oot'}
        with mock.patch('tools.builder_jobs.installed.plan', plan), mock.patch.dict(os.environ, {'OOT_SHADERS': ''}):
            script, argv, work, app, kind, game = self.build_job.command({'update': '/Apps/Ocarina of Time.app', 'quick': True})
        self.assertEqual(seen['env']['OOT_SHADERS'], 'title')
        self.assertEqual((kind, game), ('express', 'oot'))
        with mock.patch('tools.builder_jobs.installed.plan', plan):
            self.build_job.command({'update': '/Apps/Ocarina of Time.app'})
        self.assertIsNone(seen['env'])   # an ordinary update is judged as before

    def test_the_card_button_follows_the_game_pages_and_sends_the_same_request(self):
        swift = ROOT / 'tools/builder_app/Sources/MNGBuilder'
        home = (swift / 'HomePage.swift').read_text()
        self.assertLess(home.index('ForEach(T.gamePages.filter'), home.index('Button { model.quickStart(app) } label: { Text(T.quickStart)'))
        model = (swift / 'Model.swift').read_text()
        self.assertIn('start(BuildRequest(update: app.app, quick: true))', model)
        self.assertIn('quick: quick ? true : nil))', model)   # the Build page's switch: the same field
        self.assertIn('Toggle(T.quickStart', (swift / 'ActionSection.swift').read_text())

    def test_the_installed_list_names_a_quick_app(self):
        text = (ROOT / 'tools/builder_jobs/installed.py').read_text()
        self.assertIn("'shaders': info.get('MNGShaders') or 'full'", text)
        swift = ROOT / 'tools/builder_app/Sources/MNGBuilder'
        self.assertIn('if app.shaders == "title"', (swift / 'HomePage.swift').read_text())
        self.assertIn('model.updateShaders(app)', (swift / 'HomePage.swift').read_text())
        self.assertIn('var quick: Bool? = nil', (swift / 'Reports.swift').read_text())


class QuickStartCarriesDiagnostics(unittest.TestCase):
    """user 2026-11-15: "去掉诊断模式 快速启动 默认带诊断": the Launcher's 诊断模式 page is gone; a quick App (MNGShaders 'title')
    starts with every diagnostic of a debug session however it is opened (app/launcher.m start_diagnostics), with a run
    folder of its own for the per-frame record. user 2026-11-16: "诊断模式 也改成 CheckBox， 正常启动 可以带上 诊断模式": a full
    App's start carries them too while the card's 诊断模式 switch is on (diagnostics.txt), and none while it is off."""
    SWITCHES = ('TOTK_DEBUG_LOG', 'TOTK_LONGFRAME_DETAIL', 'TOTK_SVC_STATS', 'TOTK_SURVEY_SHADER_LOG', 'DM_NVN_TSV', 'DM_NVN_UPTIME')

    def parts(self):
        text = (GAME / 'app/launcher.m').read_text()
        start = text.index('static void start_diagnostics(NSString *home, const char *why) {')
        end = text.index('\n}\n', start)
        return text[start:end], text[:start] + text[end:]

    def test_the_diagnostics_set_every_switch(self):
        diag, _ = self.parts()
        for name in self.SWITCHES:
            # overwrite 0: a test start's own choice (its record path, a switch it turns off) is kept
            self.assertRegex(diag, rf'setenv\("{name}", [^;]*, 0\)', name)
        self.assertIn('Logs/MacNativeGames/diagnostics', diag)   # the run folder of its per-frame record
        self.assertIn('frames.tsv', diag)

    def test_a_quick_app_always_and_a_full_app_only_with_the_switch(self):
        _, rest = self.parts()
        self.assertIn('if (quick) start_diagnostics(home, "quick App");', rest)
        self.assertIn('else if (diagnostics_switch(data)) start_diagnostics(home,', rest)
        for name in self.SWITCHES:   # nowhere else: a full App with the switch off starts with none
            self.assertNotIn(f'setenv("{name}"', rest, name)
        switch = rest[rest.index('static BOOL diagnostics_switch(NSString *data) {'):]
        switch = switch[:switch.index('\n}\n')]
        from tools.builder_jobs import memory_saver
        self.assertIn(f'@"{memory_saver.SWITCHES["diagnostics"][0]}"', switch)   # the file the Launcher's card writes
        self.assertIn('isEqualToString:@"on"', switch)

    def test_the_diagnostic_page_is_gone(self):
        from tools.builder import games
        from tools.builder_jobs import tabs
        self.assertNotIn('diag', tabs.TABS)
        self.assertTrue(all('diag' not in pages for pages in games.PAGES.values()))
        self.assertFalse((ROOT / 'tools/builder_jobs/tab_diag.py').exists())
        swift = ROOT / 'tools/builder_app/Sources/MNGBuilder'
        self.assertFalse((swift / 'DiagTab.swift').exists())
        for name in ('Model.swift', 'HomePage.swift', 'Strings.swift'):
            text = (swift / name).read_text()
            self.assertNotIn('"diag"', text, name)
            self.assertNotIn('DiagPage', text, name)

    def test_the_quick_start_says_it_carries_the_diagnostics(self):
        strings = (ROOT / 'tools/builder_app/Sources/MNGBuilder/Strings.swift').read_text()
        start = strings.index('static let quickStartHelp')
        definition = strings[start:strings.index('static let', start + 1)]   # both languages, over two lines
        self.assertIn('诊断', definition)
        self.assertIn('diagnostic', definition)


class NoSetuidPs(unittest.TestCase):
    def test_the_build_finds_the_running_game_without_ps(self):
        # /bin/ps is setuid and cannot run under this Mac's storage guard (2026-11-15: the quick app's update stopped at
        # "Operation not permitted: 'ps'"); the processes are read with libproc / sysctl as build_app.py reads them
        text = (GAME / 'build.py').read_text()
        self.assertNotIn("['ps'", text)
        self.assertIn('build_app.running_executables()', text)
        self.assertIn('build_app.process_args(pid)', text)
        build = load('oot_build_ps', GAME / 'build.py')
        with mock.patch.object(build.build_helpers(), 'running_executables', return_value=[(1, '/x/Contents/MacOS/Other')]):
            self.assertFalse(build.app_running('/x'))


class CaptureSwitch(unittest.TestCase):
    def test_the_capture_records_before_the_lists_leave_rows_out(self):
        text = (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
        body = text[text.index('static void pso_user_record(const char *fmt, ...) {'):]
        body = body[:body.index('\n}\n')]
        self.assertLess(body.index('pso_capture(line);'), body.index('if (!g_pso_shared) return;'))   # one device list (device_list)
        self.assertLess(body.index('pso_capture(line);'), body.index('if ([g_pso_user_seen containsObject:k]) return;'))
        self.assertIn('getenv("TOTK_PSO_CAPTURE")', text)


if __name__ == '__main__':
    unittest.main()

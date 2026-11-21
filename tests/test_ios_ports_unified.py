"""Every port with an iPhone / iPad app is built with the Zelda app and tools (user 2026-11-20: "统一 统一 统一！！！ 这是规范
要求！！！ 用测试保证 不靠记忆！"): one build (games/totk/ios/tools/build_port_ipa.py), one app (games/totk/ios/Sources with
its GAME_<NAME> branches), one iOS front end (shared/runtime/frontend_ios.m), one installer. A port's games/<game>/ios holds
its profile (tools/build_ipa.py) and its answers to the shell (Sources/ios_front.m), nothing else."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PORTS = {'acnh': 'GAME_ACNH', 'odyssey': 'GAME_ODYSSEY'}
SHARED_BUILD = ROOT / 'games/totk/ios/tools/build_port_ipa.py'


def files(folder):
    return {p.name for p in folder.iterdir() if p.is_file() and p.name != '.DS_Store'} if folder.is_dir() else set()


class Ports(unittest.TestCase):
    def test_only_a_profile_and_its_answers(self):
        for game in PORTS:
            with self.subTest(game=game):
                ios = ROOT / 'games' / game / 'ios'
                self.assertEqual(files(ios / 'tools'), {'build_ipa.py'})
                self.assertEqual(files(ios / 'Sources'), {'ios_front.m'})
                self.assertEqual({p.name for p in ios.iterdir() if p.name not in ('.DS_Store', '__pycache__')}, {'tools', 'Sources'})

    def test_profiles_call_the_shared_build(self):
        for game, define in PORTS.items():
            with self.subTest(game=game):
                text = (ROOT / 'games' / game / 'ios/tools/build_ipa.py').read_text()
                self.assertIn('import build_port_ipa as port_ipa', text)
                self.assertIn(f"define='{define}'", text)
                self.assertIn(f"game='{game}'", text)
                self.assertIn('port_ipa.cli(PORT)', text)
                self.assertNotIn('xcodebuild', text, 'the build is the shared one')

    def test_one_front_end(self):
        shared = ROOT / 'shared/runtime/frontend_ios.m'
        for game in PORTS:
            with self.subTest(game=game):
                link = ROOT / 'games' / game / 'runtime/frontend_ios.m'
                self.assertTrue(link.is_symlink() and link.resolve() == shared.resolve())
        self.assertIn('CFBundleDisplayName', shared.read_text(), 'the boot title is the app\'s own name')

    def test_the_shell_knows_every_port(self):
        features = (ROOT / 'games/totk/ios/Sources/port_features.h').read_text()
        settings = (ROOT / 'games/totk/ios/Sources/picture_settings.h').read_text()
        for game, define in PORTS.items():
            with self.subTest(game=game):
                self.assertIn(f'defined({define})', features)
                self.assertRegex(settings, rf'defined\({define}\)\s*\n#define PP_THIS_GAME PP_GAME_{game.upper()}')
        self.assertIn("('totk','botw','acnh','odyssey')", (ROOT / 'games/totk/ios/tools/build_ipa.py').read_text().replace(' ', ''),
                      'pane_for gives every port the picture rows')

    def test_shaders_outside_the_app_as_zeldas(self):
        # user 2026-11-21: "为什么 安装 这么慢 … 这个不是标准吗 为什么又发明一遍 ？？？ 写测试保证": with --image-outside
        # the port build leaves the shaders beside the app (<out>-shader-seed with fingerprint.txt), which Zelda's
        # installer pushes once as image parts; the app carries no shader-seed or shader-cache of its own
        build = SHARED_BUILD.read_text()
        self.assertIn("seed_out = out.with_name(out.stem + '-shader-seed')", build)
        self.assertIn("stamp = out / 'fingerprint.txt'", build)
        installer = (ROOT / 'games/totk/ios/tools/install_device.py').read_text()
        self.assertIn("seed = ipa.with_name(ipa.stem + '-shader-seed')", installer, 'the installer looks where the build puts it')
        i = build.index('if a.image_outside:\n        # The shaders beside the app')
        outside = build[i:build.index('    else:', i)]
        self.assertNotIn("res / 'shader-seed'", outside)
        self.assertNotIn("res / 'shader-cache'", outside)

    def test_device_libraries_as_zeldas(self):
        # user 2026-11-21: "为什么 ipad在 预热着色器， 看看塞尔达 如何做的 统一 用测试保证": the port seed gets the device
        # .metallib files from Zelda's compile_seed.py (the game loads them instead of compiling on the phone), in both
        # packagings, and never the Mac's (macOS) libraries
        build = SHARED_BUILD.read_text()
        self.assertIn("from compile_seed import seed_flags", build)
        self.assertIn("TOOLS / 'compile_seed.py', out, 'iphoneos'", build)
        self.assertEqual(build.count("seed_with_mac_cache(L.seed, cache, seed_out, run,"), 2, 'outside and inside the app')
        self.assertNotRegex('a' * 64 + '-' + 'b' * 12 + '_b.metallib', __import__('re').compile(
            __import__('re').search(r"SEED_NAME = re.compile\(r'(.*?)'\)", build).group(1)))
        for game in PORTS:
            gfx = (ROOT / 'games' / game / 'third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
            self.assertIn('totk_rt_dir(), pc]', gfx, f'{game}: its shader loader takes the device library from the cache')
            self.assertIn('.metallib", totk_rt_dir()', gfx, game)

    def test_damaged_device_index_does_not_refuse_the_seed(self):
        # an empty .variants a crash left in the iPad's cache made the import refuse the whole seed (ACNH, 2026-11-21)
        ports = (ROOT / 'shared/third_party/d2r_gfx/gfx/nvn/totk_shader_seed_ports.h').read_text()
        self.assertIn('if (existing && ![user isKindOfClass:NSArray.class]) { user = nil; existing = nil; }', ports)
        self.assertNotIn('(existing && ![user isKindOfClass:NSArray.class])) {', ports)

    def test_merged_seed_names_pass_the_import(self):
        # the Mac cache keeps seed-installed.json beside its shaders; merged into the seed, the import refused it and the
        # game started with no shaders (iPad, 2026-11-21)
        import importlib.util, json, re, shutil, tempfile
        spec = importlib.util.spec_from_file_location('build_port_ipa', SHARED_BUILD)
        mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
        safe = re.search(r'regularExpressionWithPattern:\s*@"(.*?)"', (ROOT / 'shared/third_party/d2r_gfx/gfx/nvn/totk_shader_seed_ports.h').read_text()).group(1).replace('\\\\', '\\')
        with tempfile.TemporaryDirectory() as d:
            seed, cache, out = Path(d, 'seed'), Path(d, 'cache'), Path(d, 'out')
            seed.mkdir(); cache.mkdir()
            (seed / 'seed-manifest.json').write_text(json.dumps({'files': {}}))
            shader = 'a' * 64 + '-' + 'b' * 12 + '_b.metal'
            (cache / shader).write_text('x'); (cache / 'seed-installed.json').write_text('{}')
            mod.seed_with_mac_cache(seed, cache, out)
            names = json.loads((out / 'seed-manifest.json').read_text())['files']
            self.assertEqual(set(names), {shader})
            for n in names: self.assertRegex(n, safe)

    def test_one_seed_reader(self):
        # the image parts are read by the shared totk_seed_image.h, the ports' import is one shared file
        image = ROOT / 'shared/third_party/d2r_gfx/gfx/nvn/totk_seed_image.h'
        ports = ROOT / 'shared/third_party/d2r_gfx/gfx/nvn/totk_shader_seed_ports.h'
        self.assertIn('#include "totk_seed_image.h"', ports.read_text())
        self.assertIn('seed_image_data(', ports.read_text())
        for game in ('totk', *PORTS):
            link = ROOT / 'games' / game / 'third_party/d2r_gfx/gfx/nvn/totk_seed_image.h'
            self.assertTrue(link.is_symlink() and link.resolve() == image.resolve(), game)
        for game in PORTS:
            link = ROOT / 'games' / game / 'third_party/d2r_gfx/gfx/nvn/totk_shader_seed.h'
            self.assertTrue(link.is_symlink() and link.resolve() == ports.resolve(), game)

    def test_every_port_has_its_icon(self):
        # user 2026-11-21: "Odyssey 没有 icon？": its Mac build keeps the icon in app-assets, not build/AppIcon.icns
        self.assertIn("icon=work / 'app-assets/OdysseyIcon.jpg'", (ROOT / 'games/odyssey/ios/tools/build_ipa.py').read_text())
        self.assertIn("L.icon or L.build / 'AppIcon.icns'", SHARED_BUILD.read_text())

    def test_runtimes_answer_the_shell(self):
        for game in PORTS:
            with self.subTest(game=game):
                rt = ROOT / 'games' / game / 'runtime'
                self.assertIn('void runtime_audio_output_restart(int recreate)', (rt / 'audio_mixer.c').read_text())
                self.assertIn('TOTK_ROMFS_DATA', (rt / 'romfs.c').read_text(), 'the image in Documents/GameData (install_device.py --game-data)')
                front = (ROOT / 'games' / game / 'ios/Sources/ios_front.m').read_text()
                for name in ('runtime_phone_fps', 'runtime_phone_fps_set', 'runtime_display_max_hz'):
                    self.assertRegex(front, rf'\b{name}\(', name)



NVN = 'third_party/d2r_gfx/gfx/nvn'
SHARED_NVN = ROOT / 'shared' / NVN
TOOLS = ROOT / 'games/totk/ios/tools'
ARCHIVE = ('totk_pipeline_archive.h', 'totk_pipeline_archive_files.h', 'totk_pipeline_archive_folders.h',
           'totk_pipeline_archive_publish.h', 'nvn_archive_map.h', 'nvn_compiler_retry.h')
PORT_ARCHIVE = ('totk_pipeline_archive_port.h', 'totk_pipeline_archive_port_fill.h')


def load_tool(name):
    import importlib.util, sys
    sys.path.insert(0, str(TOOLS))
    spec = importlib.util.spec_from_file_location(f'ports_{name}', TOOLS / f'{name}.py')
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod


class PipelineArchive(unittest.TestCase):
    """The ports make their pipelines with Tears of the Kingdom's pipeline archive and get the device's pipelines the way
    Zelda's iPhone / iPad build makes them (user 2026-11-21: "统一 … 用测试保证 不靠记忆"): the iPad warmed 61,896 items
    (every shader library of the seed) at every start of Odyssey and Animal Crossing, while Zelda takes its pipelines from
    the archive the Mac compiled for the device's GPU and loads no library for itself."""

    def test_one_archive_for_every_game(self):
        for name in ARCHIVE + PORT_ARCHIVE:
            self.assertTrue((SHARED_NVN / name).is_file() and not (SHARED_NVN / name).is_symlink(), name)
        for game in ('totk', *PORTS):
            for name in ARCHIVE + (PORT_ARCHIVE if game in PORTS else ()):
                with self.subTest(game=game, name=name):
                    link = ROOT / 'games' / game / NVN / name
                    self.assertTrue(link.is_symlink() and link.resolve() == (SHARED_NVN / name).resolve())
        for game in PORTS:
            # the translation chain and the cache folder stay each game's: the shared header's quoted includes find them
            # beside the link (dmsh_chain.h: the ports' v4, Zelda's v9)
            for own in ('dmsh_chain.h', 'totk_shader_rt.h'):
                self.assertFalse((ROOT / 'games' / game / NVN / own).is_symlink(), f'{game}: {own} is its own')
        self.assertIn('#include "dmsh_chain.h"', (SHARED_NVN / 'totk_pipeline_archive.h').read_text())

    def test_ports_make_pipelines_from_the_archive(self):
        hooks = (SHARED_NVN / 'totk_pipeline_archive_port.h').read_text()
        self.assertIn('#include "totk_pipeline_archive.h"', hooks)
        for head in ('static id<MTLRenderPipelineState> pso_new_(MTLRenderPipelineDescriptor *d, NSString *key, NSError **e) {',
                     'static id<MTLComputePipelineState> cs_new_(NvShader *cs, NSError **e) {'):
            body = hooks[hooks.index(head):]
            body = body[:body.index('\n}\n')]
            with self.subTest(fn=head):
                for need in ('par_archive_for(g_dev, key)', 'MTLPipelineOptionFailOnBinaryArchiveMiss', 'par_pre_note(ps != nil)',
                             'par_stale(key)', 'par_add(g_dev, key, d)'):
                    self.assertIn(need, body)
        self.assertIn('if (!merged && ((k->a2c & PSO_VBORDER) || k->fsfn)) return nil;', hooks, 'a border variant is not archived (PAR_SCHEMA 5)')
        for game in PORTS:
            with self.subTest(game=game):
                gfx = (ROOT / 'games' / game / NVN / 'nvn_mtl.m').read_text()
                self.assertIn('#include "totk_pipeline_archive_port.h"', gfx)
                self.assertIn('#include "totk_pipeline_archive_port_fill.h"', gfx)
                self.assertNotIn('#include "totk_pipeline_archive.h"', gfx, 'through the ports\' hooks only')
                for old in ('static id<MTLRenderPipelineState> pso_new_(', 'par_game_idle', 'g_par_last_compile_ns', 'PAR_IDLE_NS'):
                    self.assertNotIn(old, gfx, f'no private archive of its own: {old}')

    def test_warm_up_skips_the_libraries_with_an_archive(self):
        for game in PORTS:
            with self.subTest(game=game):
                gfx = (ROOT / 'games' / game / NVN / 'nvn_mtl.m').read_text()
                start = gfx[gfx.index('static void warm_start(void) {'):]
                self.assertLess(start.index('par_open_dir(g_dev);'), start.index('warm_plan(paths,files'))
                self.assertIn('g_warm_pipelines_only = g_par_valid;', start)
                self.assertIn('warm_observed() || g_par_valid ? @[] : cache_names(totk_rt_dir())', start, 'no cache-wide libraries')
                self.assertIn('if (g_warm_pipelines_only || !warm_available(c[1])) continue;', gfx, 'no S rows')
                worker = gfx[gfx.index('MLOG("后台预热：着色器库用'):gfx.index('int mtl_warm_wait(int timeout_ms) {')]
                self.assertIn('par_write_in_place = 1;', worker, 'what the archive lacked is written by the warm-up')
                self.assertIn('warm_observed() || g_par_valid ? @[] : cache_names(rt)', worker)
                self.assertIn('par_flush_thread(); par_write_in_place = 0;', worker)
                self.assertIn('if (shader_device_libs())', gfx, 'the Mac\'s packaging mode loads the device libraries')

    def test_hosts_have_the_pipeline_archive_mode(self):
        fill = (SHARED_NVN / 'totk_pipeline_archive_port_fill.h').read_text()
        self.assertIn('int mtl_pipeline_archive_tool(int argc, char **argv) {', fill)
        for need in ('setenv("TOTK_SHADER_DEVICE_LIBS", "1", 1)', 'setenv("TOTK_PIPELINE_ARCHIVE_BATCH", "1", 1)',
                     'setenv("TOTK_PIPELINE_PREBUILT", "0", 1)', 'par_prune_superseded()', 'par_flush_thread();'):
            self.assertIn(need, fill)
        for game in PORTS:
            with self.subTest(game=game):
                loader = (ROOT / 'games' / game / 'runtime/loader.c').read_text()
                self.assertIn('need(argc==3||(argc>3&&!strcmp(argv[2],"pipeline-archive")),', loader)
                self.assertIn('if(!strcmp(argv[2],"pipeline-archive"))return mtl_pipeline_archive_tool(argc-3,argv+3);', loader)
                self.assertIn('int mtl_pipeline_archive_tool(int argc, char **argv);', (ROOT / 'games' / game / 'runtime/runtime.h').read_text())
                # Metal's archive builder needs a cache folder of the binary's own (totk_pipeline_archive_folders.h): a bundle id
                make = (ROOT / 'games' / game / 'Makefile').read_text()
                self.assertIn('GAME_INFO := -Wl,-sectcreate,__TEXT,__info_plist,runtime/game_info.plist', make)
                self.assertRegex(make, rf'xcrun clang -std=c11 \$\(NATIVE_OPT\) \$\(GAME_INFO\)')
                self.assertIn(f'org.macnativegames.{game}-native', (ROOT / 'games' / game / 'runtime/game_info.plist').read_text())
        self.assertIn("'game_info.plist'", SHARED_BUILD.read_text().split('mac_only: tuple = ')[1].split('\n')[0], 'not an app resource')

    def test_builds_make_the_device_pipelines_the_zelda_way(self):
        tool = (TOOLS / 'device_pipelines.py').read_text()
        self.assertIn("run([tool, rewritten, 'pipeline-archive', *lists]", tool)
        for build in (TOOLS / 'build_ipa.py', SHARED_BUILD):
            with self.subTest(build=build.name):
                text = build.read_text()
                self.assertIn('from device_pipelines import device_gpus, make as make_device_pipelines, seed_check', text)
                self.assertIn('device_gpus(', text)
                self.assertIn('make_device_pipelines(gpus', text)
                self.assertIn('seed_check(', text)
                self.assertNotIn("'pipeline-archive'", text, 'the steps are device_pipelines.make, not a copy')
        port = SHARED_BUILD.read_text()
        self.assertIn('variants=False', port, 'the ports record no variants: one run')
        self.assertEqual(port.count('device_pipelines(port, L, seed_out, cache, work,'), 2, 'outside and inside the app')
        self.assertIn("'--device-gpu'", port)

    def test_make_runs_zeldas_steps(self):
        import tempfile
        dp = load_tool('device_pipelines')
        for variants in (True, False):
            with self.subTest(variants=variants), tempfile.TemporaryDirectory() as d:
                d = Path(d); host = d / 'host'; host.write_bytes(b'h'); seed = d / 'seed'; seed.mkdir()
                (seed / 'seed-manifest.json').write_text('{}'); (seed / 'variant-x.metallib').write_text('old')
                lst = d / 'pso_list.tsv'; lst.write_text('R\tk\n'); logs = d / 'logs'; logs.mkdir()
                calls = []
                def run(args, cwd, log, env):
                    args = [str(x) for x in args]; calls.append((args, log.name, env.get('TOTK_PAR_RECHECK')))
                    if '--variants' in args: log.write_text('{"variants": 0, "compiled": 0, "kept": 0, "failed": 0}\n')
                    if '--mac' in args: log.write_text('{"archs": [{"arch": "applegpu_g15p", "pipelines": 1, "compiled": 1, "kept": 0, "failed": 0}], "library from source": 0}\n')
                import contextlib, io
                with contextlib.redirect_stdout(io.StringIO()):
                    dp.make(['applegpu_g15p'], host, d / 'rewritten', [lst], [seed / 'seed-manifest.json', lst, host], seed, d / 'work',
                            'iphoneos', run, d, logs, {}, lambda s, t: t.write_bytes(s.read_bytes()), variants=variants)
                modes = [('archive' if 'pipeline-archive' in a else '--variants' if '--variants' in a else '--mac') for a, _, _ in calls]
                self.assertEqual(modes, ['archive', '--variants', 'archive', '--mac'] if variants else ['archive', '--mac'])
                archive = [c for c in calls if 'pipeline-archive' in c[0]]
                self.assertTrue(all(a[0].endswith('ios-pipeline-archive') for a, _, _ in archive), 'a tool, not a game (game_slot.c)')
                self.assertTrue(all(r == '1' for _, _, r in archive), 'a new cache is checked against the runtime (mac_cache)')
                self.assertEqual(archive[-1][0][-1], str(lst))
                self.assertEqual((d / 'work/mac-cache/seed.txt').is_file(), True)
                self.assertEqual((seed / 'variant-x.metallib').exists(), not variants, 'the scan starts without the last build\'s variants')
                mac = calls[-1][0]
                self.assertEqual(mac[mac.index('--arch') + 1], 'applegpu_g15p')
                self.assertEqual(mac[mac.index('--mac') + 1], str(d / 'work/mac-cache/pipelines'))

    def test_port_lists_and_fingerprint(self):
        import tempfile
        port = load_tool('build_port_ipa')
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); a, b = d / 'a.tsv', d / 'b.tsv'
            a.write_text('# head\nR\tx\nS\ts\t0\nC\tc\n'); b.write_text('R\tx\nR\ty\n')
            self.assertEqual(port.port_lists([a, b, d / 'missing.tsv'], d / 'out/pso_list.tsv'), 3)
            self.assertEqual([l for l in (d / 'out/pso_list.tsv').read_text().splitlines() if not l.startswith('#')], ['R\tx', 'C\tc', 'R\ty'])
            seed = d / 'seed'; seed.mkdir(); (seed / 'seed-manifest.json').write_text('{}')
            plain = port.seed_fingerprint(seed)
            (seed / 'pipelines-applegpu_g15p.json').write_text('{}')
            ipad = port.seed_fingerprint(seed)
            (seed / 'pipelines-applegpu_g15p.json').unlink(); (seed / 'pipelines-applegpu_g14p.json').write_text('{}')
            self.assertEqual(len({plain, ipad, port.seed_fingerprint(seed)}), 3, 'another GPU\'s pipelines are pushed again')

if __name__ == '__main__':
    unittest.main()

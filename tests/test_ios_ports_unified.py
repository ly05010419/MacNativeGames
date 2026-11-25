"""Every port with an iPhone / iPad app is built with the Zelda app and tools (user 2026-11-20: "统一 统一 统一！！！ 这是规范
要求！！！ 用测试保证 不靠记忆！"): one build (games/totk/ios/tools/build_port_ipa.py), one app (games/totk/ios/Sources with
its GAME_<NAME> branches), one iOS front end (shared/runtime/frontend_ios.m), one installer. A port's games/<game>/ios holds
its profile (tools/build_ipa.py) and its answers to the shell (Sources/ios_front.m), nothing else."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PORTS = {'acnh': 'GAME_ACNH', 'odyssey': 'GAME_ODYSSEY', 'pla': 'GAME_PLA', 'mk8dx': 'GAME_MK8DX', 'aocx': 'GAME_AOCX'}
SHARED_BUILD = ROOT / 'games/totk/ios/tools/build_port_ipa.py'


def files(folder):
    return {p.name for p in folder.iterdir() if p.is_file() and p.name != '.DS_Store'} if folder.is_dir() else set()


# What a port's ios_front.m answers beyond the shell's frame rate: what its own runtime does not have (its Mac frontend.m
# or its Mac-only files have it). Pokémon Legends: Arceus keeps its 16:9 picture with bars, its own HUD (not hud_panel.inc)
# and controllers-only gamepad.m.
FRONT_OWN = {'acnh': ('runtime_display_max_hz',), 'odyssey': ('runtime_display_max_hz',), 'mk8dx': ('runtime_display_max_hz',),
             'aocx': ('runtime_display_max_hz',),
             'pla': ('runtime_aspect_view', 'mtl_hud_safe_area', 'runtime_keyboard_capture', 'runtime_motion_phone',
                     'runtime_shortcut_help_toggle')}


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
        zelda = re.search(r"app in \(([^)]*)\)\)", (ROOT / 'games/totk/ios/tools/build_ipa.py').read_text()).group(1)
        for app in ('totk', 'botw', *PORTS):
            self.assertIn(f"'{app}'", zelda, 'pane_for gives every port the picture rows')

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
                for name in ('runtime_phone_fps', 'runtime_phone_fps_set', *FRONT_OWN.get(game, ())):
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
                self.assertRegex(start, r'(warm_observed\(\) \|\| )?g_par_valid \? @\[\] :\s*(cache_names\(totk_rt_dir\(\)\)|\[\[NSFileManager\.defaultManager contentsOfDirectoryAtPath:totk_rt_dir\(\))',
                                 'no cache-wide libraries')
                self.assertIn('if (g_warm_pipelines_only || !warm_available(c[1])) continue;', gfx, 'no S rows')
                worker = gfx[gfx.index('MLOG("后台预热：着色器库用'):gfx.index('int mtl_warm_wait(int timeout_ms) {')]
                self.assertIn('par_write_in_place = 1;', worker, 'what the archive lacked is written by the warm-up')
                self.assertRegex(worker, r'(warm_observed\(\) \|\| )?g_par_valid \? @\[\] :[^\n]*\n?\s*(cache_names\(rt\)|\[\[NSFileManager\.defaultManager contentsOfDirectoryAtPath:rt)')
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
                self.assertRegex(make, rf'xcrun clang (\$\(MACOS_MIN\) )?-std=c11 \$\(NATIVE_OPT\) \$\(GAME_INFO\)')
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


class PhoneCodeHoldsNoHostAddress(unittest.TestCase):
    """The phone's game code is patched on the Mac and embedded (export-ios): a patch that writes this process's address
    of a host function into the text sends the device to a Mac address. Odyssey's key 3 scene-size veneer did, and the
    iPad crashed about 20 s in (2026-11-21). As Zelda's game_patches_botw.c, such a patch stays out of the phone's code."""

    def test_odyssey_scene_pick_is_mac_only(self):
        src = (ROOT / 'games/odyssey/runtime/game_patches.c').read_text()
        self.assertIn('int phone_code = TARGET_OS_IPHONE || getenv("TOTK_EXPORT_IOS") != NULL;', src)
        self.assertIn('if (!phone_code) patch_scene_pick(main_module);', src)
        self.assertEqual(src.count('patch_scene_pick(main_module)'), 1, 'called only behind the phone check')

    def test_odyssey_render_size_is_mac_only(self):
        # the export took the Mac's 2560x1440 (romfs-patches/render-res.txt) into the phone's code; the iPad froze
        src = (ROOT / 'games/odyssey/runtime/game_patches.c').read_text()
        body = src[src.index('static int render_size('):]
        body = body[:body.index('\n}\n')]
        self.assertIn('if (TARGET_OS_IPHONE || getenv("TOTK_EXPORT_IOS")) { *w = 0; *h = 0;', body)
        self.assertLess(body.index('TOTK_EXPORT_IOS'), body.index('getenv("TOTK_RENDER_RES")'), 'before any Mac setting is read')


class SharedScheduler(unittest.TestCase):
    """Super Mario Odyssey runs on Tears of the Kingdom's guest-thread scheduler, shared (user 2026-11-21: Odyssey's sound
    stuttered on the iPad, "统一 … 和塞尔达 统一 写测试保证"). Its older copy lacked moving a waiting thread to an idle core
    (core_migrate) and priority inheritance; it kept its own 2 ms preemption, which the shared one keeps on for Odyssey."""

    def test_one_scheduler(self):
        shared = ROOT / 'shared/runtime/threads.c'
        for game in ('totk', 'odyssey'):
            link = ROOT / 'games' / game / 'runtime/threads.c'
            self.assertTrue(link.is_symlink() and link.resolve() == shared.resolve(), game)
            stats = ROOT / 'games' / game / 'third_party/d2r_gfx/gfx/nvn/include/dm_wait_stats.h'
            self.assertTrue(stats.is_symlink() and stats.resolve() == (ROOT / 'shared/third_party/d2r_gfx/gfx/nvn/include/dm_wait_stats.h').resolve(), game)
        src = shared.read_text()
        self.assertIn('static int core_migrate(thr_t *w)', src)
        self.assertIn('#if TARGET_OS_IPHONE && !defined(GAME_ODYSSEY)', src, 'Odyssey preempts on the phone too, as before')
        self.assertIn('int runtime_guest_code_pc(uint64_t pc)', (ROOT / 'games/odyssey/runtime/loader.c').read_text())

PLA = ROOT / 'games/pla'


def load_pla_profile():
    import importlib.util, sys
    sys.path.insert(0, str(TOOLS))
    spec = importlib.util.spec_from_file_location('pla_ios_profile', PLA / 'ios/tools/build_ipa.py')
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod


class PlaPort(unittest.TestCase):
    """Pokémon Legends: Arceus (games/pla, the Zelda line through Bayonetta 3's copy) on Zelda's app, build, installer and
    seed folder, as Animal Crossing and Super Mario Odyssey (user 2026-11-21: "统一 … 用测试保证"). What is its own: its
    hooks into the game's text jump to host functions through a slot of the module's key page (the phone's code holds no
    Mac address), the Mac runtime's MSL fixes go into the device's shader sources before they are compiled, and its one
    stand-in shared font travels with the app."""

    def test_phone_code_holds_no_host_address(self):
        loader = (PLA / 'runtime/loader.c').read_text()
        body = loader[loader.index('int runtime_hook_branch(unsigned slot,uint8_t *at,const void *target,const char *what) {'):]
        body = body[:body.index('\n}\n')]
        self.assertIn('0xf9400210u', body, 'ldr x16, [x16, #slot]: the address is loaded, not encoded')
        self.assertIn('memcpy((void *)slot_at,&v,8);', body, 'the slot is written on the Mac and on the device')
        self.assertLess(body.index('#if TARGET_OS_IPHONE'), body.index('memcmp(at,w,sizeof w)'), 'the device only checks the words')
        for name, slot in (('font_hook.c', 'HOOK_SLOT_FONT'), ('audio_sdk.c', 'HOOK_SLOT_BANK_BOOTSTRAP')):
            with self.subTest(name=name):
                self.assertIn(f'runtime_hook_branch({slot},', (PLA / 'runtime' / name).read_text())
        self.assertNotIn('emit_adrp_br', (PLA / 'runtime/font_hook.c').read_text(), 'no adrp to a host function')
        audio = (PLA / 'runtime/audio_sdk.c').read_text()
        bank = audio[audio.index('void runtime_audio_bank_bootstrap_install(void) {'):]
        self.assertNotIn('emit_adrp_br', bank[:bank.index('\n}\n')], 'the default-on bootstrap: no adrp to a host function')
        patches = (PLA / 'runtime/game_patches.c').read_text()
        self.assertIn('int phone_code = TARGET_OS_IPHONE || getenv("TOTK_EXPORT_IOS") != NULL;', patches)
        self.assertIn('int mode = phone_code ? 0 : pla_realtime_mode();', patches, 'no 60 fps track (code rewritten at run time)')
        self.assertIn('const char *list = phone_code ? NULL : getenv("PLA_PATCHES");', patches, 'no Mac exefs patches')
        self.assertIn('if (!TARGET_OS_IPHONE) apply_compat(main_module);', patches, 'the device\'s text is read-only')

    def test_device_libraries_carry_the_mac_msl_fixes(self):
        gfx = (PLA / NVN / 'nvn_mtl.m').read_text()
        tool = gfx[gfx.index('int mtl_msl_rewrite_tool(const char *folder) {'):]
        tool = tool[:tool.index('\n}\n')]
        self.assertIn('msl_init_vertex_outputs(src) : msl_zero_unwritten_colors(src)', tool, 'the fixes shader_load makes')
        self.assertIn('already seeded', gfx, 'the vertex fix leaves a fixed source as it is')
        self.assertIn('if(argc==4&&!strcmp(argv[2],"msl-rewrite"))return mtl_msl_rewrite_tool(argv[3]);', (PLA / 'runtime/loader.c').read_text())
        mod = load_pla_profile()
        self.assertIs(mod.PORT.sources_fix, mod.sources_fix)
        calls = []
        mod.sources_fix(type('L', (), {'host': Path('/h'), 'rewritten': Path('/r')})(), Path('/s/seed'), lambda *a: calls.append(a), Path('/l'), {})
        self.assertEqual(calls[0][0], [Path('/h'), Path('/r'), 'msl-rewrite', Path('/s/seed')])
        self.assertIsNone(load_tool('build_port_ipa').Port.__dataclass_fields__['sources_fix'].default, 'the other ports: as translated')

    def test_fixed_sources_are_named_by_their_new_hashes(self):
        import hashlib, json, tempfile
        port = load_tool('build_port_ipa')
        with tempfile.TemporaryDirectory() as d:
            seed, cache, out = Path(d, 'seed'), Path(d, 'cache'), Path(d, 'out')
            seed.mkdir(); cache.mkdir()
            shader = 'a' * 64 + '-' + 'b' * 12 + '_b.metal'
            (seed / shader).write_text('raw'); (seed / 'seed-manifest.json').write_text(json.dumps({'files': {shader: 'x'}}))
            def fix(folder):
                (folder / shader).unlink(); (folder / shader).write_text('fixed')
            first = port.seed_with_mac_cache(seed, cache, out, fix=fix, fix_id='1')
            files = json.loads((out / 'seed-manifest.json').read_text())['files']
            self.assertEqual(files[shader], hashlib.sha256(b'fixed').hexdigest())
            self.assertEqual((seed / shader).read_text(), 'raw', 'the prepared seed is not written through')
            self.assertEqual(port.seed_with_mac_cache(seed, cache, out, fix=fix, fix_id='1'), first, 'kept')
            (out / 'inputs.txt').write_text('other\n'); stamp = (out / 'inputs.txt').read_text()
            port.seed_with_mac_cache(seed, cache, out, fix=fix, fix_id='2')
            self.assertNotEqual((out / 'inputs.txt').read_text(), stamp, 'another fix makes it again')

    def test_the_shell_and_its_font(self):
        main = (ROOT / 'games/totk/ios/Sources/main.m').read_text()
        self.assertIn('mng_active_start(@"pla", mng_active_version());', main)
        block = main[main.index('#ifdef GAME_PLA'):]
        block = block[:block.index('#endif')]
        mod = load_pla_profile()
        self.assertIn(f'@"shared-fonts/{mod.STAND_IN}"', block, 'the font the profile puts in GameData/shared-fonts')
        self.assertIn('setenv("PLA_SHARED_FONT_TTF"', block)
        self.assertIn('setenv("PLA_REALTIME", "0", 1)', block)
        self.assertIn('getenv("PLA_SHARED_FONT_TTF")', (PLA / 'runtime/shared_font.c').read_text())
        self.assertIn('#define GAME_FOLDER "PLA"', (PLA / 'runtime/game_version.h').read_text(), 'main.m names its shader rows')
        self.assertIn('PP_GAME_PLA = PP_GAME_ODYSSEY } pp_game;', (ROOT / 'games/totk/ios/Sources/picture_profile.h').read_text())
        features = (ROOT / 'games/totk/ios/Sources/port_features.h').read_text()
        self.assertRegex(features, r'#if [^\n]*defined\(GAME_PLA\)\n#define SHELL_FPS_CHOICE 0', 'its own 30 fps step')
        import plistlib
        totk = load_tool('build_ipa')
        root = totk.pane_for(plistlib.loads((ROOT / 'games/totk/ios/Settings.bundle/Root.plist').read_bytes()), 'pla')
        mod.pane(root['PreferenceSpecifiers'])
        keys = {x.get('Key') for x in root['PreferenceSpecifiers']} - {None}
        self.assertTrue({'TOTK_LANG', 'TOTK_PICTURE_PROFILE', 'TOTK_METALFX'} <= keys)
        self.assertFalse(keys & {'TOTK_CLOUD_SAVE_PICK', 'TOTK_CONTRIBUTE_SHADERS'})

    def test_its_icon_and_layout(self):
        mod = load_pla_profile()
        L = mod.layout(Path('/w'))
        self.assertIsNone(L.icon, 'build/AppIcon.icns, the Makefile\'s (scripts/extract_app_icon.py)')
        self.assertEqual((L.host, L.rewritten, L.romfs), (Path('/w/build/pla-native'), Path('/w/rewritten/v1101'), Path('/w/extracted/v1101/romfs')))
        self.assertEqual(mod.CHAIN_CACHE.split('rt-')[1], re.search(r'#define DMSH_CHAIN "([^"]+)"', (PLA / NVN / 'dmsh_chain.h').read_text()).group(1))


MK8DX = ROOT / 'games/mk8dx'


def load_mk8dx_profile():
    import importlib.util, sys
    sys.path.insert(0, str(TOOLS))
    spec = importlib.util.spec_from_file_location('mk8dx_ios_profile', MK8DX / 'ios/tools/build_ipa.py')
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod


class Mk8dxPort(unittest.TestCase):
    """Mario Kart 8 Deluxe on the Zelda line (games/mk8dx, Super Mario Odyssey's runtime; no nxnative code: games/mk8d is
    not used) on Zelda's app, build, installer and seed folder, as the other ports (the maintainer 2026-11-21: "统一 …
    用测试保证"). What is its own: the Mii parts of the player's firmware and the Booster Course Pass's files travel in
    GameData, and its seed is Odyssey's seed tool run on this game."""

    def test_its_layout_and_game_data(self):
        mod = load_mk8dx_profile()
        L = mod.layout(Path('/w'))
        self.assertEqual((L.host, L.rewritten, L.romfs, L.fonts, L.seed),
                         (Path('/w/build/mk8dx-native'), Path('/w/rewritten/local'), Path('/w/extracted/local/romfs'),
                          Path('/w/extracted/firmware/shared-fonts'), Path('/w/build/shader-seed')))
        self.assertEqual(L.icon, Path('/w/extracted/local/icon.jpg'), 'the icon of its control data')
        self.assertEqual(mod.CHAIN_CACHE.split('rt-')[1], re.search(r'#define DMSH_CHAIN "([^"]+)"', (MK8DX / NVN / 'dmsh_chain.h').read_text()).group(1))
        self.assertIs(mod.PORT.game_data, mod.game_data)
        self.assertNotIn('nxnative', (MK8DX / 'ios/tools/build_ipa.py').read_text().replace('no nxnative', ''))
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            work, res = Path(d, 'work'), Path(d, 'GameData')
            for rel in mod.GAME_DATA.values(): (work / rel / 'x').mkdir(parents=True); (work / rel / 'x/f.dat').write_text(rel)
            mod.game_data(res, work, L, [])
            self.assertEqual({str(p.relative_to(res)) for p in res.rglob('*') if p.is_file()}, {f'{n}/x/f.dat' for n in mod.GAME_DATA})

    def test_the_shell_reads_its_game_data(self):
        main = (ROOT / 'games/totk/ios/Sources/main.m').read_text()
        block = main[main.index('#ifdef GAME_MK8DX'):]
        block = block[:block.index('#endif')]
        mod = load_mk8dx_profile()
        self.assertEqual(set(mod.GAME_DATA), {'mii-model', 'aoc'})
        self.assertIn('setenv("TOTK_MII_MODEL", [res stringByAppendingPathComponent:@"mii-model"]', block)
        self.assertIn('setenv("TOTK_AOC_DIR", [res stringByAppendingPathComponent:@"aoc"]', block)
        self.assertIn('getenv("TOTK_MII_MODEL")', (MK8DX / 'runtime/mii_model.c').read_text())
        self.assertIn('getenv("TOTK_AOC_DIR")', (MK8DX / 'runtime/fs_host.c').read_text())
        # the active players' id: GAME_INBOX_ID of its game_version.h, which main.m reports first
        self.assertIn('#define GAME_INBOX_ID "mk8dx"', (MK8DX / 'runtime/game_version.h').read_text())
        self.assertLess(main.index('#ifdef GAME_INBOX_ID'), main.index('mng_active_start(@"totk"'))
        # no shader inbox, as Animal Crossing and Pokémon Legends: Arceus
        self.assertEqual(main.count('defined(GAME_PLA) || defined(GAME_MK8DX)'), 3)
        self.assertIn('PP_GAME_MK8DX = PP_GAME_ODYSSEY,', (ROOT / 'games/totk/ios/Sources/picture_profile.h').read_text())
        # the discarded nxnative-based iPhone attempt (games/mk8d/ios, removed) left no branch in the shell
        for f in [*(ROOT / 'games/totk/ios/Sources').glob('*.[hmc]'), ROOT / 'games/totk/ios/tools/build_ipa.py']:
            self.assertNotRegex(f.read_text(), r"\bGAME_MK8D\b|'mk8d'", f.name)
        self.assertRegex((ROOT / 'games/totk/ios/Sources/port_features.h').read_text(), r'#if [^\n]*defined\(GAME_MK8DX\)[^\n]*\n#define SHELL_FPS_CHOICE 0')
        import plistlib
        root = load_tool('build_ipa').pane_for(plistlib.loads((ROOT / 'games/totk/ios/Settings.bundle/Root.plist').read_bytes()), 'mk8dx')
        mod.pane(root['PreferenceSpecifiers'])
        keys = {x.get('Key') for x in root['PreferenceSpecifiers']} - {None}
        self.assertTrue({'TOTK_LANG', 'TOTK_PICTURE_PROFILE', 'TOTK_METALFX'} <= keys)
        self.assertFalse(keys & {'TOTK_CLOUD_SAVE_PICK', 'TOTK_CONTRIBUTE_SHADERS'})

    def test_phone_code_holds_no_host_address(self):
        # its game patches write nothing into the game's text (the iOS export embeds that text): no host address, no
        # Mac-only switch (PhoneCodeHoldsNoHostAddress)
        patches = (MK8DX / 'runtime/game_patches.c').read_text()
        body = patches[patches.index('void runtime_game_patches(Module *main_module) {'):]
        body = body[:body.index('\n}\n')]
        self.assertIn('(void)main_module;', body)
        for word in ('patch_', 'emit_', 'memcpy', 'getenv'):
            self.assertNotIn(word, body)

    def test_the_phone_schedules_as_its_mac(self):
        # Odyssey's runtime and audio renderer: the asynchronous preemption on, on the phone as on the Mac
        self.assertIn('#if TARGET_OS_IPHONE && !defined(GAME_ODYSSEY) && !defined(GAME_MK8DX)', (ROOT / 'shared/runtime/threads.c').read_text())
        link = MK8DX / 'runtime/threads.c'
        self.assertTrue(link.is_symlink() and link.resolve() == (ROOT / 'shared/runtime/threads.c').resolve())
        self.assertIn('int runtime_guest_code_pc(uint64_t pc)', (MK8DX / 'runtime/loader.c').read_text())

    def test_its_seed_is_odysseys_tool(self):
        import importlib.util
        tool = MK8DX / 'scripts/prepare_mk8dx_shader_seed.py'
        text = tool.read_text()
        self.assertIn("GAME.parent / 'odyssey/scripts/prepare_odyssey_shader_seed.py'", text, 'the tool is run, not copied')
        self.assertFalse((MK8DX / 'scripts/prepare_shader_seed.py').exists())
        spec = importlib.util.spec_from_file_location('mk8dx_seed', tool)
        mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
        import json, tempfile
        with tempfile.TemporaryDirectory() as d:
            cache = Path(d)
            rows = {'vs': [{'variant': 'a-1', 'deps': {'attr': {'6': 'uint', '0': 'float'}}}],
                    'ps': [{'variant': 'b-1', 'deps': {'tex': {'0': '*'}, 'fout': {'0': 'uint'}}}]}
            for i, (stage, rs) in enumerate(rows.items()):
                (cache / f'{i}.variants').write_text(json.dumps(rs))
                (cache / f'{rs[0]["variant"]}_b.json').write_text(json.dumps({'stage': stage}))
            self.assertEqual(mod.rules_from_cache(cache), {'vs': [{'attr': {'6': 'uint'}}]})
        # its UI layout archives name no members (SFAT attributes 0): read by hash, other refusals stand
        import struct
        def archive(attribute, hi=4):
            body = struct.pack('<4sHHIIHH', b'SARC', 0x14, 0xfeff, 0x3c, 0x38, 0x100, 0) + struct.pack('<4sHHI', b'SFAT', 0xc, 1, 0x65)
            body += struct.pack('<4I', 0xabc, attribute, 0, hi) + struct.pack('<4sHH', b'SFNT', 8, 0)
            return body + b'data'
        def strict(data): raise ValueError('invalid SARC member')
        read = mod.unnamed_sarc(strict)
        self.assertEqual(read(archive(0)), {'#00000abc': b'data'})
        for bad in (archive(0x1000000), archive(0, hi=9)):
            with self.assertRaises(ValueError): read(bad)


class PhoneFontsAreOpen(unittest.TestCase):
    def test_no_macos_system_font_goes_into_a_phone_app(self):
        # user 2026-11-20: without firmware fonts, the open-source fonts (tools/deps/open_fonts.py); Arceus's first phone
        # build had copied macOS's Arial Unicode into the app (2026-11-21)
        for f in sorted(ROOT.glob('games/*/ios/tools/*.py')):
            with self.subTest(f=str(f.relative_to(ROOT))):
                self.assertNotIn('/System/Library/Fonts', f.read_text())
        self.assertIn('from tools.deps.open_fonts import open_fonts', (ROOT / 'games/pla/ios/tools/build_ipa.py').read_text())


class PlaPhoneMapping(unittest.TestCase):
    def test_phone_maps_the_room_behind_the_image_as_the_mac(self):
        # Arceus's relative relocations past its image need zero-filled room behind it; the phone's mapping lacked it
        # and stopped at "relative addend" (iPhone 14, 2026-11-21): one sizing rule for both
        src = (ROOT / 'games/pla/runtime/loader.c').read_text()
        self.assertEqual(src.count('m->size=module_map_size(m);'), 2, 'the Mac and the phone paths')
        self.assertNotIn('m->size=page_up(m->file_size)', src)


class PortEntitlements(unittest.TestCase):
    """A port may ask for entitlements beyond the increased memory limit (Info.plist MNGEntitlements, written from
    Port.entitlements; install_device.py requests them in the profile and signs with them). Arceus asks for extended
    virtual addressing: its 32 GiB address region did not fit the phone's default address space (2026-11-21)."""

    def test_only_arceus_asks_and_the_installer_honours_it(self):
        build = SHARED_BUILD.read_text()
        self.assertIn("entitlements: tuple = ()", build, 'the other ports ask for nothing more')
        self.assertIn("MNGEntitlements", build)
        self.assertIn("entitlements=('com.apple.developer.kernel.extended-virtual-addressing',)", (ROOT / 'games/pla/ios/tools/build_ipa.py').read_text())
        for g in ('acnh', 'odyssey', 'mk8dx', 'aocx'):
            self.assertNotIn('entitlements=', (ROOT / 'games' / g / 'ios/tools/build_ipa.py').read_text(), g)
        inst = (ROOT / 'games/totk/ios/tools/install_device.py').read_text()
        self.assertIn('extra = wanted_entitlements(a.ipa)', inst)
        self.assertIn("plistlib.dumps({ENTITLEMENT: True, **{k: True for k in extra}})", inst)
        self.assertIn("'get-task-allow', ENTITLEMENT, *extra) if k in ents", inst)

    def test_arceus_reserves_its_region_where_the_phone_can(self):
        src = (ROOT / 'games/pla/runtime/services.c').read_text()
        self.assertIn('TARGET_OS_IPHONE?VM_FLAGS_ANYWHERE:VM_FLAGS_FIXED', src)
        self.assertIn('#define VAMM_SPACE_AT ((uint64_t)(uintptr_t)vamm_space)', src)
        ios = src[src.index('static void memory_init(void) {'):]
        ios = ios[:ios.index('#else')]
        self.assertLess(ios.index('VM_FLAGS_ANYWHERE'), ios.index('alias_requested()'), 'the VAMM region before the alias region shrinks')


class PlaOfflineConsole(unittest.TestCase):
    def test_arceus_answers_the_exit_section_from_the_shared_offline_host(self):
        # saving enters nn::oe's exit-request section; the SDK's own read a missing applet object and the iPhone crashed
        # converting an older save (2026-11-21): the shared offline console answers it, after Arceus's own entries
        link = ROOT / 'games/pla/runtime/offline_host.c'
        self.assertTrue(link.is_symlink() and link.resolve() == (ROOT / 'shared/runtime/offline_host.c').resolve())
        self.assertIn('defined(GAME_PLA)', (ROOT / 'shared/runtime/offline_host.c').read_text())
        src = (ROOT / 'games/pla/runtime/services.c').read_text()
        self.assertIn('return runtime_offline_override(name);', src)


class PhoneFrontAnswersTheMacsDisplayCalls(unittest.TestCase):
    def test_every_vi_call_a_mac_front_answers_the_phone_answers(self):
        # Mario Kart 8 Deluxe crashed at boot on the iPhone in nn::vi::SetLayerScalingMode: its Mac front end answers it,
        # the shared phone front end did not (2026-11-22). Every vi entry of a port's Mac frontend.m is in the phone's.
        import re
        phone = (ROOT / 'shared/runtime/frontend_ios.m').read_text()
        for game in ('acnh', 'odyssey', 'mk8dx', 'pla', 'aocx'):
            mac = ROOT / 'games' / game / 'runtime/frontend.m'
            if not mac.is_file(): continue
            for name in sorted(set(re.findall(r'"(_ZN2nn2vi\w+)"', mac.read_text()))):
                with self.subTest(game=game, name=name):
                    self.assertIn(name, phone)


AOCX = ROOT / 'games/aocx'


def load_aocx_profile():
    import importlib.util, sys
    sys.path.insert(0, str(TOOLS))
    spec = importlib.util.spec_from_file_location('aocx_ios_profile', AOCX / 'ios/tools/build_ipa.py')
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod


class AocxPort(unittest.TestCase):
    """Hyrule Warriors: Age of Calamity 1.4.1 on the Zelda line (games/aocx, Red Dead Redemption's runtime; no nxnative code)
    on Zelda's app, build, installer and seed folder, as the other ports (the maintainer 2026-11-22: "统一 … 用测试保证").
    What is its own: the add-on content in GameData, no font in the app, the host's VP9 movies (libvpx for the device), its
    own 30 fps, and a seed read from its RDB data files and main (scripts/ktgl_shaders.py, prepare_aocx_shader_seed.py)."""

    def test_its_layout_icon_and_libraries(self):
        mod = load_aocx_profile()
        L = mod.layout(Path('/w'))
        self.assertEqual((L.host, L.rewritten, L.romfs, L.seed, L.version),
                         (Path('/w/build/aocx-native'), Path('/w/rewritten/local'), Path('/w/extracted/local/romfs'),
                          Path('/w/build/shader-seed'), '1.4.1'))
        self.assertIsNone(L.icon, 'build/AppIcon.icns, from its control data (scripts/build_aocx.py)')
        self.assertIn("extract_app_icon.py", (AOCX / 'scripts/build_aocx.py').read_text())
        self.assertEqual(mod.PORT.libs, ('libopus.a', 'libvpx.a'), 'its VP9 movies are decoded by the host (libvpx)')
        self.assertEqual(mod.CHAIN_CACHE.split('rt-')[1], re.search(r'#define DMSH_CHAIN "([^"]+)"', (AOCX / NVN / 'dmsh_chain.h').read_text()).group(1))
        self.assertNotIn('nxnative', (AOCX / 'ios/tools/build_ipa.py').read_text())

    def test_add_on_content_and_no_font_in_the_app(self):
        import tempfile
        mod = load_aocx_profile()
        with tempfile.TemporaryDirectory() as d:
            work = Path(d); L = mod.layout(work)
            L.fonts.mkdir(); (L.fonts / 'old.ttf').write_text('x')
            mod.prepare(L, None, {}, None, None, work)
            self.assertEqual(list(L.fonts.iterdir()), [], 'no macOS or firmware font: the game draws with its own')
            aoc = work / 'extracted/aoc'; (aoc / '2/asset').mkdir(parents=True)
            (aoc / '2/asset/a.bin').write_text('a'); (aoc / '2/DLCDATA.dat').write_text('d'); (aoc / '2.json').write_text('{}')
            res = work / 'res'
            mod.game_data(res, work, L, None)
            self.assertEqual(sorted(str(p.relative_to(res)) for p in res.rglob('*') if p.is_file()),
                             ['aoc/2/DLCDATA.dat', 'aoc/2/asset/a.bin'], 'the add-ons, not their reports')
            with self.assertRaises(RuntimeError):
                mod.check(L)

    def test_the_shell_knows_it(self):
        main = (ROOT / 'games/totk/ios/Sources/main.m').read_text()
        block = main[main.index('#ifdef GAME_AOCX'):]
        block = block[:block.index('#endif')]
        self.assertIn('setenv("TOTK_ROMFS_PATCHES", "0", 1); setenv("TOTK_ROMFS_MODS", "0", 1);', block)
        self.assertIn('setenv("TOTK_AOC_DIR", [res stringByAppendingPathComponent:@"aoc"]', block)
        self.assertIn('#define GAME_INBOX_ID "aocx"', (AOCX / 'runtime/game_version.h').read_text(), 'its active players\' id')
        self.assertEqual(main.count('defined(GAME_RDR) || defined(GAME_AOCX)'), 3, 'no shader inbox')
        self.assertIn('#if !defined(GAME_AOCX) && !defined(GAME_ACNH)', main, 'no contribution prune')
        self.assertRegex((ROOT / 'games/totk/ios/Sources/port_features.h').read_text(),
                         r'#if [^\n]*defined\(GAME_AOCX\)[^\n]*\n#define SHELL_FPS_CHOICE 0')
        self.assertIn('PP_GAME_AOCX = PP_GAME_ODYSSEY,', (ROOT / 'games/totk/ios/Sources/picture_profile.h').read_text())
        self.assertIn('int runtime_phone_fps(void) { return 30; }', (AOCX / 'ios/Sources/ios_front.m').read_text())
        import plistlib
        mod = load_aocx_profile()
        root = load_tool('build_ipa').pane_for(plistlib.loads((ROOT / 'games/totk/ios/Settings.bundle/Root.plist').read_bytes()), 'aocx')
        mod.pane(root['PreferenceSpecifiers'])
        keys = {x.get('Key') for x in root['PreferenceSpecifiers']} - {None}
        self.assertTrue({'TOTK_LANG', 'TOTK_PICTURE_PROFILE', 'TOTK_METALFX'} <= keys)
        self.assertFalse(keys & {'TOTK_CLOUD_SAVE_PICK', 'TOTK_CONTRIBUTE_SHADERS'})

    def test_phone_code_holds_no_host_address(self):
        patches = (AOCX / 'runtime/game_patches.c').read_text()
        body = patches[patches.index('void runtime_game_patches(Module *main_module) {'):]
        body = body[:body.index('\n}\n')]
        self.assertIn('(void)main_module;', body)
        for word in ('patch_', 'emit_', 'memcpy', 'getenv'):
            self.assertNotIn(word, body)

    def test_the_phone_schedules_as_its_mac(self):
        self.assertIn('&& !defined(GAME_RDR) && !defined(GAME_AOCX)', (ROOT / 'shared/runtime/threads.c').read_text())
        for name in ('threads.c', 'offline_host.c', 'movie_host.c', 'movie_pace.h', 'audio_out.c'):
            link = AOCX / 'runtime' / name
            self.assertTrue(link.is_symlink() and link.resolve() == (ROOT / 'shared/runtime' / name).resolve(), name)
        self.assertIn('defined(GAME_AOCX)', (ROOT / 'shared/runtime/offline_host.c').read_text())

    def test_its_seed_is_the_zelda_lines_tool(self):
        tool = (AOCX / 'scripts/prepare_aocx_shader_seed.py').read_text()
        self.assertIn("GAME.parent / 'odyssey/scripts/prepare_odyssey_shader_seed.py'", tool, 'the tool is run, not copied')
        self.assertIn('depth_minus_w=False', tool, 'its NVN 0..1 depth')
        self.assertIn('import ktgl_shaders', tool)
        self.assertIn('prepare_aocx_shader_seed.py', (AOCX / 'scripts/build_aocx.py').read_text())


if __name__ == '__main__':
    unittest.main()

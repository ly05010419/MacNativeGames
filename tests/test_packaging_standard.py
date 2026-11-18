"""The packaging standards every game must meet (CLAUDE.md, "每个游戏都要满足的标准"), checked in the sources.

Each game's packaging is its own copy of the code, so an optimization made for one game does not reach the others by itself.
A new games/<name>/build.py fails here until it is added to PATHS. A standard a path does not meet must be listed in its gaps
with the reason; a gap that has been closed must be removed, so the table stays true.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(*names):
    return '\n'.join((ROOT / n).read_text(errors='replace') for n in names if (ROOT / n).is_file())


def seed_identity_ignores_runtime(text):
    """The translation cache's identity must not contain the runtime binary's hash."""
    body = re.search(r'def translation_context\(.*?\n    return ', text, re.S)
    return body is None or 'sha256(binary)' not in body.group(0)


STANDARDS = {
    'shader data in its own signed bundle': lambda t: 'ShaderData-' in t,
    'pipeline archive built while packaging': lambda t: 'pipeline-archive' in t or 'mac_pipelines' in t,
    # every game comes to the title once the warm-up is done, archive complete or not (user 2026-11-11: "所有塞尔达 都是 预热玩
    # 进标题！", "所有游戏都是"); the Mac shortcut that skipped the wait with a complete archive (2026-10-29) is gone
    'title after the warm-up': lambda t: ('TOTK_WARM_BEFORE_TITLE' in t or 'setenv("DM_PSO_PREPARE", "1", 1)' in t) and '标题不等预热读完' not in t,
    'no program runs inside a new app': lambda t: re.search(r"run_outside_bundle|work/'build/oot-native'|mac_pipelines\(build/'tools/dmhost'", t) is not None,
    'seed and library digests are reused': lambda t: 'seed_digests' in t or 'record_verified' in t or 'class Digests:' in t,
    'repackaging keeps the .app folder': lambda t: 'install_app(' in t and re.search(r'(partial|temporary)\.rename\(out\)', t) is None,
    'a runtime change can update quickly': lambda t: 'place_runtime' in t or 'update_ocarina' in t or 'def update(app, work):' in t,
    'a runtime change keeps the translations': seed_identity_ignores_runtime,
    # the runtime's warm-up, its contribution filter and the launcher's prune read pso_title.tsv / pso_manifest.tsv beside the
    # shader map (build/): Ocarina of Time's were only in the seed, so every pipeline it drew was contributed (2026-11-10)
    'warm-up lists beside the shader map': lambda t: "'build/pso_manifest.tsv'" in t or "check_archive_lists(resources/'pipeline-archive', resources/'build')" in t or "check_archive_lists(resources/'pipeline-archive', resources)" in t,
    # the pipeline archive is built from those same build/ lists: Ocarina of Time's fast update built it from the seed's
    # older list, 2,957 of the App's 7,516 render pipelines, and 4,559 were compiled at the first start (2026-11-11)
    'pipeline archive built from the App\'s lists': lambda t: (
        'check_archive_lists(' in t or "lists = [p for p in (build / 'pso_title.tsv', build / 'pso_manifest.tsv'" in t),
}

GFX = 'games/{}/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m'

# What the three ports MNG Launcher builds through tools/port_app (Animal Crossing, Echoes of Wisdom, Super Mario
# Odyssey) do not do yet. Seven of the eight are the same for all three because the packaging they share is the same;
# the eighth differs and is below. Read in the sources 2026-11-18, each reason naming what shows it. None seen in play.
PORT_APP_GAPS = {
    'shader data in its own signed bundle': 'no file of these ports names ShaderData-: the shader data is inside the '
        'app, not in a bundle of its own',
    'pipeline archive built while packaging': 'no packager here builds one: mac_pipelines is nowhere in these ports, '
        'and the only pipeline-archive they name is the vendored runtime header totk_pipeline_archive.h (Echoes of '
        'Wisdom does not name it at all)',
    'title after the warm-up': 'neither TOTK_WARM_BEFORE_TITLE nor DM_PSO_PREPARE is set anywhere in these ports, so '
        'nothing holds the title until the warm-up is done',
    'seed and library digests are reused': "only Odyssey's vendored third_party/totk-scripts/prepare_shader_seed.py "
        'has seed_digests/record_verified, and its own packaging path does not call it',
    'repackaging keeps the .app folder': 'tools/port_app/bundle.py staged() does shutil.rmtree(output) and then '
        'stage.rename(output): every repackaging replaces the .app folder instead of installing into it, which is why '
        'register() has to make Finder and the Dock read the icon again',
    'warm-up lists beside the shader map': 'no packager writes build/pso_title.tsv and build/pso_manifest.tsv beside '
        'the shader map, nor checks them (check_archive_lists)',
    "pipeline archive built from the App's lists": 'there is no pipeline archive to build from them (the second gap)',
}

# The eighth, per port: two of the three run a program inside the app they have just made.
PORT_APP_RUNS_INSIDE = {
    'acnh': "package_native_app.py's assemble() ends with bundle.check_launcher(launcher_bin), the launcher it "
            'compiled into Contents/MacOS of the staged bundle, run there right after signing it',
    'eow': "package_app.py's assemble() ends with bundle.check_launcher(macos / EXECUTABLE): the app's own launcher, "
           'run inside the bundle it has just signed',
    'odyssey': 'this packager runs nothing inside the app, but no name of its path says so in the form this check '
               'knows (run_outside_bundle)',
}


def port_app_gaps(game):
    return {**PORT_APP_GAPS, 'no program runs inside a new app': PORT_APP_RUNS_INSIDE[game]}

# name -> (the sources of that packaging path, {standard not met: why})
PATHS = {
    'totk': (['build_app.py', 'update_runtime.py', 'games/totk/scripts/prepare_shader_seed.py', GFX.format('totk')], {}),
    'switch 2 dumps (Bananza, Pokopia)': (
        ['build_app.py', 'update_runtime.py', 'games/totk/scripts/prepare_ns2_seed.py', GFX.format('totk')], {}),
    'oot': (['games/oot/build.py', 'update_runtime.py', 'games/oot/scripts/prepare_ns2_seed.py', GFX.format('oot')], {}),
    # TMNT (PR #2125) also builds its pipeline archive anew on every update, where Tears of the Kingdom's keeps the files whose
    # descriptions did not change (TOTK_PAR_RECHECK, "极速更新"); no check here tells that apart by the sources' words (its own
    # copy of nvn_mtl.m has them), so it is noted, not tested (after PR #2125)
    'tmnt': (['games/tmnt/build.py', 'games/tmnt/scripts/build_private_bundle.py', 'games/tmnt/scripts/package_private.py',
              'games/tmnt/scripts/tmnt_bundle.py', 'games/tmnt/scripts/tmnt_update.py', 'games/tmnt/scripts/tmnt_pipelines.py',
              GFX.format('tmnt')], {}),
    'ss': (['games/ss/build.py', 'games/ss/scripts/package_ss.py', 'games/ss/scripts/ss_bundle.py',
            'games/ss/scripts/ss_pipelines.py', 'games/ss/scripts/ss_update.py', 'games/ss/app/launcher.c'], {}),
    'dread': (['games/dread/build.py', 'games/dread/scripts/package_dread.py', 'games/dread/scripts/dread_bundle.py',
               'games/dread/scripts/dread_pipelines.py', 'games/dread/app/launcher.m'], {}),
    'worldend': (['games/worldend/build.py', 'games/worldend/scripts/build_worldend.py', 'games/worldend/scripts/worldend_bundle.py',
                  'games/worldend/scripts/worldend_pipelines.py', 'games/worldend/scripts/worldend_update.py',
                  'games/worldend/app/launcher.c'], {}),
    # The three ports built through tools/port_app (bundle.py, adapter.py): unlike every path above, their packaging is
    # not its own copy of the code -- the staging, the signature, the Info.plist and the venv are shared, so those two
    # files are read with each of them. Measured 2026-11-18 against the sources, not played.
    'acnh': (['games/acnh/build.py', 'games/acnh/scripts/package_native_app.py',
              'games/acnh/scripts/precompile_cached_shaders.py', 'games/acnh/tools/prepare_shader_seed.py',
              'games/acnh/scripts/acnh_app_launcher.c', GFX.format('acnh'),
              'tools/port_app/bundle.py', 'tools/port_app/adapter.py'], port_app_gaps('acnh')),
    'eow': (['games/eow/build.py', 'games/eow/scripts/package_app.py', 'games/eow/scripts/prepare_runtime.py',
             'games/eow/scripts/build_shader_libs.py', 'games/eow/scripts/prepare_shader_seed.py',
             'games/eow/app/launcher.c', GFX.format('eow'),
             'tools/port_app/bundle.py', 'tools/port_app/adapter.py'], port_app_gaps('eow')),
    'odyssey': (['games/odyssey/build.py', 'games/odyssey/scripts/build_odyssey.py',
                 'games/odyssey/scripts/package_native_app.py', 'games/odyssey/scripts/precompile_cached_shaders.py',
                 'games/odyssey/scripts/prepare_runtime.py', 'games/odyssey/scripts/odyssey_app_launcher.c',
                 GFX.format('odyssey'), 'tools/port_app/bundle.py', 'tools/port_app/adapter.py'], port_app_gaps('odyssey'))
}

BUILD_SCRIPTS = {'games/tmnt/build.py': 'tmnt', 'games/oot/build.py': 'oot', 'games/dread/build.py': 'dread', 'games/ss/build.py': 'ss',
                 'games/worldend/build.py': 'worldend', 'games/acnh/build.py': 'acnh', 'games/eow/build.py': 'eow',
                 'games/odyssey/build.py': 'odyssey'}


class PackagingStandard(unittest.TestCase):
    def test_every_game_with_its_own_build_is_checked(self):
        found = {str(p.relative_to(ROOT)) for p in (ROOT / 'games').glob('*/build.py')}
        unknown = sorted(found - set(BUILD_SCRIPTS))
        self.assertEqual(unknown, [], 'a game with its own build.py: add it to PATHS and BUILD_SCRIPTS, and check it against the standards')
        # the message above asks for both: registering the build.py alone would make this test pass with nothing checked
        for script, name in sorted(BUILD_SCRIPTS.items()):
            self.assertIn(name, PATHS, f'{script} is in BUILD_SCRIPTS but names no path in PATHS: nothing checks it')

    def test_paths_meet_the_standards(self):
        for name, (sources, gaps) in PATHS.items():
            if not (ROOT / sources[0]).is_file():   # a branch without this game
                continue
            text = read(*sources)
            for standard, check in STANDARDS.items():
                with self.subTest(path=name, standard=standard):
                    met = bool(check(text))
                    if standard in gaps:
                        self.assertFalse(met, f'{name} now meets this standard: remove it from the gaps')
                    else:
                        self.assertTrue(met, f'{name} does not meet this standard; meet it, or list it in the gaps with the reason')

    def test_every_switch_2_game_is_planned_for_the_express_update(self):
        # "a runtime change can update quickly" passed for Bananza and Pokopia by its word in update_runtime.py (Tears of the
        # Kingdom's place_runtime) while update_runtime.plan answered "only the normal update" for both (user 2026-11-12:
        # "为什么要完整打包？"); asked of the plan itself, for each game, with its checks passing
        import plistlib
        import sys
        import tempfile
        from unittest import mock
        sys.path.insert(0, str(ROOT))
        import build_app
        import update_runtime
        for g in build_app.NS2.values():
            version = g['version']
            with self.subTest(game=version), tempfile.TemporaryDirectory() as tmp, \
                    mock.patch.object(update_runtime, 'check_game_data'), mock.patch.object(update_runtime, 'check_rewriter'), \
                    mock.patch.object(update_runtime, 'check_seed_chain', return_value=False):
                work = Path(tmp) / 'work'
                work.mkdir()
                app = Path(tmp) / f'{build_app.APPS[version][0]}.app'
                (app / f'Contents/Resources/totk/rewritten/{version}').mkdir(parents=True)
                (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(
                    {'CFBundleExecutable': build_app.APPS[version][2], 'MNGWorkFolder': str(work)}))
                data = app / f'Contents/Resources/totk/build/game-data/{version}'
                data.mkdir(parents=True)
                for name in build_app.game_data_files(version):
                    (data / name).write_text('x')
                self.assertEqual(update_runtime.plan(app)['kind'], 'express')

    def test_gaps_name_real_standards_and_reasons(self):
        for name, (_, gaps) in PATHS.items():
            for standard, why in gaps.items():
                self.assertIn(standard, STANDARDS, name)
                self.assertTrue(why.strip(), name)


class DevicePipelineArchive(unittest.TestCase):
    """Every game's iOS device pipeline archive is merged into a few files (user 2026-11-13: "统一 都做 … 保证 以后都这样"):
    one file per pipeline used up the iPad's vnodes, as a pipeline made from an archive keeps its file mapped (Ocarina of
    Time, 19,750 files: jetsam vnode-limit with 7,098 mapped regions; Breath of the Wild's iPhone build had 15,770 files).
    nxnative bundles one archive per GPU. Every copy of the tool is the same file, so a fix reaches every game."""
    COPIES = sorted((ROOT / 'games').glob('*/ios/tools/device_pipelines.py'))

    def test_every_copy_merges(self):
        self.assertTrue(self.COPIES, 'no device_pipelines.py found')
        for path in self.COPIES:
            with self.subTest(path=str(path.relative_to(ROOT))):
                text = path.read_text()
                groups = re.search(r'^GROUPS = (\d+)', text, re.M)
                self.assertIsNotNone(groups, 'GROUPS is set')
                self.assertGreater(int(groups.group(1)), 0, 'merged by default, never one file per pipeline')
                self.assertIn('def plan_groups(', text)

    def test_every_copy_is_the_same_file(self):
        texts = {path.read_bytes() for path in self.COPIES}
        self.assertEqual(len(texts), 1, 'copies of device_pipelines.py differ: '
                         + ', '.join(str(p.relative_to(ROOT)) for p in self.COPIES))


class IosMacPipelineCache(unittest.TestCase):
    """Every iOS build that makes the device's pipelines on the Mac keeps that cache when the runtime changes and has the
    run check it (device_pipelines.mac_cache, TOTK_PAR_RECHECK), as the Mac build does (build_app.mac_pipelines). User
    2026-11-13: "根治，写测试 以后保证这样": the OOT iPad build threw both caches away for a runtime change, 240 s + 228 s
    of pipelines made again from scratch, and every metal-tt group compiled again."""
    BUILDS = [p for p in sorted((ROOT / 'games').glob('*/ios/tools/build_ipa.py')) if "'pipeline-archive'" in p.read_text()]

    def test_every_build_checks_instead_of_throwing_away(self):
        self.assertTrue(self.BUILDS, 'no iOS build makes pipelines on the Mac')
        for path in self.BUILDS:
            with self.subTest(path=str(path.relative_to(ROOT))):
                text = path.read_text()
                self.assertIn('mac_cache(folder,want)', text)
                self.assertIn('**recheck', text, 'the run gets TOTK_PAR_RECHECK')
                self.assertNotRegex(text, r'rmtree\(folder\b', 'the cache folder is never thrown away')


if __name__ == '__main__':
    unittest.main()

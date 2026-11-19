"""update_runtime.py --shader-libs, and the express update that does the same by itself: the app's own startup seed and
its compiled libraries are taken into the work folder, only the shaders without a library are compiled, and the shader
data bundle and the pipeline archive are made again (an app updated without a Metal compiler had libraries for 1,890 of
the 17,884 shaders its warm-up loads, and compiled the rest at a cold start). plan() says first what the update does."""
import json
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build_app  # noqa: E402
import update_runtime  # noqa: E402

COMPILER = ('metal', 'Apple metal 1', {})


class AdoptAppSeed(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        self.app, self.tree = root / 'Game.app', root / 'work/totk'
        bundle = self.app / 'Contents/Helpers/ShaderData-v143.bundle/Contents/Resources'
        (bundle / 'shader-seed').mkdir(parents=True)
        (bundle / 'shader-seed/seed-manifest.json').write_text('{"chain": "c"}')
        (bundle / 'shader-seed/a_b.metal').write_text('// a')
        (bundle / 'shader-libs').mkdir()
        (bundle / 'shader-libs' / ('a' * 64 + '.metallib')).write_bytes(b'lib a')
        build = self.app / 'Contents/Resources/totk/build'   # the links the game reads them by (link_shader_data)
        build.mkdir(parents=True)
        for name in build_app.SHADER_DATA:
            (build / name).symlink_to(os.path.relpath(bundle / name, build))

    def adopt(self):
        with mock.patch.object(update_runtime, 'say'):
            update_runtime.adopt_app_seed(self.tree, self.app, 'v143')

    def test_seed_and_libraries_come_from_the_app(self):
        self.adopt()
        seed = self.tree / 'build/shader-seed/v143'
        self.assertFalse(seed.is_symlink())
        self.assertEqual(sorted(p.name for p in seed.iterdir()), ['a_b.metal', 'seed-manifest.json'])
        self.assertEqual((self.tree / 'build/shader-libs' / ('a' * 64 + '.metallib')).read_bytes(), b'lib a')

    def test_another_seed_in_the_work_folder_is_replaced_and_its_libraries_kept(self):
        old = self.tree / 'build/shader-seed/v143'
        old.mkdir(parents=True)
        (old / 'old_b.metal').write_text('// old')
        old.with_name('v143.verified.json').write_text('{}')
        libs = self.tree / 'build/shader-libs'
        libs.mkdir(parents=True)
        (libs / ('b' * 64 + '.metallib')).write_bytes(b'work lib')
        self.adopt()
        self.assertEqual(sorted(p.name for p in old.iterdir()), ['a_b.metal', 'seed-manifest.json'])
        self.assertFalse(old.with_name('v143.verified.json').exists(), 'the old identities would name other files')
        self.assertEqual(sorted(p.name for p in libs.iterdir()), ['b' * 64 + '.metallib'])

    def test_an_app_without_a_seed_is_refused(self):
        build_app.remove_tree(self.app / 'Contents/Helpers')
        with self.assertRaises(build_app.BuildError):
            self.adopt()


class FakeApp(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.app = Path(tmp.name) / 'Game.app'
        (self.app / 'Contents').mkdir(parents=True)
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable': 'TOTK'}))
        self.work = Path(tmp.name) / 'work'

    def identity(self):
        return [mock.patch.object(update_runtime, 'standalone_game', return_value=None),
                mock.patch.object(update_runtime, 'skyward_app', return_value=False),
                mock.patch.object(update_runtime, 'ocarina_app', return_value=False),
                mock.patch.object(update_runtime, 'app_identity', return_value=('totk', 'v143')),
                mock.patch.object(update_runtime, 'check_seed_chain', return_value=False)]


class NoCompiler(FakeApp):
    """Without a Metal compiler --shader-libs stops before the work folder or the app is touched; --seed goes on as a
    full build does (its new shaders without a library, a warning that plan() gives first), so a player without Xcode's
    Metal toolchain keeps the fast update the Launcher offers (after PR #2297)."""
    def main(self, *flags, compiler=None):
        patches = [*self.identity(), mock.patch.object(build_app, 'metal_compiler', return_value=compiler),
                   mock.patch.object(build_app, 'prerequisites', side_effect=AssertionError('went on'))]
        for p in patches:
            p.start()
        self.addCleanup(mock.patch.stopall)
        try:
            update_runtime.main([str(self.app), '--work', str(self.work), *flags])
        finally:
            self.went_on = build_app.prerequisites.called

    def test_shader_libs_without_a_compiler(self):
        with self.assertRaises(build_app.BuildError) as raised:
            self.main('--shader-libs')
        self.assertIn('MetalToolchain', str(raised.exception))
        self.assertFalse(self.went_on)
        self.assertFalse(self.work.exists())

    def test_seed_without_a_compiler_goes_on(self):
        with self.assertRaises(AssertionError):   # it goes on to the build (stopped here)
            self.main('--seed')
        self.assertTrue(self.went_on)

    def test_a_plain_update_needs_no_compiler(self):
        with self.assertRaises(AssertionError):   # it goes on to the build (stopped here)
            self.main()

    def test_with_a_compiler_it_goes_on(self):
        with self.assertRaises(AssertionError):
            self.main('--shader-libs', compiler=COMPILER)

    def test_seed_and_shader_libs_together_are_refused(self):
        with self.assertRaises(build_app.BuildError):
            self.main('--seed', '--shader-libs', compiler=COMPILER)


class Stop(Exception):
    pass


class ShaderLibsRoute(FakeApp):
    """What main() does with the work folder before the runtime is built: --shader-libs completes the libraries of the
    app's seed (required), and so does a plain (express) update of an app whose seed lacks libraries when a compiler is
    at hand; --seed and the newest list's new shaders make a new seed (the app's libraries first; a missing compiler is
    a warning there)."""
    def route(self, *flags, adds=False, gaps=0, compiler=COMPILER):
        tree = self.work / 'totk'
        steps = mock.Mock()
        patches = [*self.identity(),
                   mock.patch.object(build_app, 'metal_compiler', return_value=compiler),
                   mock.patch.object(update_runtime, 'missing_shader_libs', return_value=gaps),
                   mock.patch.object(build_app, 'prerequisites', return_value=Path('python3')),
                   mock.patch.object(build_app, 'prepare_work', return_value=tree),
                   mock.patch.object(update_runtime, 'game_data', return_value=self.app / 'data'),
                   mock.patch.object(update_runtime, 'check_game_data'),
                   mock.patch.object(update_runtime, 'check_rewriter'),
                   mock.patch.object(update_runtime, 'rewriter_sha256'),
                   mock.patch.object(update_runtime, 'prepare_native_5k', return_value=('keep', None)),
                   mock.patch.object(update_runtime, 'check_overlays', return_value=[]),
                   mock.patch.object(update_runtime, 'check_work_files'),
                   mock.patch.object(update_runtime, 'newest_shader_list', return_value=(self.work / 'list.tsv', 'given')),
                   mock.patch.object(update_runtime, 'list_adds_shaders', steps.list_adds_shaders),
                   mock.patch.object(update_runtime, 'adopt_app_seed', steps.adopt_app_seed),
                   mock.patch.object(update_runtime, 'make_seed', steps.make_seed),
                   mock.patch.object(build_app, 'step_shader_libs', steps.step_shader_libs),
                   mock.patch.object(build_app, 'make_runtime', side_effect=Stop),
                   mock.patch.object(update_runtime, 'say')]
        steps.list_adds_shaders.return_value = adds
        for p in patches:
            p.start()
        self.addCleanup(mock.patch.stopall)
        with self.assertRaises(Stop):
            update_runtime.main([str(self.app), '--work', str(self.work), *flags])
        return steps, tree

    def test_shader_libs_completes_the_apps_seed(self):
        steps, tree = self.route('--shader-libs')
        steps.adopt_app_seed.assert_called_once_with(tree, self.app.resolve(), 'v143')
        steps.step_shader_libs.assert_called_once()
        self.assertTrue(steps.step_shader_libs.call_args.kwargs['required'])
        steps.make_seed.assert_not_called()
        steps.list_adds_shaders.assert_not_called()

    def test_seed_takes_the_apps_libraries(self):
        steps, _ = self.route('--seed', gaps=5)
        steps.make_seed.assert_called_once()
        self.assertNotIn('required', steps.make_seed.call_args.kwargs)
        self.assertEqual(steps.make_seed.call_args.kwargs['app'], self.app.resolve())
        steps.adopt_app_seed.assert_not_called()

    def test_seed_without_a_compiler_makes_the_seed(self):
        steps, _ = self.route('--seed', compiler=None)
        steps.make_seed.assert_called_once()

    def test_new_list_shaders_make_a_seed_that_fills_the_gaps(self):
        steps, _ = self.route(adds=True, gaps=5)
        steps.make_seed.assert_called_once()
        steps.adopt_app_seed.assert_not_called()

    def test_a_plain_update_makes_no_seed(self):
        steps, _ = self.route()
        steps.make_seed.assert_not_called()
        steps.adopt_app_seed.assert_not_called()
        steps.step_shader_libs.assert_not_called()

    def test_a_plain_update_completes_missing_libraries(self):   # the app of PR #2297: 1,890 of 17,884 had one
        steps, tree = self.route(gaps=15994)
        steps.adopt_app_seed.assert_called_once_with(tree, self.app.resolve(), 'v143')
        steps.step_shader_libs.assert_called_once()
        self.assertTrue(steps.step_shader_libs.call_args.kwargs['required'])
        steps.make_seed.assert_not_called()

    def test_shaders_this_compiler_refused_are_not_gaps(self):
        refused = frozenset({'r' * 64})
        with mock.patch.object(build_app, 'refused_shader_libs', return_value=refused) as asked:
            steps, tree = self.route()
        asked.assert_called_once_with(tree, COMPILER[1])
        self.assertEqual(update_runtime.missing_shader_libs.call_args.args, (self.app.resolve(), refused))

    def test_without_a_compiler_a_plain_update_leaves_them(self):
        steps, _ = self.route(gaps=15994, compiler=None)
        steps.adopt_app_seed.assert_not_called()
        steps.step_shader_libs.assert_not_called()
        steps.make_seed.assert_not_called()


def fake_app_with_seed(root, metal, libs):
    """An app whose ShaderData seed names the .metal files of `metal` (name -> SHA-256) and carries libraries `libs`."""
    app = root / 'Game.app'
    bundle = app / 'Contents/Helpers/ShaderData-v143.bundle/Contents/Resources'
    (bundle / 'shader-seed').mkdir(parents=True)
    files = {**metal, 'seed-manifest.json': 'x' * 64, 'shader-keys.json': 'y' * 64}
    (bundle / 'shader-seed/seed-manifest.json').write_text(json.dumps({'chain': 'c', 'files': files}))
    (bundle / 'shader-libs').mkdir()
    for digest in libs:
        (bundle / 'shader-libs' / f'{digest}.metallib').write_bytes(b'lib')
    build = app / 'Contents/Resources/totk/build'
    build.mkdir(parents=True)
    for name in build_app.SHADER_DATA:
        (build / name).symlink_to(os.path.relpath(bundle / name, build))
    return app


class MissingShaderLibs(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)

    def test_counts_the_seeds_shaders_without_a_library(self):
        a, b, c = 'a' * 64, 'b' * 64, 'c' * 64
        # two .metal files of one source share a library; files other than .metal are not shaders
        app = fake_app_with_seed(self.root, {'k1_v.metal': a, 'k2_v.metal': a, 'k3_v.metal': b, 'k4_v.metal': c}, [a])
        self.assertEqual(update_runtime.missing_shader_libs(app), 2)

    def test_refused_shaders_are_not_counted(self):
        a, b = 'a' * 64, 'b' * 64
        app = fake_app_with_seed(self.root, {'k1_v.metal': a, 'k2_v.metal': b}, [])
        self.assertEqual(update_runtime.missing_shader_libs(app, frozenset({b})), 1)

    def test_a_complete_seed_has_none(self):
        app = fake_app_with_seed(self.root, {'k1_v.metal': 'a' * 64}, ['a' * 64, 'd' * 64])
        self.assertEqual(update_runtime.missing_shader_libs(app), 0)

    def test_no_readable_seed(self):
        self.assertIsNone(update_runtime.missing_shader_libs(self.root / 'None.app'))
        app = fake_app_with_seed(self.root, {}, [])
        (app / 'Contents/Resources/totk/build/shader-seed/seed-manifest.json').write_text('{"chain": "c"}')
        self.assertIsNone(update_runtime.missing_shader_libs(app))


class PlanSaysIt(unittest.TestCase):
    """plan() (MNG Launcher picks the update by it) says before the update starts what the update does about the
    libraries: an express update compiles the missing ones, or leaves them without a compiler; a fast update without a
    compiler names the toolchain. The kind stays what it was: no update is refused for want of a compiler."""
    def plan(self, *, gaps, compiler, stale=False):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        app = Path(tmp.name) / 'Game.app'
        (app / 'Contents').mkdir(parents=True)
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable': 'TOTK'}))
        data = app / 'Contents/Resources/totk/build/game-data/v143'
        data.mkdir(parents=True)
        patches = [mock.patch.object(update_runtime, 'standalone_game', return_value=None),
                   mock.patch.object(update_runtime, 'ocarina_app', return_value=False),
                   mock.patch.object(update_runtime, 'app_identity', return_value=('totk', 'v143')),
                   mock.patch.object(build_app, 'recorded_work_folder', return_value=(Path(tmp.name) / 'work', 'recorded')),
                   mock.patch.object(build_app, 'game_data_files', return_value=[]),
                   mock.patch.object(update_runtime, 'check_game_data'),
                   mock.patch.object(update_runtime, 'check_rewriter'),
                   mock.patch.object(update_runtime, 'rewriter_sha256'),
                   mock.patch.object(update_runtime, 'check_seed_chain', return_value=stale),
                   mock.patch.object(update_runtime, 'check_work_files'),
                   mock.patch.object(update_runtime, 'missing_shader_libs', return_value=gaps),
                   mock.patch.object(build_app, 'metal_compiler', return_value=compiler)]
        for p in patches:
            p.start()
        self.addCleanup(mock.patch.stopall)
        return update_runtime.plan(app)

    def test_complete(self):
        found = self.plan(gaps=0, compiler=None)
        self.assertEqual((found['kind'], found['shader_lib_gaps']), ('express', 0))
        self.assertNotIn('compiled library', found['reason'])

    def test_express_compiles_the_gaps(self):
        found = self.plan(gaps=15994, compiler=COMPILER)
        self.assertEqual((found['kind'], found['shader_lib_gaps']), ('express', 15994))
        self.assertIn('15,994 startup shaders have no compiled library, which it compiles', found['reason'])

    def test_express_without_a_compiler_names_the_toolchain(self):
        found = self.plan(gaps=15994, compiler=None)
        self.assertEqual(found['kind'], 'express')
        self.assertIn('MetalToolchain', found['reason'])

    def test_fast_without_a_compiler_names_the_toolchain(self):
        found = self.plan(gaps=None, compiler=None, stale=True)
        self.assertEqual(found['kind'], 'fast')
        self.assertIn('MetalToolchain', found['reason'])
        self.assertNotIn('MetalToolchain', self.plan(gaps=None, compiler=COMPILER, stale=True)['reason'])


if __name__ == '__main__':
    unittest.main()

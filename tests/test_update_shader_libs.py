"""update_runtime.py --shader-libs: the app's own startup seed and its compiled libraries are taken into the work folder,
only the shaders without a library are compiled, and the shader data bundle and the pipeline archive are made again.
--seed and --shader-libs stop before anything is built when there is no Metal compiler (an app updated without one
had libraries for 1,890 of the 17,884 shaders its warm-up loads, and compiled the rest at every cold start)."""
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


class NoCompilerStopsTheUpdate(FakeApp):
    """--seed / --shader-libs without a Metal compiler: an error before the work folder or the app is touched."""
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

    def test_seed_without_a_compiler(self):
        with self.assertRaises(build_app.BuildError):
            self.main('--seed')
        self.assertFalse(self.went_on)

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
    app's seed (required), --seed makes a new one (required, the app's libraries first), the newest list's new shaders
    alone make one as before (a missing compiler is still a warning there)."""
    def route(self, *flags, adds=False):
        tree = self.work / 'totk'
        steps = mock.Mock()
        patches = [*self.identity(),
                   mock.patch.object(build_app, 'metal_compiler', return_value=COMPILER),
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

    def test_seed_requires_the_libraries(self):
        steps, _ = self.route('--seed')
        steps.make_seed.assert_called_once()
        self.assertTrue(steps.make_seed.call_args.kwargs['required'])
        self.assertEqual(steps.make_seed.call_args.kwargs['app'], self.app.resolve())
        steps.adopt_app_seed.assert_not_called()

    def test_new_list_shaders_alone_keep_the_warning(self):
        steps, _ = self.route(adds=True)
        steps.make_seed.assert_called_once()
        self.assertFalse(steps.make_seed.call_args.kwargs['required'])

    def test_a_plain_update_makes_no_seed(self):
        steps, _ = self.route()
        steps.make_seed.assert_not_called()
        steps.adopt_app_seed.assert_not_called()
        steps.step_shader_libs.assert_not_called()


if __name__ == '__main__':
    unittest.main()

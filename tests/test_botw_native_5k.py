"""BotW's larger heaps and capability updates, keeping the selected resolution."""
from contextlib import ExitStack
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import build_app
import update_runtime

ROOT = Path(__file__).resolve().parents[1]


class HeapPatch(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'), 'clang required')
    def test_render_arena_bounds_and_native_size_argument(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'output-heaps'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/test_botw_output_heaps.c'), '-lm', '-o', str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn('native allocation argument ok', result.stdout)

    @unittest.skipUnless(shutil.which('clang'), 'clang required')
    def test_64_bit_values_guards_and_distinct_veneers(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'heap-patch'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/test_botw_heap_patch.c'), '-o', str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn('64-bit heap patch ok', result.stdout)


class Capabilities(unittest.TestCase):
    def test_both_versions_update_keep_disable_and_promote(self):
        for version in ('botw160', 'botw190'):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                res, logs = root/'res', root/'logs'
                game = res/f'extracted/{version}'
                game.mkdir(parents=True)
                saved = root/'graphics-output.txt'
                saved.write_text('2\n')
                with mock.patch.object(build_app, 'native_5k_default', return_value=False):
                    action, made = update_runtime.prepare_native_5k(root, res, root, version, logs, None)
                    self.assertEqual(action, 'keep')
                    action, made = update_runtime.prepare_native_5k(root, res, root, version, logs, True)
                    self.assertEqual(action, 'add')
                    update_runtime.replace_native_5k(res, action, made, version)
                    self.assertEqual((game/'native-5k').read_text(), '5120x2880\n')
                    self.assertEqual((game/'native-4k').read_text(), '3840x2160\n')
                    self.assertEqual(update_runtime.prepare_native_5k(root, res, root, version, logs, None)[0], 'keep')
                    action, made = update_runtime.prepare_native_5k(root, res, root, version, logs, False)
                    update_runtime.replace_native_5k(res, action, made, version)
                    self.assertFalse((game/'native-5k').exists())
                    self.assertTrue((game/'native-4k').exists())
                self.assertEqual(saved.read_text(), '2\n')

    def test_full_cli_routes_both_versions_and_rejects_ios_and_conflicts(self):
        class Stop(Exception):
            pass
        for version in ('botw160', 'botw190'):
            for flags, expected, error in ((('--native-5k',), True, None),
                                           (('--no-native-5k',), False, None),
                                           (('--native-5k', '--no-native-4k'), None, 'includes native 4K'),
                                           (('--native-5k', '--platform', 'ios'), None, 'needs a Mac')):
                with self.subTest(version=version, flags=flags), tempfile.TemporaryDirectory() as tmp, ExitStack() as stack:
                    root = Path(tmp)
                    for name in ('game.nsp', 'update.nsp', 'keys'):
                        (root/name).touch()
                    tree = root/'work/totk'
                    tree.mkdir(parents=True)
                    for name, result in dict(input_kind='nsp', fonts_valid=True, prerequisites=Path('/python'),
                                             prepare_work=tree, identify_game='botw', identify_botw_update=version,
                                             prepare_update_container=root/'update.nsp', native_4k_default=True,
                                             native_5k_default=False, native_5k_roomy=False).items():
                        stack.enter_context(mock.patch.object(build_app, name, return_value=result))
                    stack.enter_context(mock.patch.object(build_app.shutil, 'disk_usage', return_value=mock.Mock(free=1000*2**30)))
                    stack.enter_context(mock.patch.object(build_app, 'say'))
                    for name in ('check_package', 'check_dlc', 'step_code_botw', 'step_data_botw',
                                 'step_shaders_botw', 'step_shader_libs', 'step_binary'):
                        stack.enter_context(mock.patch.object(build_app, name))
                    package = stack.enter_context(mock.patch.object(build_app, 'step_app', side_effect=Stop))
                    args = [str(root/'game.nsp'), '-k', str(root/'keys'), '--update', str(root/'update.nsp'),
                            '--work', str(root/'work'), *flags]
                    if error:
                        with self.assertRaisesRegex(build_app.BuildError, error):
                            build_app.main(args)
                        package.assert_not_called()
                    else:
                        with self.assertRaises(Stop):
                            build_app.main(args)
                        self.assertEqual(package.call_args.kwargs['native_5k'], expected)
                        self.assertTrue(package.call_args.kwargs['native_4k'])


if __name__ == '__main__':
    unittest.main()

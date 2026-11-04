"""5K capability follows memory and explicit choices, without selecting a new game resolution."""
from contextlib import ExitStack
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import build_app
import update_runtime


class StopBuild(Exception):
    pass


class BuildDefaults(unittest.TestCase):
    def test_memory_boundary_and_failed_detection(self):
        for gib, expected in ((8, False), (16, False), (24, False), (31, False),
                              (32, True), (36, True), (48, True), (64, True)):
            with self.subTest(memory=gib):
                self.assertEqual(build_app.native_5k_default(gib << 30), expected)
        for error in (OSError('no sysctl'), subprocess.CalledProcessError(1, 'sysctl'), ValueError('bad value')):
            with mock.patch.object(build_app.subprocess, 'check_output', side_effect=error):
                self.assertFalse(build_app.native_5k_default())

    def test_automatic_selection_is_only_supported_mac_games_and_respects_the_4k_opt_out(self):
        with mock.patch.object(build_app, 'native_5k_default', return_value=True):
            for platform in ('macos', 'ios', 'visionos'):
                for version in ('v0', 'v112', 'v121', 'v143', 'botw160', 'botw190'):
                    self.assertEqual(build_app.native_5k_enabled(platform, version),
                                     platform == 'macos' and version in ('v143', 'botw160', 'botw190'))
            self.assertFalse(build_app.native_5k_enabled('macos', 'v143', native_4k=False))
            self.assertFalse(build_app.native_5k_enabled('macos', 'v143', requested=False))
        with mock.patch.object(build_app, 'native_5k_default', return_value=False):
            self.assertTrue(build_app.native_5k_enabled('macos', 'v143', requested=True))

    def build_options(self, gib, flags=(), version='v143', platform='macos'):
        """Run the actual CLI routing up to packaging; long game-dependent steps are replaced."""
        with tempfile.TemporaryDirectory() as tmp, ExitStack() as stack:
            root = Path(tmp)
            game, update, keys, work = root/'game.nsp', root/'update.nsp', root/'keys', root/'work'
            for p in (game, update, keys):
                p.touch()
            tree = work/'totk'
            tree.mkdir(parents=True)
            replacements = dict(input_kind='nsp', fonts_valid=True, prerequisites=Path('/python'),
                                prepare_work=tree, identify_game='totk', identify_update=(version, update),
                                native_4k_default=gib >= 16, native_5k_default=gib >= 32,
                                native_5k_roomy=gib >= 32)
            for name, value in replacements.items():
                stack.enter_context(mock.patch.object(build_app, name, return_value=value))
            stack.enter_context(mock.patch.object(build_app.shutil, 'disk_usage', return_value=mock.Mock(free=1000*2**30)))
            stack.enter_context(mock.patch.object(build_app, 'say'))
            for name in ('step_code', 'step_data', 'step_shaders', 'step_shader_libs', 'step_binary', 'build_device_translator'):
                stack.enter_context(mock.patch.object(build_app, name))
            data = stack.enter_context(mock.patch.object(build_app, 'step_update_data'))
            app = stack.enter_context(mock.patch.object(build_app, 'step_app', side_effect=StopBuild))
            stack.enter_context(mock.patch.object(build_app, 'run', side_effect=StopBuild))   # iOS packaging
            with self.assertRaises(StopBuild):
                build_app.main([str(game), '-k', str(keys), '--update', str(update), '--work', str(work),
                                '--platform', platform, *flags])
            return data.call_args.kwargs, app.call_args.kwargs if app.called else None

    def test_full_build_routes_memory_and_override_choices_to_resources_and_metadata(self):
        for gib, flags, expected_4k, expected_5k, preference in (
                (8, (), False, False, None), (16, (), True, False, None),
                (24, (), True, False, None), (32, (), True, True, None),
                (64, (), True, True, None), (16, ('--native-5k',), True, True, True),
                (64, ('--no-native-5k',), True, False, False),
                (64, ('--no-native-4k',), False, False, False)):
            with self.subTest(memory=gib, flags=flags):
                data, app = self.build_options(gib, flags)
                self.assertEqual((data['native_4k'], data['native_5k']), (expected_4k, expected_5k))
                self.assertIs(app['native_5k'], preference)
        for version in ('v121',):
            data, _ = self.build_options(64, version=version)
            self.assertFalse(data['native_5k'])
        data, app = self.build_options(64, platform='ios')
        self.assertFalse(data['native_4k'] or data['native_5k'])
        self.assertIsNone(app)

    def test_explicit_unsupported_and_conflicting_options_still_stop(self):
        with self.assertRaisesRegex(build_app.BuildError, '--native-5k needs'):
            self.build_options(64, ('--native-5k',), version='v121')
        with self.assertRaisesRegex(build_app.BuildError, '--native-5k needs'):
            self.build_options(64, ('--native-5k',), platform='ios')
        with self.assertRaisesRegex(build_app.BuildError, '--native-5k includes native 4K'):
            self.build_options(64, ('--native-5k', '--no-native-4k'))


class UpdateDefaults(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.tree, self.res, self.logs = self.root/'tree', self.root/'res', self.root/'logs'
        self.overlays = self.res/'extracted/v143/romfs-patches-out'
        self.overlays.mkdir(parents=True)
        self.saved = self.root/'userdata/picture-mode.txt'
        self.saved.parent.mkdir()
        self.saved.write_text('native-2k\n')

    def overlay(self, name='3840x2160', heapdef=None):
        folder = self.overlays/name
        folder.mkdir(exist_ok=True)
        (folder/'manifest.tsv').write_text('old manifest\n')
        (folder/'overlay.json').write_text(json.dumps({'heapdef': heapdef}))
        return folder

    def prepare(self, memory=32, preference=None, failure=None, version='v143'):
        def generate(argv, log, **kwargs):
            size = argv[2]
            scene = '-scene-'+argv[argv.index('--scene')+1] if '--scene' in argv else ''
            folder = Path(argv[argv.index('--output-root')+1])/(size+scene)
            folder.mkdir(parents=True)
            (folder/'manifest.tsv').write_text('new verified fixture\n')
        with mock.patch.object(build_app, 'native_5k_default', return_value=memory >= 32), \
             mock.patch.object(update_runtime, 'check_work_files', side_effect=failure) as checked, \
             mock.patch.object(update_runtime, 'run', side_effect=generate) as generated, \
             mock.patch.object(update_runtime, 'say'):
            action, made = update_runtime.prepare_native_5k(self.tree, self.res, self.root/'data', version, self.logs, preference)
        self.assertEqual(self.saved.read_text(), 'native-2k\n')
        return action, made, checked, generated

    def test_32gb_upgrade_prepares_complete_resources_and_only_changes_the_staged_copy(self):
        old = self.overlay()
        action, made, checked, generated = self.prepare()
        self.assertEqual(action, 'add')
        checked.assert_called_once()
        self.assertEqual(generated.call_count, 9)   # 4K plus all five 5K canvas/scene combinations
        self.assertTrue((made/'5120x2880-scene-5120x2880/manifest.tsv').is_file())
        self.assertEqual((old/'manifest.tsv').read_text(), 'old manifest\n')
        stage = self.root/'stage'
        shutil.copytree(self.res, stage)
        update_runtime.replace_native_5k(stage, action, made)
        self.assertTrue((stage/'extracted/v143/romfs-patches-out/5120x2880/manifest.tsv').is_file())
        self.assertFalse((self.overlays/'5120x2880').exists())

    def test_smaller_machines_and_other_games_are_unchanged_but_explicit_on_works(self):
        self.assertEqual(self.prepare(memory=24)[0], 'keep')
        self.assertEqual(self.prepare(version='v121')[0], 'keep')
        self.assertEqual(self.prepare(version='botw190')[0], 'add')
        self.assertEqual(self.prepare(memory=16, preference=True)[0], 'add')

    def test_existing_5k_is_preserved_on_a_smaller_mac_and_explicit_off_only_removes_5k(self):
        self.overlay()
        self.overlay('5120x2880-scene-5120x2880')
        action, _, checked, generated = self.prepare(memory=16)
        self.assertEqual(action, 'keep')
        checked.assert_not_called()
        generated.assert_not_called()
        action, made, _, _ = self.prepare(preference=False)
        self.assertEqual(action, 'remove')
        update_runtime.replace_native_5k(self.res, action, made)
        self.assertTrue((self.overlays/'3840x2160/manifest.tsv').is_file())
        self.assertFalse((self.overlays/'5120x2880-scene-5120x2880').exists())

    def test_wrong_or_missing_cache_is_not_used_and_forced_upgrade_fails_before_replacement(self):
        error = build_app.BuildError('not the app\'s main module')
        action, _, _, generated = self.prepare(failure=error)
        self.assertEqual(action, 'keep')
        generated.assert_not_called()
        with self.assertRaisesRegex(build_app.BuildError, 'cannot add native 5K'):
            self.prepare(preference=True, failure=error)

    def test_modded_heap_resources_require_a_rebuild_with_the_same_mod(self):
        self.overlay(heapdef='/mod/HeapDef.zs')
        self.assertEqual(self.prepare()[0], 'keep')
        with self.assertRaisesRegex(build_app.BuildError, 'same --mod'):
            self.prepare(preference=True)

    def test_explicit_preference_survives_update_metadata_and_auto_remains_auto(self):
        app = self.root/'Game.app'
        info_path = app/'Contents/Info.plist'
        info_path.parent.mkdir(parents=True)
        for preference in (True, False, None):
            info_path.write_bytes(plistlib.dumps({'MNGNative5K': True, 'CFBundleName': 'Game'}))
            with mock.patch.object(build_app, 'source_commit', return_value='abc1234'):
                update_runtime.record_commit(app, self.root/'work', native_5k=preference)
            info = plistlib.loads(info_path.read_bytes())
            self.assertEqual(info['CFBundleName'], 'Game')
            self.assertIs(info.get('MNGNative5K'), preference)
            self.assertEqual(self.saved.read_text(), 'native-2k\n')

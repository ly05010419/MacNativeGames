"""Exercise the preparation pipeline with synthetic inputs, and stage tiny resources."""
from contextlib import ExitStack, redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
for folder in ('scripts', 'dm/games', 'dm/tools', 'dm/ios/tools'):
    sys.path.insert(0, str(REPO / folder))
import build_ipa_from_nsp as build
import game
import gen_resources
import nacp
import profile_guess
import xcode_projects


class XcodePipelineTests(unittest.TestCase):
    def test_prepare_preserves_upstream_options_and_graphics_paths(self):
        for graphics in ('nvn', 'vulkan', 'gl'):
            for derived in (False, True):
                with self.subTest(graphics=graphics, derived=derived):
                    self.prepare_fixture(graphics, derived)

    def prepare_fixture(self, graphics, derived):
        with tempfile.TemporaryDirectory() as temporary, ExitStack() as stack:
            root = Path(temporary).resolve()
            (root / 'prod.keys').touch()
            base = root / 'base.nsp'
            update = root / 'update.nsp'
            base.touch()
            update.touch()
            registered = game.load('0100A6301214E000')
            profile = SimpleNamespace(id=registered.id, title_id=registered.title_id, dir=root / 'profile' / registered.id,
                                      get=registered.get, words=registered.words,
                                      builds=lambda: {'v0': {'display': '1.0.0', 'main': 'other-build'}})
            sources = {key: (False, '') for key in ('glslc_capture', 'vk_capture', 'gl_capture', 'ue_decode', 'oodle_packs', 'ue_legacy')}
            sources['graphics'] = (graphics, 'fixture')
            fam = {'base': base, 'updates': [] if derived else [(327680, update)], 'dlc': []}
            version = 'v0' if derived else 'v327680'
            extraction = root / 'extracted' / version
            (extraction / 'RomFS').mkdir(parents=True)
            (extraction / 'ExeFS').mkdir()
            def fake_run(cmd, log, **kwargs):
                log.parent.mkdir(parents=True, exist_ok=True)
                log.write_text('')  # a successful command with no diagnostic output

            mocks = {}
            for name, value in {
                'scan_nsps': (int(registered.id, 16), fam), 'check_prereqs': None, 'check_metal': None,
                'free_gb': 10000, 'build_nx_extract': root / 'nx_extract',
                'extract': extraction, 'can_clone': False, 'build_mismatch': [],
                'extract_aoc': None, 'rewrite': profile, 'split_shaders': 'fixture-key',
                'build_shaders': root / 'shaders', 'build_vk_shaders': root / 'shaders',
                'build_gl_shaders': root / 'shaders', 'provisional_prep': None, 'chain_header': None,
                'refine_after_rewrite': profile, 'derive_profile': '1.0.0',
            }.items():
                mocks[name] = stack.enter_context(patch.object(build, name, return_value=value))
            mocks['run'] = stack.enter_context(patch.object(build, 'run', side_effect=fake_run))
            stack.enter_context(patch.dict(os.environ))  # derived profiles change the tool search environment
            stack.enter_context(patch.object(game, 'by_title', return_value=registered))
            stack.enter_context(patch.object(game, 'load', return_value=profile))
            stack.enter_context(patch.object(profile_guess, 'build_sources', return_value=sources))
            stack.enter_context(patch.object(nacp, 'name', return_value='Fixture Game'))
            stack.enter_context(patch.object(nacp, 'version', return_value='1.0.0'))
            stack.enter_context(patch.object(nacp, 'languages', return_value=['en-US', 'ja']))
            stack.enter_context(patch.object(xcode_projects, 'signing_settings', return_value={
                'PRODUCT_BUNDLE_IDENTIFIER': 'com.example.saved', 'DEVELOPMENT_TEAM': 'ABCDEFGHIJ'}))
            register = stack.enter_context(patch.object(xcode_projects, 'register_project'))
            progress = build.Progress()
            stack.enter_context(patch.object(build, 'PROG', progress))
            output = io.StringIO()
            try:
                with redirect_stdout(output):
                    code = build.cli([str(root), '-k', str(root / 'prod.keys'), '--prepare-xcode',
                                      '--language', 'ja', '--no-show-fps', '--no-motion', '--progress', 'json'])
            finally:
                progress.close()
            self.assertEqual(code, 0)
            events = [json.loads(line) for line in output.getvalue().splitlines()]
            self.assertTrue(any(event.get('name') == 'prepare Xcode' for event in events))
            self.assertFalse(any(event.get('name') == 'app build' for event in events))
            command = mocks['run'].call_args.args[0]
            self.assertEqual(command[-1], '--prepare-xcode')
            environment = mocks['run'].call_args.kwargs['env']
            self.assertEqual(environment['DM_LANG'], 'ja')
            self.assertEqual(environment['DM_SHOW_FPS'], '0')
            self.assertIn('DM_ENV_FILE', environment)   # --no-motion bakes DM_MOTION=0 into the app's dm/env.txt
            self.assertEqual(Path(environment['DM_ENV_FILE']).read_text(), 'DM_MOTION=0\n')
            self.assertEqual(environment['GFXAPI'], graphics)
            self.assertEqual(environment['DM_BUNDLE_ID'], 'com.example.saved')
            self.assertEqual(environment['DM_TEAM_ID'], 'ABCDEFGHIJ')
            self.assertEqual(Path(environment['DM_XCODE_PROJECT_DIR']), root / f'{registered.id}_build/xcode')
            register.assert_called_once()
            self.assertEqual(mocks['derive_profile'].call_count, int(derived))
            if graphics == 'nvn':
                self.assertEqual(mocks['split_shaders'].call_args.args[8], derived)
            else:
                builder = mocks['build_vk_shaders' if graphics == 'vulkan' else 'build_gl_shaders']
                self.assertTrue(builder.call_args.kwargs['reuse'])
            warnings = [event.get('msg', '') for event in events]
            self.assertEqual(any('this Xcode project uses' in message for message in warnings), derived)
            if derived:
                self.assertTrue(any('main is a build' in message for message in warnings))

    def test_provisional_warning_stays_json(self):
        output = io.StringIO()
        profile = SimpleNamespace(dir=Path('/external/profile/fixture'), id='fixture')
        with patch.object(build, 'PROG', build.Progress('json')), redirect_stdout(output):
            build.report_profile_status(profile, profile, 'v0', False, True, 'Xcode project')
        events = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertIn('Xcode project was built from a provisional', events[0]['msg'])


class BuildEnvFileTests(unittest.TestCase):
    def test_no_motion_keeps_the_env_files_switches(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            mine = work / 'mine.txt'
            mine.write_text('# diagnostics\nDM_MOTION_LOG=1\nDM_MOTION=1\n')
            self.assertEqual(build.build_env_file(work, str(mine), True), str(mine))
            self.assertEqual(Path(build.build_env_file(work, str(mine), False)).read_text(), 'DM_MOTION_LOG=1\nDM_MOTION=0\n')


class ResourceStagingTests(unittest.TestCase):
    def test_nvn_vulkan_gl_and_incremental_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve() / "Game's $ literal `ticks`"
            ios, romfs, shaders, aoc = [root / name for name in ('xcode', 'romfs', 'shaders', 'aoc')]
            for folder in (ios / 'Gen', romfs, aoc / '1'):
                folder.mkdir(parents=True)
            (ios / 'Gen/dm.xcconfig').touch()
            (romfs / 'marker').write_text('game')
            (aoc / '1/content').write_text('dlc')
            chain_h = root / 'dmsh_chain.h'
            chain_h.write_text('#define DMSH_CHAIN "fixture"\n')
            app = root / 'products/DmIOS.app'
            environment = dict(os.environ, SRCROOT=str(ios), TARGET_BUILD_DIR=str(app.parent),
                               UNLOCALIZED_RESOURCES_FOLDER_PATH=app.name, PLATFORM_NAME='iphoneos')
            for chain in ('b-fixture', 'vk', 'gl'):
                with self.subTest(chain=chain):
                    shader = shaders / chain
                    shader.mkdir(parents=True)
                    (shader / 'fixture.metallib').write_text(chain)
                    arguments = ['gen_resources.py', str(ios), str(romfs), 'marker', str(shader), chain, str(aoc), str(root / 'dd'), str(chain_h)]
                    with patch.object(sys, 'argv', arguments):
                        gen_resources.main()
                    result = subprocess.run(['bash', str(REPO / 'dm/ios/tools/stage_resources.sh')],
                                            env=environment, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual((app / 'romfs/marker').read_text(), 'game')
                    self.assertEqual((app / 'msl' / chain / 'fixture.metallib').read_text(), chain)
                    self.assertEqual([p.name for p in (app / 'msl').iterdir()], [chain])
                    self.assertEqual((app / 'aoc/1/content').read_text(), 'dlc')
            # Quick reinstalls omit only RomFS, including removing an old bundled copy; restoring the default stages it again.
            environment['DM_COPY_ROMFS'] = 'NO'
            result = subprocess.run(['bash', str(REPO / 'dm/ios/tools/stage_resources.sh')], env=environment, capture_output=True)
            self.assertEqual(result.returncode, 0); self.assertFalse((app / 'romfs').exists())
            self.assertTrue((app / 'msl/gl/fixture.metallib').is_file()); self.assertTrue((app / 'aoc/1/content').is_file())
            environment.pop('DM_COPY_ROMFS')
            result = subprocess.run(['bash', str(REPO / 'dm/ios/tools/stage_resources.sh')], env=environment, capture_output=True)
            self.assertEqual(result.returncode, 0); self.assertEqual((app / 'romfs/marker').read_text(), 'game')
            # Sources moved to another shader chain since preparing: stop before copying anything.
            arguments = ['gen_resources.py', str(ios), str(romfs), 'marker', str(shaders / 'b-fixture'), 'b-fixture', str(aoc), str(root / 'dd'), str(chain_h)]
            with patch.object(sys, 'argv', arguments):
                gen_resources.main()
            chain_h.write_text('#define DMSH_CHAIN "newer"\n')
            result = subprocess.run(['bash', str(REPO / 'dm/ios/tools/stage_resources.sh')],
                                    env=environment, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('prepare the game again', result.stderr)
            self.assertEqual([p.name for p in (app / 'msl').iterdir()], ['gl'])
            # A disconnected drive fails before deleting the last good app's resources.
            (romfs / 'marker').unlink()
            result = subprocess.run(['bash', str(REPO / 'dm/ios/tools/stage_resources.sh')],
                                    env=environment, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue((app / 'romfs/marker').exists())
            self.assertTrue((app / 'msl/gl/fixture.metallib').exists())
            # Unsigned IPA mode bypasses staging even without a device or mounted data.
            environment.update(DM_COPY_GAME_RESOURCES='NO', PLATFORM_NAME='iphonesimulator')
            result = subprocess.run(['bash', str(REPO / 'dm/ios/tools/stage_resources.sh')], env=environment)
            self.assertEqual(result.returncode, 0)

    def test_pipeline_archive(self):   # the archive made offline goes to <App>/pso when build_ipa.sh names one, and leaves when it no longer does
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            ios, romfs, shader, pso = root / 'xcode', root / 'romfs', root / 'shaders/b-fixture', root / 'pso'
            for folder in (ios / 'Gen', romfs, shader, pso):
                folder.mkdir(parents=True)
            (ios / 'Gen/dm.xcconfig').touch(); (romfs / 'marker').write_text('game'); (shader / 'k_bp.metallib').write_text('lib')
            (pso / 'prebuilt.tsv').write_text('chain\tfixture\n'); (pso / 'applegpu_g18p.mtl4a').write_text('archive')
            chain_h = root / 'dmsh_chain.h'; chain_h.write_text('#define DMSH_CHAIN "fixture"\n')
            app = root / 'products/DmIOS.app'
            environment = dict(os.environ, SRCROOT=str(ios), TARGET_BUILD_DIR=str(app.parent), UNLOCALIZED_RESOURCES_FOLDER_PATH=app.name, PLATFORM_NAME='iphoneos')
            def stage(*pso_arg):
                with patch.object(sys, 'argv', ['gen_resources.py', str(ios), str(romfs), 'marker', str(shader), 'b-fixture', '', str(root / 'dd'), str(chain_h), *pso_arg]):
                    gen_resources.main()
                return subprocess.run(['bash', str(REPO / 'dm/ios/tools/stage_resources.sh')], env=environment, capture_output=True, text=True)
            result = stage(str(pso))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((app / 'pso/applegpu_g18p.mtl4a').read_text(), 'archive')
            self.assertTrue((app / 'msl/b-fixture/k_bp.metallib').exists())   # the pulling libraries the bases are made of
            (pso / 'prebuilt.tsv').unlink()   # an archive folder without its stamp: stop before copying
            self.assertNotEqual(stage(str(pso)).returncode, 0)
            self.assertTrue((app / 'pso/applegpu_g18p.mtl4a').exists())
            result = stage('')   # built without an archive (DM_PSO_GPUS=none)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse((app / 'pso').exists())


class ImageStaging(unittest.TestCase):
    def test_prepared_tools_symlink_stages_image_without_expanding_it(self):
        import romfs_image
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve(); ios = root / 'xcode'; (ios / 'Gen').mkdir(parents=True)
            (ios / 'tools').symlink_to(REPO / 'dm/ios/tools', target_is_directory=True)
            (ios / 'Gen/dm.xcconfig').touch()
            source = root / 'source'; source.mkdir(); (source / 'marker').write_text('resource')
            image = root / 'RomFS.img'; romfs_image.pack(source, image)
            shader = root / 'shaders/gl'; shader.mkdir(parents=True); (shader / 'k.metallib').touch()
            with patch.object(sys, 'argv', ['gen_resources.py', str(ios), str(image), 'marker', str(shader), 'gl', '', str(root / 'dd'), 'unused']):
                gen_resources.main()
            app = root / 'products/Game.app'
            environment = dict(os.environ, SRCROOT=str(ios), TARGET_BUILD_DIR=str(app.parent), UNLOCALIZED_RESOURCES_FOLDER_PATH=app.name, PLATFORM_NAME='iphoneos')
            def stage():
                result = subprocess.run(['bash', str(ios / 'tools/stage_resources.sh')], env=environment, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            stage(); self.assertEqual((app / 'romfs.img').read_bytes(), image.read_bytes()); self.assertFalse((app / 'romfs').exists())
            self.assertFalse((app / 'romfs.id').exists())   # the default build has no identity
            environment['DM_COPY_ROMFS'] = 'NO'; stage(); self.assertFalse((app / 'romfs.img').exists())
            self.assertTrue(romfs_image.same_identity((app / 'romfs.id').read_text(), romfs_image.identity(image)))   # the slim build names the image it needs
            environment.pop('DM_COPY_ROMFS'); stage(); self.assertTrue((app / 'romfs.img').is_file()); self.assertFalse((app / 'romfs.id').exists())


if __name__ == '__main__':
    unittest.main()

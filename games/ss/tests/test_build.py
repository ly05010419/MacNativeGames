"""Packaging safety and portable-toolchain checks; no game files or keys."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME / 'scripts'))
import build_ss as build
import extract_ss
import package_ss
import verify_ss


class BuildTests(unittest.TestCase):
    def test_package_retains_license_and_patent_notices_without_build_tools(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);package_ss.package_notices(root)
            for name in ('third_party/nxnative/LICENSE','runtime/third_party/libvpx/LICENSE',
                         'runtime/third_party/libvpx/PATENTS','runtime/third_party/stb_vorbis/LICENSE',
                         'runtime/third_party/macnativegames/LICENSE','THIRD_PARTY_NOTICES.md'):
                self.assertEqual((root/name).read_bytes(),(GAME/name).read_bytes())
            self.assertFalse((root/'third_party/nxnative/build_ipa_from_nsp.py').exists())
            self.assertFalse((root/'prebuilt').exists())

    @unittest.skipUnless(sys.platform=='darwin','native macOS launcher')
    def test_launcher_keeps_disposable_state_outside_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();app=root/'Test.app';contents=app/'Contents'
            mac=contents/'MacOS';game=contents/'Resources/game'
            mac.mkdir(parents=True);game.mkdir(parents=True)
            binary=mac/'SkywardSwordHD'
            subprocess.run(['clang','-arch','arm64','-mmacosx-version-min=26.0','-O2','-Wall','-Wextra','-Werror',
                            str(GAME/'app/launcher.c'),'-o',str(binary)],check=True)
            host=game/'dmhost';host.write_text('#!/bin/sh\nprintf "%s\\n" "$DM_SAVE" "$DM_TEMP" "$DM_ROOT"\nexit 23\n');host.chmod(0o755)
            original={p.relative_to(app):p.read_bytes() for p in app.rglob('*') if p.is_file()}
            profile=root/'profile';profile.mkdir()
            result=subprocess.run([str(binary)],env=dict(os.environ,DM_SS_PROFILE=str(profile)),capture_output=True)
            self.assertEqual(result.returncode,23)
            log,=profile.glob('session-*.log')
            self.assertEqual(log.read_text().splitlines(),[str(profile/'save'),str(profile/'temp'),str(game)])
            self.assertTrue((profile/'save').is_dir());self.assertTrue((profile/'temp').is_dir())
            alias=root/'inside-alias';alias.symlink_to(game,target_is_directory=True)
            result=subprocess.run([str(binary)],env=dict(os.environ,DM_SS_PROFILE=str(alias)),capture_output=True,text=True)
            self.assertEqual(result.returncode,1);self.assertIn('outside the signed app',result.stderr)
            self.assertEqual({p.relative_to(app):p.read_bytes() for p in app.rglob('*') if p.is_file()},original)

    def test_tooling_checksums(self):
        build.verify_prebuilt()

    def test_tooling_rejects_unlisted_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shutil.copytree(GAME / 'prebuilt', root / 'prebuilt')
            (root / 'prebuilt/macos-arm64/dmsh_xlate/unexpected.dll').write_bytes(b'extra')
            with mock.patch.object(build, 'GAME', root):
                with self.assertRaisesRegex(ValueError, 'file set differs'):
                    build.verify_prebuilt()

    def test_rejects_modified_originals(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            exefs = root / 'extract/v65536/ExeFS'
            exefs.mkdir(parents=True)
            for name in ('main', 'subsdk0', 'subsdk1', 'sdk', 'rtld'):
                (exefs / name).write_bytes(b'NSO0' + bytes(100))
            with self.assertRaisesRegex(ValueError, 'original ExeFS hash mismatch: main'):
                package_ss.verify_originals(root)

    def test_refuses_to_replace_working_app(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'working.app'
            output.mkdir()
            marker = output / 'keep'
            marker.write_text('existing user app')
            with self.assertRaisesRegex(ValueError, 'output exists'):
                package_ss.package(Path(tmp), Path(tmp), output, 'en-US', {})
            self.assertEqual(marker.read_text(), 'existing user app')

    def test_source_staging_preserves_generated_tables(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            source = build.stage(work)
            table = source / f'dm/games/{package_ss.TITLE}/c1_full.tsv'
            table.write_text('generated locally')
            build.stage(work)
            self.assertEqual(table.read_text(), 'generated locally')
            self.assertTrue((source / 'dm/tools/nx_extract/Select.cs').is_file())
            self.assertTrue((source / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate').is_file())
            self.assertFalse((source / 'dm/games/01002B00111A2000').exists())
            chain = subprocess.check_output([sys.executable, str(source / 'dm/gfx/tools/dmsh_chain.py')], text=True, env=build.environment(work)).strip()
            self.assertIn(f'"{chain}"', (source / 'dm/gfx/nvn/dmsh_chain.h').read_text())

    def test_staged_seed_is_the_repositorys_and_a_stale_one_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = build.stage(Path(tmp)) / f'dm/games/{package_ss.TITLE}'
            package_ss.verify_seed(profile)   # control: the staged seed passes
            (profile / 'pso_manifest.tsv').write_text('# an older seed\n')
            with self.assertRaisesRegex(ValueError, 'warm-up seed differs'): package_ss.verify_seed(profile)
            (profile / 'pso_manifest.tsv').unlink()
            with self.assertRaisesRegex(ValueError, 'warm-up seed differs'): package_ss.verify_seed(profile)

    def test_packaging_rejects_wrong_game_shader_stamp(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            source = build.stage(work)
            env = build.environment(work)
            chain = subprocess.check_output([sys.executable, str(source / 'dm/gfx/tools/dmsh_chain.py')], text=True, env=env).strip()
            libraries = work / f'shader/cache/b-{chain}'
            libraries.mkdir(parents=True)
            (libraries / 'inputs.tsv').write_text('game\t0100000000000000\ninputs\twrong\n')
            with mock.patch.object(package_ss, 'verify_originals', return_value={}), mock.patch('inspect_inputs.inspect'), \
                 mock.patch.object(package_ss, 'verify_rewrite', return_value={}), mock.patch.object(package_ss, 'verify_cut', return_value={}):
                with self.assertRaisesRegex(ValueError, 'shader library stamp differs'):
                    package_ss.package(source, work, work / 'test.app', 'en-US', env)
            self.assertFalse((work / 'test.partial.app').exists())

    def test_build_environment_is_made_from_nothing(self):
        planted = dict(DM_GAMES_DIR='/elsewhere', DM_GAMES_FIRST='1', PYTHONPATH='/elsewhere', DM_SPIN_LIST='/elsewhere/list', XLATE='/elsewhere/x', DMSH_CAS='/elsewhere/cas')
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(os.environ, planted):
            work = Path(tmp)
            env = build.environment(work)
            self.assertFalse(set(planted) & set(env))
            self.assertEqual((env['DM_GAME'], env['SS_BUILD_ENV']), (package_ss.TITLE, '1'))
            for key in ('TMPDIR', 'NUGET_PACKAGES', 'DOTNET_CLI_HOME'):
                self.assertTrue(Path(env[key]).is_relative_to(work))
                self.assertTrue(Path(env[key]).is_dir())

    def test_importer_takes_only_a_checked_extraction(self):
        path = GAME / 'third_party/nxnative/build_ipa_from_nsp.py'
        spec = importlib.util.spec_from_file_location('nsp_builder_test', path)
        module = importlib.util.module_from_spec(spec)
        with mock.patch.object(sys, 'path', [str(GAME / 'runtime/tools')] + sys.path):   # the build stages the script beside dm/tools (romfs.py, nso.py)
            spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(module.BuildError, 'reads only a checked extraction'):
                module.main(module.parse_args([str(root), '--platform', 'mac', '-p', 'v65536']))
            extracted = ['--extracted', str(root), '--work-dir', str(root / 'w'), '--platform', 'mac']
            with mock.patch.dict(os.environ, {}, clear=True), self.assertRaisesRegex(module.BuildError, 'no inherited environment'):
                module.main(module.parse_args(extracted + ['-p', 'v65536']))
            with mock.patch.dict(os.environ, SS_BUILD_ENV='1'), self.assertRaisesRegex(module.BuildError, 'requires --platform mac --package v65536'):
                module.main(module.parse_args(extracted + ['-p', 'v327680']))
            self.assertEqual((module.SS_TITLE, module.SS_PACKAGE, module.SS_PINS), (package_ss.TITLE, package_ss.PACKAGE, 'assets/expected-1.0.1.json'))

    def test_rejects_missing_or_modified_offline_dependency(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            package = work / 'local.nupkg'
            with self.assertRaisesRegex(ValueError, 'package is missing'):
                extract_ss.prepare_offline_dependencies(work, package)
            package.write_bytes(b'wrong package')
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                extract_ss.prepare_offline_dependencies(work, package)
            self.assertFalse((work / 'dependencies').exists())

    def test_profile_is_nxnatives_at_the_pin(self):
        sys.path.insert(0, str(GAME/'runtime/games'))
        import game
        import hashlib
        profile = game.load(package_ss.TITLE)
        self.assertEqual(profile.get('fps_target', '0'), '0')
        self.assertEqual(set(profile.builds()), {'v65536'})
        for row in json.loads((GAME/'third_party/ss-profile-import.json').read_text()):
            self.assertEqual(hashlib.sha256((GAME/row['destination']).read_bytes()).hexdigest(), row['sha256'], row['destination'])
        self.assertEqual(sorted(p.name for p in (GAME/'runtime/games').iterdir() if p.is_dir() and p.name != '__pycache__'), [package_ss.TITLE])

    def test_offline_library_rejects_placeholder_and_missing_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); key='0123456789abcdef'; table=root/'map.tsv'
            table.write_text(f'package\tindex\tstage\tdm_ctl_sha16\nvs_main.bin\t0\tvertex\t{key}\n')
            lib=root/f'{key}_b.metallib';lib.write_bytes(b'RUNTIME_METAL\n')
            with self.assertRaisesRegex(ValueError,'not an offline Metal library'):
                verify_ss.verify_libraries(root,table)
            lib.unlink()
            with self.assertRaisesRegex(ValueError,'incomplete offline shader key'):
                verify_ss.verify_libraries(root,table)

    def test_packaging_rejects_changed_rewrite_outputs_and_rules(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);tools=root/'source/dm/tools';tools.mkdir(parents=True)
            rule=tools/'patch_nso.py';rule.write_bytes(b'synthetic rule')
            patched=root/'patched';patched.mkdir();out=patched/'main.patched.bin';out.write_bytes(b'rewritten')
            import hashlib
            digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
            (patched/'build_src.sha256').write_text(f'{digest(rule)}  patch_nso.py\n')
            pins={'modules':{'main':{'sha256':'original'}}}
            (patched/'rewrite-info.json').write_text(json.dumps({'originals':{'main':'original'},'outputs':{out.name:digest(out)}}))
            package_ss.verify_rewrite(root/'source',patched,pins)
            outside=root/'source/dm/games/other/patches.py';outside.parent.mkdir(parents=True);outside.write_bytes(b'x')
            (patched/'build_src.sha256').write_text(f'{digest(rule)}  patch_nso.py\n{digest(outside)}  ../games/other/patches.py\n')
            with self.assertRaisesRegex(ValueError,'rule outside this target'):package_ss.verify_rewrite(root/'source',patched,pins)
            (patched/'build_src.sha256').write_text(f'{digest(rule)}  patch_nso.py\n')
            out.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'rewrite output changed'):package_ss.verify_rewrite(root/'source',patched,pins)
            rule.write_bytes(b'changed rule')
            with self.assertRaisesRegex(ValueError,'stale rewrite rule'):package_ss.verify_rewrite(root/'source',patched,pins)

    @unittest.skipUnless(sys.platform=='darwin','macOS atomic directory publication')
    def test_atomic_publication_never_replaces_an_existing_app(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);partial=root/'new.partial.app';output=root/'new.app'
            partial.mkdir();output.mkdir();(output/'keep').write_text('existing')
            with self.assertRaises(OSError):package_ss.publish(partial,output)
            self.assertEqual((output/'keep').read_text(),'existing')
            self.assertTrue(partial.is_dir())
            (output/'keep').unlink();output.rmdir()
            package_ss.publish(partial,output)
            self.assertTrue(output.is_dir());self.assertFalse(partial.exists())

    def test_layer_only_geometry_requires_valid_reflection(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);libs=root/'b-test';libs.mkdir();key='0123456789abcdef'
            table=root/'map.tsv';table.write_text(f'gs_main.bin\t0\tgeometry\t{key}\n')
            refl=libs/f'{key}_b.json'
            value={'chain':'test','dm_ctl':key,'stage':'gs','gs_slice':{'loc':1,'comp':'x'}}
            refl.write_text(json.dumps(value))
            self.assertEqual(verify_ss.verify_libraries(libs,table)['folded_layer_geometry'],[key])
            value['gs_slice']['loc']=32;refl.write_text(json.dumps(value))
            with self.assertRaisesRegex(ValueError,'invalid folded geometry'):verify_ss.verify_libraries(libs,table)

    def test_packaging_rejects_unresolved_shader_resources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);libs=root/'b-test';libs.mkdir();key='0123456789abcdef'
            table=root/'map.tsv';table.write_text(f'gs_main.bin\t0\tgeometry\t{key}\n')
            (libs/f'{key}_b.metallib').write_bytes(b'MTLBsynthetic')
            for suffix in ('_bg0','_bg1','_bg2','_bg3','_bg4','_bgv'):
                (libs/f'{key}{suffix}.metallib').write_bytes(b'MTLBsynthetic')
            value={'chain':'test','dm_ctl':key,'stage':'gs',
                   'bindings':[{'kind':'sbuf','msl_name':'gp_s0','reg':None}]}
            refl=libs/f'{key}_b.json';refl.write_text(json.dumps(value))
            with self.assertRaisesRegex(ValueError,'unresolved shader binding'):verify_ss.verify_libraries(libs,table)
            value['bindings'][0]['reg']=0;refl.write_text(json.dumps(value))
            self.assertEqual(verify_ss.verify_libraries(libs,table)['keys'],1)
            for invalid in (-1,True,'0'):
                value['bindings'][0]['reg']=invalid;refl.write_text(json.dumps(value))
                with self.assertRaisesRegex(ValueError,'unresolved shader binding'):verify_ss.verify_libraries(libs,table)
            value['bindings'][0]['kind']='unexpected';refl.write_text(json.dumps(value))
            with self.assertRaisesRegex(ValueError,'unknown shader binding'):verify_ss.verify_libraries(libs,table)
            value['bindings']=[];value['drv_unfillable']=['c0 outside table'];refl.write_text(json.dumps(value))
            with self.assertRaisesRegex(ValueError,'unfillable driver reads'):verify_ss.verify_libraries(libs,table)


if __name__ == '__main__':
    unittest.main()

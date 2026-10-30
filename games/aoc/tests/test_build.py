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
import build_aoc as build


class BuildTests(unittest.TestCase):
    def test_package_retains_license_and_patent_notices_without_build_tools(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);build.package_notices(root)
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
            binary=mac/'AgeOfCalamity'
            subprocess.run(['clang','-arch','arm64','-mmacosx-version-min=26.0','-O2','-Wall','-Wextra','-Werror',
                            str(GAME/'app/launcher.c'),'-o',str(binary)],check=True)
            host=game/'dmhost';host.write_text('#!/bin/sh\nprintf "%s\\n" "$DM_SAVE" "$DM_TEMP" "$DM_ROOT"\nexit 23\n');host.chmod(0o755)
            original={p.relative_to(app):p.read_bytes() for p in app.rglob('*') if p.is_file()}
            profile=root/'profile';profile.mkdir()
            result=subprocess.run([str(binary)],env=dict(os.environ,DM_AOC_PROFILE=str(profile)),capture_output=True)
            self.assertEqual(result.returncode,23)
            log,=profile.glob('session-*.log')
            self.assertEqual(log.read_text().splitlines(),[str(profile/'save'),str(profile/'temp'),str(game)])
            self.assertTrue((profile/'save').is_dir());self.assertTrue((profile/'temp').is_dir())
            alias=root/'inside-alias';alias.symlink_to(game,target_is_directory=True)
            result=subprocess.run([str(binary)],env=dict(os.environ,DM_AOC_PROFILE=str(alias)),capture_output=True,text=True)
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
            exefs = root / 'extract/v327680/ExeFS'
            exefs.mkdir(parents=True)
            for name in ('main', 'subsdk0', 'sdk'):
                (exefs / name).write_bytes(b'NSO0' + bytes(100))
            with self.assertRaisesRegex(ValueError, 'original ExeFS hash mismatch: main'):
                build.verify_originals(root)

    def test_refuses_to_replace_working_app(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'working.app'
            output.mkdir()
            marker = output / 'keep'
            marker.write_text('existing user app')
            with self.assertRaisesRegex(ValueError, 'output exists'):
                build.package(Path(tmp), Path(tmp), output, 'en-US')
            self.assertEqual(marker.read_text(), 'existing user app')

    def test_source_staging_preserves_generated_tables(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            source = build.stage(work)
            table = source / f'dm/games/{build.TITLE}/c1_full.tsv'
            table.write_text('generated locally')
            build.stage(work)
            self.assertEqual(table.read_text(), 'generated locally')
            self.assertTrue((source / 'dm/tools/nx_extract/Program.cs').is_file())
            self.assertTrue((source / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate').is_file())
            chain = subprocess.check_output([sys.executable, str(source / 'dm/gfx/tools/dmsh_chain.py')], text=True).strip()
            self.assertIn(f'"{chain}"', (source / 'dm/gfx/nvn/dmsh_chain.h').read_text())

    def test_packaging_rejects_wrong_game_shader_stamp(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            source = build.stage(work)
            chain = subprocess.check_output([sys.executable, str(source / 'dm/gfx/tools/dmsh_chain.py')], text=True).strip()
            libraries = work / f'shader/cache/b-{chain}'
            libraries.mkdir(parents=True)
            (libraries / 'inputs.tsv').write_text('game\t0100000000000000\ninputs\twrong\n')
            with mock.patch.object(build, 'verify_originals', return_value={}), mock.patch('inspect_inputs.inspect'), mock.patch.object(build,'verify_rewrite',return_value={}):
                with self.assertRaisesRegex(ValueError, 'shader library stamp differs'):
                    build.package(source, work, work / 'test.app', 'en-US')
            self.assertFalse((work / 'test.partial.app').exists())

    def test_build_caches_stay_in_work_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            env = build.environment(work)
            for key in ('TMPDIR', 'NUGET_PACKAGES', 'DOTNET_CLI_HOME'):
                self.assertTrue(Path(env[key]).is_relative_to(work))
                self.assertTrue(Path(env[key]).is_dir())

    def test_rejects_unsupported_build_before_rewrite(self):
        path = GAME / 'third_party/nxnative/build_ipa_from_nsp.py'
        spec = importlib.util.spec_from_file_location('nsp_builder_test', path)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root / 'keys').write_bytes(b'synthetic')
            # Exercise main's title gate using a synthetic container inventory.
            with mock.patch.object(module, 'find_repo', return_value=GAME / 'runtime'), \
                 mock.patch.object(module, 'apple_silicon', return_value=True), \
                 mock.patch.object(module, 'scan_nsps', return_value=(0x0100000000000000, {})):
                args = module.parse_args([str(root), '-k', str(root / 'keys'), '--platform', 'mac', '-p', 'v327680'])
                sys.path.insert(0, str(GAME / 'runtime/games'))
                sys.path.insert(0, str(GAME / 'runtime/tools'))
                with self.assertRaisesRegex(module.BuildError, 'supports only Hyrule Warriors: Age of Calamity'):
                    module.main(args)

    def test_rejects_missing_or_modified_offline_dependency(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            package = work / 'local.nupkg'
            with self.assertRaisesRegex(ValueError, 'package is missing'):
                build.prepare_offline_dependencies(work, package)
            package.write_bytes(b'wrong package')
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                build.prepare_offline_dependencies(work, package)
            self.assertFalse((work / 'dependencies').exists())

    def test_staging_removes_obsolete_translator_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            source = build.stage(work)
            for rel in ('dm/third_party/ryujinx/src/old.cs',
                        'dm/gfx/tools/dmsh_xlate/Program.cs',
                        'dm/gfx/tools/dmsh_xlate_bin/zh-CN/Humanizer.resources.dll',
                        'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate.pdb'):
                path = source / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'obsolete')
            build.stage(work)
            self.assertFalse((source / 'dm/third_party/ryujinx').exists())
            self.assertFalse((source / 'dm/gfx/tools/dmsh_xlate').exists())
            files = list((source / 'dm/gfx/tools/dmsh_xlate_bin').rglob('*'))
            self.assertEqual(len(files), 12)
            self.assertFalse(any(p.suffix == '.pdb' or p.name == 'Humanizer.resources.dll' for p in files))

    def test_profile_is_original_timing_and_zero_system_resource(self):
        sys.path.insert(0,str(GAME/'runtime/games'))
        import game
        profile=game.load(build.TITLE)
        self.assertEqual(profile.get('fps_target'),'0')
        self.assertEqual(profile.get('sys_resource'),'0')
        self.assertEqual(set(profile.builds()),{'v327680'})
        self.assertFalse((GAME/'runtime/games/0100965017338000').exists())

    def test_profile_rejects_unsupported_120_fps(self):
        sys.path.insert(0,str(GAME/'runtime/games'))
        import game
        profile=game.load(build.TITLE)
        profile.rows=[(k,['120'] if k=='fps_target' else v) for k,v in profile.rows]
        with self.assertRaisesRegex(SystemExit,'120 gameplay timing is incomplete'):
            game.header(profile)

    def test_offline_library_rejects_placeholder_and_missing_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); key='0123456789abcdef'; table=root/'map.tsv'
            table.write_text(f'package\tindex\tstage\tdm_ctl_sha16\nvs_main.bin\t0\tvertex\t{key}\n')
            lib=root/f'{key}_b.metallib';lib.write_bytes(b'RUNTIME_METAL\n')
            with self.assertRaisesRegex(ValueError,'not an offline Metal library'):
                build.verify_libraries(root,table)
            lib.unlink()
            with self.assertRaisesRegex(ValueError,'incomplete offline shader key'):
                build.verify_libraries(root,table)

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
            build.verify_rewrite(root/'source',patched,pins)
            out.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'rewrite output changed'):build.verify_rewrite(root/'source',patched,pins)
            rule.write_bytes(b'changed rule')
            with self.assertRaisesRegex(ValueError,'stale rewrite rule'):build.verify_rewrite(root/'source',patched,pins)

    @unittest.skipUnless(sys.platform=='darwin','macOS atomic directory publication')
    def test_atomic_publication_never_replaces_an_existing_app(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);partial=root/'new.partial.app';output=root/'new.app'
            partial.mkdir();output.mkdir();(output/'keep').write_text('existing')
            with self.assertRaises(OSError):build.publish(partial,output)
            self.assertEqual((output/'keep').read_text(),'existing')
            self.assertTrue(partial.is_dir())
            (output/'keep').unlink();output.rmdir()
            build.publish(partial,output)
            self.assertTrue(output.is_dir());self.assertFalse(partial.exists())

    def test_layer_only_geometry_requires_valid_reflection(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);libs=root/'b-test';libs.mkdir();key='0123456789abcdef'
            table=root/'map.tsv';table.write_text(f'gs_main.bin\t0\tgeometry\t{key}\n')
            refl=libs/f'{key}_b.json'
            value={'chain':'test','dm_ctl':key,'stage':'gs','gs_slice':{'loc':1,'comp':'x'}}
            refl.write_text(json.dumps(value))
            self.assertEqual(build.verify_libraries(libs,table)['folded_layer_geometry'],[key])
            value['gs_slice']['loc']=32;refl.write_text(json.dumps(value))
            with self.assertRaisesRegex(ValueError,'invalid folded geometry'):build.verify_libraries(libs,table)

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
            with self.assertRaisesRegex(ValueError,'unresolved shader binding'):build.verify_libraries(libs,table)
            value['bindings'][0]['reg']=0;refl.write_text(json.dumps(value))
            self.assertEqual(build.verify_libraries(libs,table)['keys'],1)
            for invalid in (-1,True,'0'):
                value['bindings'][0]['reg']=invalid;refl.write_text(json.dumps(value))
                with self.assertRaisesRegex(ValueError,'unresolved shader binding'):build.verify_libraries(libs,table)
            value['bindings'][0]['kind']='unexpected';refl.write_text(json.dumps(value))
            with self.assertRaisesRegex(ValueError,'unknown shader binding'):build.verify_libraries(libs,table)
            value['bindings']=[];value['drv_unfillable']=['c0 outside table'];refl.write_text(json.dumps(value))
            with self.assertRaisesRegex(ValueError,'unfillable driver reads'):build.verify_libraries(libs,table)


if __name__ == '__main__':
    unittest.main()

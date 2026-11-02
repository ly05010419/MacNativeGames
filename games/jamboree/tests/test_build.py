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
import build_jamboree as build


class BuildTests(unittest.TestCase):
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
            exefs = root / 'extract/v458752/ExeFS'
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
            with mock.patch.object(build, 'verify_originals', return_value={}):
                with self.assertRaisesRegex(ValueError, 'shader library stamp differs'):
                    build.package(source, work, work / 'test.app', 'en-US')
            self.assertFalse((work / 'test.partial.app').exists())

    def test_input_links_follow_relocated_originals(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()   # macOS: the temporary folder is under /var, a link to /private/var
            inputs, work = root / 'originals', root / 'build'
            inputs.mkdir(); work.mkdir()
            original = inputs / 'base.nsp'
            original.write_bytes(b'synthetic container')
            result = build.link_inputs(inputs, work)
            self.assertEqual((result / original.name).resolve(), original)
            original.unlink()
            (inputs / 'update.nsp').write_bytes(b'synthetic update')
            build.link_inputs(inputs, work)
            self.assertFalse((result / 'base.nsp').is_symlink())
            self.assertTrue((result / 'update.nsp').is_symlink())

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
                args = module.parse_args([str(root), '-k', str(root / 'keys'), '--platform', 'mac', '-p', 'v458752'])
                sys.path.insert(0, str(GAME / 'runtime/games'))
                sys.path.insert(0, str(GAME / 'runtime/tools'))
                with self.assertRaisesRegex(module.BuildError, 'supports only Super Mario Party Jamboree'):
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

    def test_shader_map_has_no_original_paths(self):
        rows = [line.split('\t') for line in (GAME / f'runtime/games/{build.TITLE}/shader_map.tsv').read_text().splitlines()
                if line and not line.startswith('#')]
        self.assertEqual(rows[0], ['package', 'index', 'stage', 'dm_ctl_sha16'])
        self.assertEqual(len(rows) - 1, 25693)
        for row in rows[1:]:
            self.assertEqual(len(row), 4)
            self.assertNotIn('/', '\t'.join(row))
            self.assertRegex(row[3], r'^[0-9a-f]{16}$')


if __name__ == '__main__':
    unittest.main()

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from tmnt_work import Steps, build_lock, inventory, adopt_romfs, protect_inputs


class WorkCacheTests(unittest.TestCase):
    def test_verified_reuse_invalidation_and_bounded_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); steps = Steps(root)
            untouched = root / 'user-notes'; untouched.write_text('keep')
            output = root / 'generated'
            calls = []
            def build():
                calls.append(True); output.mkdir(); (output / 'data').write_text('valid')
            self.assertTrue(steps.run('test', {'input': 1}, [output], build))
            self.assertFalse(steps.run('test', {'input': 1}, [output], build))
            (output / 'data').write_text('bad')
            self.assertTrue(steps.run('test', {'input': 1}, [output], build))
            self.assertTrue(steps.run('test', {'input': 2}, [output], build))
            (output / 'stale').write_text('extra')
            self.assertTrue(steps.run('test', {'input': 2}, [output], build))
            self.assertFalse((output / 'stale').exists())
            self.assertEqual(len(calls), 4)
            self.assertEqual(untouched.read_text(), 'keep')

    def test_corrupt_receipt_rebuilds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); steps = Steps(root); output = root / 'data'
            for corrupt in ('[]', 'null', '1', '{'):
                (steps.receipts / 'test.json').write_text(corrupt)
                self.assertTrue(steps.run('test', {}, [output], lambda: output.write_text('good')))

    def test_inputs_cannot_overlap_managed_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); work = root / 'work'; work.mkdir()
            source = work / 'original'; source.write_text('preserve')
            for inputs, outputs in (([source], [work]), ([work], [source]), ([source], [source])):
                with self.assertRaises(ValueError):
                    protect_inputs(inputs, outputs)
            self.assertEqual(source.read_text(), 'preserve')
            protect_inputs([source], [root / 'separate'])

    def test_receipt_temporary_symlink_is_not_followed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); steps = Steps(root); original = root / 'original'
            original.write_text('preserve')
            (steps.receipts / 'test.tmp').symlink_to(original)
            with self.assertRaises(OSError):
                steps.run('test', {}, [root / 'output'], lambda: (root / 'output').write_text('good'))
            self.assertEqual(original.read_text(), 'preserve')

    def test_executable_permission_change_invalidates_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); steps = Steps(root); output = root / 'tool'
            def build():
                output.write_text('tool'); output.chmod(0o755)
            steps.run('tool', {}, [output], build)
            output.chmod(0o644)
            self.assertTrue(steps.run('tool', {}, [output], build))
            self.assertEqual(output.stat().st_mode & 0o777, 0o755)

    def test_failed_step_does_not_leave_valid_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); steps = Steps(root); output = root / 'data'
            def fail():
                output.write_text('partial'); raise RuntimeError('failed')
            with self.assertRaises(RuntimeError): steps.run('test', {}, [output], fail)
            self.assertFalse((root / '.verified-steps/test.json').exists())
            self.assertTrue(steps.run('test', {}, [output], lambda: output.write_text('valid')))

    def test_symlink_and_parent_escape_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); steps = Steps(root); link = root / 'link'
            link.symlink_to(root.parent, target_is_directory=True)
            for output in (link, root.parent / 'outside', root):
                with self.assertRaises(ValueError): steps.run('test', {}, [output], lambda: None)
            with self.assertRaises(ValueError): inventory(link)

    def test_same_work_root_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            with build_lock(directory):
                with self.assertRaises(ValueError):
                    with build_lock(directory): pass
            with build_lock(directory): pass

    def test_adoption_verifies_and_reindexes_actual_romfs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); app = root / 'TMNT.app'
            image = app / 'Contents/Resources/game/romfs/romfs.bin'
            image.parent.mkdir(parents=True); image.write_bytes(b'ROMFS fixture')
            with mock.patch('expected_inputs.check_supported_romfs') as check, mock.patch('extract_romfs.index_romfs', return_value=[]) as index:
                self.assertTrue(adopt_romfs(app, root / 'romfs'))
                check.assert_called_once(); index.assert_called_once_with(image)
            self.assertEqual((root / 'romfs/romfs.bin').read_bytes(), image.read_bytes())
            self.assertEqual(json.loads((root / 'romfs/index.json').read_text())['status'], 'pass')
            with mock.patch('expected_inputs.check_supported_romfs', side_effect=ValueError('unsupported')):
                self.assertFalse(adopt_romfs(app, root / 'bad'))
            self.assertFalse((root / 'bad').exists())

class BuildReuseTests(unittest.TestCase):
    def test_build_pipeline_reuses_outputs_and_shader_seed(self):
        import argparse
        import build_private_bundle as builder
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); exefs = root / 'exefs'; romfs = root / 'romfs'
            exefs.mkdir(); (exefs / 'main').write_text('fixture')
            romfs.mkdir(); (romfs / 'romfs.bin').write_text('romfs')
            (romfs / 'index.json').write_text(json.dumps({'romfs_sha256': 'fixture'}))
            work = root / 'work'
            args = argparse.Namespace(work=work, out=root / 'TMNT.app', exefs=exefs, romfs=romfs,
                fonts=None, dotnet=None, record_work=None, link_romfs=False, app=True)
            calls = []
            def script(name, *argv):
                calls.append(name)
                if name == 'package_private.py': return
                out = Path(argv[argv.index('--out') + 1])
                out.parent.mkdir(parents=True, exist_ok=True)
                if name in ('module_metadata.py', 'prepare_host_tables.py', 'prepare_nro_digest.py'):
                    out.write_text('generated'); return
                out.mkdir()
                (out / 'fixture').write_text('generated')
                if name == 'prepare_resources.py':
                    for name in ('romfs_config.h', 'shared_fonts_config.h'): (out / name).write_text('header')
                if name == 'prepare_nro.py': (out / 'module.nro').write_text('nro')
                if name == 'rewrite_nro.py':
                    for suffix in ('patched.bin', 'fixups.tsv', 'gates.tsv'): (out / ('nro.' + suffix)).write_text('nro result')
            with mock.patch.object(builder, 'check_supported_romfs'), mock.patch.object(builder, 'verify_exefs'), mock.patch.object(builder, 'script', side_effect=script), mock.patch.object(builder, 'call'), mock.patch('tmnt_work.source_identity', return_value={'tools': 1}):
                builder.build(args)
                seed = work / 'build/shader-seed'; seed.mkdir(); (seed / 'translation').write_text('keep')
                first = list(calls); calls.clear()
                builder.build(args)
                self.assertEqual(calls, ['package_private.py'])
                self.assertIn('build_shader_tools.py', first)
                self.assertEqual((seed / 'translation').read_text(), 'keep')
                (work / 'rewritten/fixture').write_text('corrupt'); calls.clear()
                builder.build(args)
                self.assertIn('rewrite_modules.py', calls)
                self.assertNotIn('build_shader_tools.py', calls)

    def test_migration_copies_mouse_and_cloud_settings_once(self):
        import os
        import subprocess
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); home = root / 'home'; binaries = root / 'bin'; binaries.mkdir()
            stat = binaries / 'stat'; stat.write_text('#!/bin/sh\nprintf "100\\n"\n'); stat.chmod(0o755)
            base = home / 'Library/Application Support/MacNativeGames'
            old = base / 'TMNT-old'; (old / 'save').mkdir(parents=True)
            for name in ('account.uid', 'mouse.txt', 'cloud-sync.plist', 'save/GameSave.sav'):
                (old / name).write_text(name)
            script = Path(__file__).resolve().parents[1] / 'scripts/profile_launcher.sh'
            env = dict(os.environ, HOME=str(home), PATH=str(binaries) + ':' + os.environ['PATH'])
            subprocess.run(['sh', str(script)], env=env, check=True, capture_output=True)
            stable = base / 'TMNT'
            for name in ('mouse.txt', 'cloud-sync.plist'):
                self.assertEqual((stable / name).read_text(), name)
                self.assertEqual((old / name).read_text(), name)
            (stable / 'mouse.txt').write_text('new preference')
            subprocess.run(['sh', str(script)], env=env, check=True, capture_output=True)
            self.assertEqual((stable / 'mouse.txt').read_text(), 'new preference')


if __name__ == '__main__': unittest.main()

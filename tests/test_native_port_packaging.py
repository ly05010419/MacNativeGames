"""Dread/Worldend: actual sealed-data reuse, publication and MNG routing."""
import importlib
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
for game in ('dread', 'worldend'): sys.path.insert(0, str(ROOT/f'games/{game}/scripts'))


class PortPackaging(unittest.TestCase):
    @unittest.skipUnless(sys.platform == 'darwin', 'codesign and APFS')
    def test_sealed_data_and_app_directory_are_retained(self):
        for game in ('dread', 'worldend'):
            with self.subTest(game=game), tempfile.TemporaryDirectory() as tmp:
                module = importlib.import_module(game+'_bundle')
                root = Path(tmp); old = root/'Old.app'; new = root/'New.app'
                data = root/'source'; data.mkdir(); (data/'synthetic.metallib').write_bytes(b'synthetic data')
                digests = module.Digests(root/'digests.json')
                a = module.signed_bundle(old, old, 'ShaderData', {'libs': data}, digests, os.environ)
                info = {'CFBundleIdentifier': 'org.macnativegames.'+game}
                (old/'Contents/Info.plist').write_bytes(plistlib.dumps(info))
                old_inode, shader_inode = old.stat().st_ino, a.stat().st_ino
                b = module.signed_bundle(new, old, 'ShaderData', {'libs': data}, digests, os.environ)
                (new/'Contents/Info.plist').write_bytes(plistlib.dumps(info))
                seal = 'Contents/_CodeSignature/CodeResources'
                self.assertEqual((a/seal).read_bytes(), (b/seal).read_bytes())
                module.install_app(new, old, verify=lambda _: True, remove=lambda p: __import__('shutil').rmtree(p))
                self.assertEqual(old.stat().st_ino, old_inode)
                self.assertEqual(a.stat().st_ino, shader_inode)
                (data/'synthetic.metallib').write_bytes(b'changed input')
                c = module.signed_bundle(root/'Changed.app', old, 'ShaderData', {'libs': data}, digests, os.environ)
                self.assertNotEqual((a/seal).read_bytes(), (c/seal).read_bytes())

    def test_mng_build_inputs_and_version_routing(self):
        from tools.builder_jobs import repo, build_job, installed, classify
        for game, title in (('dread', '010093801237C800'), ('worldend', '010048900CF64000')):
            self.assertEqual(repo.game_of_title(title), game)
            self.assertIn('org.macnativegames.'+game, installed.bundle_ids())
            with tempfile.TemporaryDirectory() as tmp:
                plan = {'game': game, 'base': '/base.nsp', 'keys': '/prod.keys'}
                if game == 'dread': plan.update(update='/update.nsp', update_version=327680)
                script, args, work, app, kind, result = build_job.command({'plan': plan, 'output_dir': tmp, 'work_root': tmp})
                self.assertEqual(script, ROOT/f'games/{game}/build.py')
                self.assertIn('/prod.keys', args)
                if game == 'dread': self.assertIn('/update.nsp', args)
                self.assertEqual((kind, result), ('full', game))
        self.assertEqual(classify._supported('dread'), {0, 327680})
        self.assertEqual(classify._supported('worldend'), {0})
        self.assertTrue(build_job.same_version({'game':'dread'}, {'game':'dread','version':'v100'}))
        self.assertFalse(build_job.same_version({'game':'dread'}, {'game':'dread','version':'v210'}))
        self.assertTrue(build_job.same_version({'game':'worldend'}, {'game':'worldend','version':'v0'}))

    def test_dread_version_keeps_each_identity(self):
        import update_runtime
        module = update_runtime.standalone_module('dread')
        before = os.environ.get('DREAD_VERSION')
        try:
            module.select_version('v100')
            self.assertEqual(module.EXPECTED['display_version'], '1.0.0')
            base = module.EXPECTED['exefs_sha256']['main']
            module.select_version('v210')
            self.assertEqual(module.EXPECTED['display_version'], '2.1.0')
            self.assertNotEqual(module.EXPECTED['exefs_sha256']['main'], base)
        finally:
            if before is None: os.environ.pop('DREAD_VERSION', None)
            else: os.environ['DREAD_VERSION'] = before

    def test_archive_checks_same_lists_and_fresh_process(self):
        import subprocess
        for game in ('dread', 'worldend'):
            module = importlib.import_module(game+'_pipelines')
            with self.subTest(game=game), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); profile = root/'build'; profile.mkdir()
                (profile/'pso_manifest.tsv').write_text('C\tfixture\n')
                archive = root/'pipeline-archive'; archive.mkdir(); saved = archive/'old.mngarc'; saved.write_bytes(b'old')
                calls = []
                def run(argv, **kw):
                    calls.append(kw['env'])
                    self.assertEqual(kw['env']['DM_PSO_MANIFEST'], str(profile/'pso_manifest.tsv'))
                    kw['stdout'].write('[MNG] manifest archive: 1 requested, 1 built, 1 shaders, success\n')
                    return subprocess.CompletedProcess(argv, 0)
                with mock.patch.object(module, 'run_outside_bundle', side_effect=run):
                    module.mac_pipelines(root/'host', profile, root/'libraries', root, {})
                self.assertEqual(calls[1]['DM_PSO_REQUIRE_ARCHIVE'], '1')
                self.assertEqual(saved.read_bytes(), b'old')
                (profile/'pso_manifest.tsv').write_text('C\tchanged\n')
                with self.assertRaisesRegex(ValueError, 'another warm-up list'): module.check_archive_lists(archive, profile)

    def test_dread_romfs_reuse_checks_bytes_and_missing_file(self):
        import hashlib
        import json
        import update_runtime
        module = update_runtime.standalone_module('dread')
        with tempfile.TemporaryDirectory() as tmp:
            romfs = Path(tmp)/'extracted/v100/romfs'
            romfs.mkdir(parents=True)
            payload = b'original synthetic data'
            digest = hashlib.sha256(payload).hexdigest()
            (romfs/'index.json').write_text(json.dumps({'status': 'pass', 'romfs_sha256': digest}))
            with mock.patch.object(module, 'EXPECTED', {'romfs': {'sha256': digest, 'size': len(payload)}}):
                self.assertFalse(module.romfs_done(romfs))
                (romfs/'romfs.bin').write_bytes(payload)
                self.assertTrue(module.romfs_done(romfs))
                (romfs/'romfs.bin').write_bytes(b'X' * len(payload))
                self.assertFalse(module.romfs_done(romfs))


if __name__ == '__main__': unittest.main()

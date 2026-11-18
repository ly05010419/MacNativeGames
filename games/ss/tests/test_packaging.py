"""Packaging regressions with real signing/publication and synthetic resources."""
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME/'scripts'))
import ss_bundle as bundle
import ss_pipelines as pipelines


class Packaging(unittest.TestCase):
    def test_hash_reuse_and_mutation_with_restored_mtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); p = root/'data'; p.write_bytes(b'first')
            d = bundle.Digests(root/'digests.json'); initial = d.sha256(p); d.save()
            with mock.patch('hashlib.file_digest', side_effect=AssertionError('rehash')):
                self.assertEqual(bundle.Digests(d.record).sha256(p), initial)
            before = p.stat(); p.write_bytes(b'other'); os.utime(p, ns=(before.st_atime_ns, before.st_mtime_ns))
            self.assertNotEqual(d.sha256(p), initial)

    @unittest.skipUnless(sys.platform == 'darwin', 'macOS signing and APFS publication')
    def test_signed_shader_bundle_is_preserved_and_failed_seal_keeps_old_app(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); old = root/'Old.app'; new = root/'New.app'; data = root/'libs'; data.mkdir()
            (data/'test.metallib').write_text('synthetic test data')
            digests = bundle.Digests(root/'hashes.json')
            first = bundle.signed_bundle(old, root/'absent', 'ShaderData', {'libs': data}, digests, os.environ)
            second = bundle.signed_bundle(new, old, 'ShaderData', {'libs': data}, digests, os.environ)
            game_data = bundle.signed_bundle(old, root/'absent', 'GameData', {'RomFS': data}, digests, os.environ)
            bundle.signed_bundle(new, old, 'GameData', {'RomFS': data}, digests, os.environ)
            data_inode = game_data.stat().st_ino
            seal = 'Contents/_CodeSignature/CodeResources'
            self.assertEqual((first/seal).read_bytes(), (second/seal).read_bytes())
            inode = old.stat().st_ino; shader_inode = first.stat().st_ino
            with self.assertRaisesRegex(Exception, 'signature does not verify'):
                bundle.install_app(new, old, verify=lambda _: False, remove=lambda _: None)
            self.assertEqual(first.stat().st_ino, shader_inode)
            self.assertEqual(game_data.stat().st_ino, data_inode)
            self.assertTrue((second/seal).exists())
            bundle.install_app(new, old, verify=lambda _: True, remove=lambda _: None)
            self.assertEqual(old.stat().st_ino, inode)
            self.assertEqual(first.stat().st_ino, shader_inode)
            self.assertEqual(game_data.stat().st_ino, data_inode)
            (data/'test.metallib').write_text('changed')
            third = bundle.signed_bundle(root/'Third.app', old, 'ShaderData', {'libs': data}, digests, os.environ)
            self.assertNotEqual((first/seal).read_bytes(), (third/seal).read_bytes())

    def test_archive_retains_files_checks_completion_and_exact_lists(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); profile = root/'profile'; profile.mkdir()
            (profile/'pso_manifest.tsv').write_text('R\tfixture\n')
            archive = root/'pipeline-archive'; archive.mkdir(); saved = archive/'1.mtl4a'; saved.write_bytes(b'old archive')
            host = root/'tools/dmhost'
            def run(argv, **kw):
                self.assertEqual(argv, [str(host)])
                self.assertGreater(int(kw['env']['DM_PSO_PREPARE_THREADS']), 1)
                self.assertEqual(kw['env']['DM_PSO_MANIFEST'], str(profile/'pso_manifest.tsv'))
                kw['stdout'].write('[MNG] manifest archive: 1 requested, 1 built, success\n')
                return subprocess.CompletedProcess(argv, 0)
            with mock.patch.object(pipelines, 'run_tool', side_effect=run):
                pipelines.mac_pipelines(host, profile, root/'cache/b-chain', root, {})
            self.assertEqual(saved.read_bytes(), b'old archive')
            pipelines.check_archive_lists(archive, profile)
            (profile/'pso_manifest.tsv').write_text('R\tdifferent\n')
            with self.assertRaisesRegex(ValueError, 'another warm-up list'):
                pipelines.check_archive_lists(archive, profile)
            with mock.patch.object(pipelines, 'run_tool', return_value=subprocess.CompletedProcess([], 0)):
                with self.assertRaisesRegex(ValueError, 'did not complete'):
                    pipelines.mac_pipelines(host, profile, root/'cache/b-chain', root, {})

    def test_mng_selects_ss_build_and_preserves_keys(self):
        from tools.builder_jobs import build_job, repo, installed
        self.assertEqual(repo.game_of_title('01002DA013484800'), 'ss')
        self.assertIn('org.macnativegames.ss', installed.bundle_ids())
        with tempfile.TemporaryDirectory() as tmp:
            script, args, work, app, kind, game = build_job.command({
                'plan': {'game': 'ss', 'base': '/base.nsp', 'update': '/update.nsp', 'keys': '/prod.keys'},
                'output_dir': tmp, 'work_root': tmp})
            self.assertEqual(script, GAME/'build.py')
            self.assertIn('/prod.keys', args)
            self.assertEqual((kind, game), ('full', 'ss'))

    def test_unknown_existing_output_is_not_replaced(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = Path(tmp)/'Other.app'; app.mkdir()
            with self.assertRaisesRegex(ValueError, 'not an SS app'):
                bundle.check_output(app)

    def test_update_refuses_a_work_folder_inside_the_checkout(self):
        import ss_update
        with self.assertRaisesRegex(ValueError, 'outside the checkout'):
            ss_update.update(Path('/nonexistent/Skyward.app'), GAME/'build-output')


if __name__ == '__main__':
    unittest.main()

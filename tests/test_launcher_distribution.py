"""A downloaded Launcher works after relocation without the developer checkout or a separately installed Python."""
import hashlib
import importlib
import io
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    return importlib.import_module('tools.builder_app.' + name)


builder = module('make_app')
bundled = module('bundled_job')


class Distribution(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.resources = self.root / 'Moved App.app/Contents/Resources'
        self.resources.mkdir(parents=True)
        self.support = self.root / 'support'

    def archive(self, files=None):
        files = files or {'build_app.py': b'# source', 'tools/builder_jobs/__main__.py': b'print("ok")'}
        path = self.resources / 'source.tar.gz'
        with tarfile.open(path, 'w:gz') as t:
            for name, data in files.items():
                member = tarfile.TarInfo(name)
                member.size = len(data)
                member.mode = 0o755 if name.endswith('.py') else 0o644
                t.addfile(member, io.BytesIO(data))
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        (self.resources.parent / 'Info.plist').write_bytes(plistlib.dumps({
            'MNGSourceArchiveSHA256': digest, 'MNGSourceCommit': 'a' * 40,
        }))
        return path, digest

    def test_relocation_and_reuse_need_no_checkout_or_writable_app(self):
        archive, digest = self.archive()
        source = bundled.prepare(self.resources, self.support)
        self.assertEqual(source, self.support / 'sources' / digest)
        self.assertTrue(os.access(source / 'build_app.py', os.X_OK))
        self.assertEqual((source / '.mng-source-commit').read_text().strip(), 'a' * 40)
        archive.unlink()  # no extraction or dependency on the original app on subsequent jobs
        self.assertEqual(bundled.prepare(self.resources, self.support), source)

    def test_changed_release_gets_its_own_source_and_preserves_the_previous_one(self):
        self.archive()
        previous = bundled.prepare(self.resources, self.support)
        self.archive({'build_app.py': b'# next release', 'tools/builder_jobs/__main__.py': b'pass'})
        current = bundled.prepare(self.resources, self.support)
        self.assertNotEqual(previous, current)
        self.assertEqual((previous / 'build_app.py').read_bytes(), b'# source')

    def test_bad_digest_and_unsafe_archive_never_publish_a_source_folder(self):
        path, digest = self.archive()
        path.write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'damaged'):
            bundled.prepare(self.resources, self.support)
        self.assertFalse((self.support / 'sources' / digest).exists())
        self.archive({'../escaped': b'bad', 'build_app.py': b'x'})
        with self.assertRaises(tarfile.TarError):
            bundled.prepare(self.resources, self.support)
        self.assertFalse((self.support / 'sources/escaped').exists())
        self.assertEqual(list((self.support / 'sources').glob('.source-*')), [])

    def test_parallel_jobs_install_once_and_run_from_the_bundled_source(self):
        program = b'import json, os, sys; print(json.dumps(dict(cwd=os.getcwd(), repo=os.environ["MNG_REPO"], args=sys.argv[1:])))'
        self.archive({'build_app.py': b'# source', 'tools/builder_jobs/__main__.py': program})
        script = self.resources / 'bundled_job.py'
        shutil.copy2(ROOT / 'tools/builder_app/bundled_job.py', script)
        env = dict(os.environ, HOME=str(self.root / 'new user'), PYTHONDONTWRITEBYTECODE='1')
        env.pop('PYTHONPATH', None)
        processes = [subprocess.Popen([sys.executable, str(script), 'checks'], cwd=self.root,
                                      env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for _ in range(3)]
        try:
            outputs = []
            for p in processes:
                out, err = p.communicate(timeout=30)
                self.assertEqual(p.returncode, 0, err)
                outputs.append(json.loads(out))
            self.assertEqual(outputs, [outputs[0]] * 3)
            self.assertEqual(outputs[0]['cwd'], outputs[0]['repo'])
            self.assertEqual(outputs[0]['args'], ['checks'])
            self.assertIn('/new user/Library/Application Support/', outputs[0]['repo'])
        finally:
            for p in processes:
                if p.poll() is None:
                    p.kill()
                p.wait()

    def test_source_export_excludes_untracked_data_and_keeps_internal_symlinks(self):
        repo = self.root / 'repo'
        repo.mkdir()
        def git(*args):
            return subprocess.run(['git', '-C', str(repo), *args], check=True, capture_output=True)
        git('init', '-q')
        files = ('build_app.py', 'tools/builder_jobs/__main__.py', 'tools/builder_app/bundled_job.py',
                 'games/totk/third_party/d2r_gfx/gfx/nvn/totk_seed_image.h',
                 'games/oot/ios/tools/seed_image.py', 'games/totk/ios/tools/seed_image.py')
        for name in files:
            p = repo / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('# fixture\n')
        (repo / 'linked.py').symlink_to('build_app.py')
        git('add', '.')
        git('-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', '-c', 'commit.gpgsign=false', 'commit', '-qm', 'fixture')
        (repo / 'prod.keys').write_text('must stay local')
        (repo / 'CLAUDE.md').write_text('private notes')
        archive = self.resources / 'source.tar.gz'
        builder.source_archive(repo, archive)
        with tarfile.open(archive) as tar:
            self.assertNotIn('prod.keys', tar.getnames())
            self.assertNotIn('CLAUDE.md', tar.getnames())
            self.assertTrue(tar.getmember('linked.py').issym())
        (repo / 'build_app.py').write_text('# changed')
        with self.assertRaisesRegex(ValueError, 'committed'):
            builder.source_archive(repo, archive)
        git('checkout', '--', 'build_app.py')
        git('rm', '--cached', files[-1])
        git('-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', '-c', 'commit.gpgsign=false', 'commit', '-qm', 'missing dependency')
        with self.assertRaisesRegex(ValueError, 'commit these dependencies'):
            builder.source_archive(repo, archive)

    def test_portable_source_records_game_build_identity_without_git(self):
        from tools.builder_jobs import repo
        build = repo.build_app()
        (self.root / '.mng-source-commit').write_text('abcdef0123456789\n')
        with mock.patch.object(build, 'REPO', self.root), mock.patch.object(build.subprocess, 'run') as run:
            self.assertEqual(build.source_commit(), 'abcdef0')
            run.assert_not_called()

    def test_portable_launcher_passes_migration_without_recompiling_it(self):
        from tools.builder_jobs import migrate
        self.archive()
        source = bundled.prepare(self.resources, self.support)
        with mock.patch.object(migrate, 'LAUNCHER', self.resources.parent.parent), mock.patch.object(migrate.repo, 'REPO', source):
            self.assertEqual(migrate.launcher_state()['state'], 'ok')


if __name__ == '__main__':
    unittest.main()

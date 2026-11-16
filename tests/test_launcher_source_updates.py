"""A downloaded Launcher takes newer build scripts from a signed source release (user 2026-11-16: "做根本方案，手动挑版本发").

Without this, a Launcher only ever ran the source it was shipped with: an express update rebuilt the same old code, and a
fix reached players only through a new Launcher download. Now a source release on GitHub (tag source-*, published by
hand with the private release tool) carries mng-source.json, its Ed25519 signature and the archive. The Launcher checks
the signature with the public key in its own signed Info.plist, then the archive's size and SHA-256, and runs the newest
verified source that is newer than its own and asks for no newer Launcher. These tests serve the release from a local
HTTP server and run the real bundled_job.py."""
import base64
import hashlib
import http.server
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
import threading
import time
import unittest

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

ROOT = Path(__file__).resolve().parents[1]
updates = importlib.import_module('tools.builder_app.source_updates')
builder = importlib.import_module('tools.builder_app.make_app')

# the fake tools.builder_jobs both sources carry: says which source ran it
PROGRAM = (b'import json, os, sys\n'
           b'print(json.dumps(dict(repo=os.environ["MNG_REPO"], commit=open(".mng-source-commit").read().strip(),'
           b' args=sys.argv[1:])))\n')


def key_pair():
    private = Ed25519PrivateKey.generate()
    raw_private = private.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                        serialization.NoEncryption())
    raw_public = private.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return base64.b64encode(raw_private).decode(), base64.b64encode(raw_public).decode()


def tar_gz(files):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w:gz') as t:
        for name, data in files.items():
            member = tarfile.TarInfo(name)
            member.size = len(data)
            member.mode = 0o644
            t.addfile(member, io.BytesIO(data))
    return buf.getvalue()


def manifest(archive, commit, commit_time, min_launcher='1.0.2'):
    return json.dumps({
        'format': 1, 'commit': commit, 'commit_time': commit_time, 'archive': f'mng-source-{commit[:12]}.tar.gz',
        'archive_sha256': hashlib.sha256(archive).hexdigest(), 'archive_size': len(archive), 'min_launcher': min_launcher,
    }, indent=1).encode()


class Server:
    """Serves a GitHub-like release list and its assets; counts the requests per path."""
    def __init__(self):
        self.files, self.hits = {}, {}
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                outer.hits[self.path] = outer.hits.get(self.path, 0) + 1
                body = outer.files.get(self.path)
                if body is None:
                    self.send_response(404); self.end_headers(); return
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.url = f'http://127.0.0.1:{self.httpd.server_address[1]}'
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def release(self, tag, published, data, signature, archive, archive_name):
        assets = []
        for name, body in (('mng-source.json', data), ('mng-source.json.sig', signature), (archive_name, archive)):
            self.files[f'/download/{tag}/{name}'] = body
            assets.append({'name': name, 'browser_download_url': f'{self.url}/download/{tag}/{name}'})
        listing = json.loads(self.files.get('/releases', b'[]'))
        listing.append({'tag_name': tag, 'draft': False, 'published_at': published, 'assets': assets})
        self.files['/releases'] = json.dumps(listing).encode()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class SourceUpdates(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.home = self.root / 'home'
        self.support = self.home / 'Library/Application Support/MacNativeGames/Builder'
        self.resources = self.root / 'MNG Launcher.app/Contents/Resources'
        self.resources.mkdir(parents=True)
        self.private, self.public = key_pair()
        self.server = Server()
        self.addCleanup(self.server.close)

    # --- the Launcher as shipped: its own source, key and version --------------------------------------------------------
    def launcher(self, version='1.0.2', commit_time=1000, api=None):
        archive = tar_gz({'build_app.py': b'# bundled', 'tools/builder_jobs/__main__.py': PROGRAM})
        (self.resources / 'source.tar.gz').write_bytes(archive)
        (self.resources.parent / 'Info.plist').write_bytes(plistlib.dumps({
            'CFBundleShortVersionString': version, 'MNGSourceCommit': 'b' * 40, 'MNGSourceCommitTime': commit_time,
            'MNGSourceArchiveSHA256': hashlib.sha256(archive).hexdigest(), 'MNGSourcePublicKey': self.public,
            'MNGSourceRepo': 'owner/repo', 'MNGSourceAPI': api or self.server.url + '/releases',
        }))
        for name in ('bundled_job.py', 'source_updates.py'):
            shutil.copy2(ROOT / 'tools/builder_app' / name, self.resources / name)

    def publish(self, commit='c' * 40, commit_time=2000, min_launcher='1.0.2', tag='source-20261116-cccccccc',
                published='2026-11-16T10:00:00Z', tamper_archive=False, bad_signature=False):
        archive = tar_gz({'build_app.py': b'# downloaded', 'tools/builder_jobs/__main__.py': PROGRAM})
        data = manifest(archive, commit, commit_time, min_launcher)
        signature = updates.sign(data, self.private).encode()
        if bad_signature:
            signature = updates.sign(data + b' ', self.private).encode()
        served = archive + b'x' if tamper_archive else archive
        self.server.release(tag, published, data, signature, served, f'mng-source-{commit[:12]}.tar.gz')
        return hashlib.sha256(archive).hexdigest()

    def job(self, *args, env=None):
        env = dict(os.environ, HOME=str(self.home), PYTHONDONTWRITEBYTECODE='1', MNG_LANG='en', **(env or {}))
        env.pop('PYTHONPATH', None)
        env.pop('MNG_SOURCE_UPDATES', None)
        done = subprocess.run([sys.executable, str(self.resources / 'bundled_job.py'), *args], cwd=self.root, env=env,
                              capture_output=True, text=True, timeout=120)
        self.assertEqual(done.returncode, 0, done.stderr)
        result = json.loads(done.stdout.strip().splitlines()[-1])
        return result, done

    def wait_installed(self, digest, seconds=60):
        ready = self.support / 'sources' / digest / '.ready'
        deadline = time.time() + seconds
        while time.time() < deadline and not ready.is_file():
            time.sleep(0.2)
        return ready.is_file()

    # --- signature and manifest ------------------------------------------------------------------------------------------
    def test_only_the_launchers_key_and_untouched_bytes_verify(self):
        archive = b'archive'
        data = manifest(archive, 'c' * 40, 5)
        signature = updates.sign(data, self.private)
        self.assertEqual(updates.verify(data, signature, self.public)['commit'], 'c' * 40)
        with self.assertRaises(updates.SourceError):
            updates.verify(data.replace(b'"commit_time": 5', b'"commit_time": 6'), signature, self.public)
        with self.assertRaises(updates.SourceError):
            updates.verify(data, signature, key_pair()[1])
        with self.assertRaises(updates.SourceError):
            updates.verify(data, 'not base64!', self.public)

    def test_a_manifest_with_a_wrong_field_is_refused_even_when_signed(self):
        good = json.loads(manifest(b'a', 'c' * 40, 5))
        for change in ({'commit': 'xyz'}, {'commit_time': '5'}, {'archive_sha256': '0' * 63}, {'archive_size': 2 ** 40},
                       {'min_launcher': '1.0'}, {'archive': '../escape.tar.gz'}, {'format': 2}, {'extra': 1}):
            with self.subTest(change=change):
                data = json.dumps(dict(good, **change)).encode()
                with self.assertRaises(updates.SourceError):
                    updates.verify(data, updates.sign(data, self.private), self.public)

    # --- what a job runs ------------------------------------------------------------------------------------------------
    def test_an_update_downloads_the_signed_source_and_runs_it(self):
        self.launcher()
        digest = self.publish()
        result, done = self.job('build', 'request.json')
        self.assertEqual(result['commit'], 'c' * 40)
        # bundled_job resolves links (the release guard's temporary folder is one), so compare real paths
        self.assertEqual(Path(result['repo']).resolve(), (self.support / 'sources' / digest).resolve())
        self.assertEqual(result['args'], ['build', 'request.json'])
        self.assertIn('cccccccccccc', done.stdout + done.stderr)   # the log says which code it uses
        # the next job runs it without the network
        self.server.files.clear()
        self.assertEqual(self.job('checks')[0]['commit'], 'c' * 40)

    def test_a_tampered_archive_or_signature_never_runs(self):
        for kind in ('tamper_archive', 'bad_signature'):
            with self.subTest(kind=kind):
                shutil.rmtree(self.home, ignore_errors=True)
                self.server.files.clear()
                self.launcher()
                digest = self.publish(**{kind: True})
                result, _ = self.job('build', 'request.json')
                self.assertEqual(result['commit'], 'b' * 40)
                self.assertFalse((self.support / 'sources' / digest).exists())
                self.assertEqual(list((self.support / 'sources').glob('.download-*')), [])

    def test_an_older_release_is_never_downloaded(self):
        self.launcher(commit_time=3000)
        self.publish(commit_time=2000)
        result, _ = self.job('build', 'request.json')
        self.assertEqual(result['commit'], 'b' * 40)
        self.assertFalse(any('.tar.gz' in path for path in self.server.hits))

    def test_source_for_a_newer_launcher_says_to_download_it(self):
        self.launcher(version='1.0.2')
        self.publish(min_launcher='1.1.0')
        result, done = self.job('build', 'request.json')
        self.assertEqual(result['commit'], 'b' * 40)
        self.assertIn('1.1.0', done.stdout + done.stderr)
        self.assertFalse(any('.tar.gz' in path for path in self.server.hits))

    def test_no_network_keeps_the_source_on_this_mac(self):
        self.launcher(api='http://127.0.0.1:9/releases')
        result, _ = self.job('build', 'request.json')
        self.assertEqual(result['commit'], 'b' * 40)

    def test_the_newest_of_several_releases_wins(self):
        self.launcher()
        self.publish(commit='d' * 40, commit_time=1500, tag='source-a', published='2026-11-16T09:00:00Z')
        self.publish(commit='e' * 40, commit_time=2500, tag='source-b', published='2026-11-16T11:00:00Z')
        self.assertEqual(self.job('build', 'request.json')[0]['commit'], 'e' * 40)

    def test_the_home_page_answers_at_once_and_fetches_in_the_background(self):
        self.launcher()
        digest = self.publish()
        result, done = self.job('apps')
        self.assertEqual(result['commit'], 'b' * 40)          # this answer: the source already here
        self.assertEqual(len(done.stdout.strip().splitlines()), 1)   # nothing but the job's JSON on stdout
        self.assertTrue(self.wait_installed(digest))
        self.assertEqual(self.job('apps')[0]['commit'], 'c' * 40)
        # the release list is asked once in ten minutes by the home page; the background download takes the signed
        # manifest that check found (and verifies it again) instead of asking GitHub a second time
        self.assertEqual(self.server.hits.get('/releases'), 1)

    def test_a_downloaded_source_is_left_for_a_newer_bundled_one(self):
        self.launcher()
        self.publish()
        self.job('build', 'request.json')
        self.launcher(commit_time=9000)                       # a newer Launcher with newer source of its own
        self.server.files.clear()
        self.assertEqual(self.job('checks')[0]['commit'], 'b' * 40)

    # --- the Launcher build carries the key and the commit time --------------------------------------------------------
    def test_make_app_records_the_public_key_repository_and_commit_time(self):
        settings = builder.source_settings(ROOT)
        key = (ROOT / 'tools/builder_app/source_signing.pub').read_text().strip()
        self.assertEqual(settings['MNGSourcePublicKey'], key)
        self.assertEqual(len(base64.b64decode(key)), 32)
        self.assertEqual(settings['MNGSourceRepo'], 'ly05010419/MacNativeGames')
        self.assertIsInstance(settings['MNGSourceCommitTime'], int)
        self.assertNotIn('MNGSourceAPI', settings)


if __name__ == '__main__':
    unittest.main()

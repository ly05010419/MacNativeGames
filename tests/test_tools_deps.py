"""tools/deps without the network: digests, the reentrant lock, staging, the cache layout build_app.py's caches use, and
the command line a Makefile calls (with an installed folder already in a temporary cache)."""
import hashlib
import io
import os
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

import tools.deps as deps  # noqa: E402
from tools.deps import core, pins  # noqa: E402


class Cache(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cache = Path(self.tmp.name)
        self.env = mock.patch.dict(os.environ, {'MNG_DEPS_CACHE': str(self.cache)})
        self.env.start()

    def tearDown(self):
        self.env.stop()
        self.tmp.cleanup()


class Pins(unittest.TestCase):
    def test_every_source_has_a_recipe_a_marker_and_a_full_digest(self):
        for name, (version, url, pin) in pins.SOURCES.items():
            with self.subTest(name=name):
                self.assertIn(name, deps.RECIPES)
                self.assertIn(name, deps.MARKER)
                self.assertTrue(url.startswith('https://'))
                self.assertRegex(pin, r'^[0-9a-f]{40}$' if url.endswith('.git') else r'^[0-9a-f]{64}([0-9a-f]{64})?$')

    def test_folder_names_are_the_ones_build_app_made(self):
        self.assertEqual(deps._folder('opus'), 'opus-1.6.1')
        self.assertEqual(deps._folder('libvpx'), 'libvpx-1.16.0-1024874c5919')
        self.assertEqual(deps._folder('dav1d'), 'dav1d-1.5.3')
        key = lambda names: hashlib.sha256(repr({n: pins.BUILD_TOOLS[n] for n in sorted(names)}).encode()).hexdigest()[:12]
        self.assertEqual(key(('meson', 'ninja')), '7678300352e8')
        self.assertEqual(key(('cmake', 'meson', 'ninja')), 'd27b3914577e')


class Downloads(Cache):
    def test_a_download_is_kept_only_when_its_digest_matches(self):
        payload = b'pinned bytes'
        want = hashlib.sha256(payload).hexdigest()

        def curl(args, log_name, **_):
            Path(args[args.index('-o') + 1]).write_bytes(payload)
        with mock.patch.object(core, 'run', side_effect=curl) as run:
            archive = core.fetch('https://example.invalid/a.tar.gz', want, 'a.tar.gz')
            self.assertEqual(archive.read_bytes(), payload)
            self.assertEqual(run.call_args[0][0][0], '/usr/bin/curl')       # never urllib: python.org has no CA certificates
            core.fetch('https://example.invalid/a.tar.gz', want, 'a.tar.gz')
            self.assertEqual(run.call_count, 1)                              # kept: not downloaded again
            with self.assertRaises(deps.DepsError):
                core.fetch('https://example.invalid/b.tar.gz', '0' * 64, 'b.tar.gz')
        self.assertFalse((self.cache / 'src/b.tar.gz').exists())
        self.assertFalse((self.cache / 'src/b.tar.gz.partial').exists())

    def test_sha512_pins_are_checked_as_sha512(self):
        payload = b'sdk'
        want = hashlib.sha512(payload).hexdigest()
        with mock.patch.object(core, 'run', side_effect=lambda a, l, **k: Path(a[a.index('-o') + 1]).write_bytes(payload)):
            self.assertEqual(core.fetch('https://example.invalid/sdk.tar.gz', want, 'sdk.tar.gz').read_bytes(), payload)

    def test_unpack_refuses_paths_leaving_the_folder(self):
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode='w:gz') as t:
            info = tarfile.TarInfo('../escape.txt'); info.size = 1
            t.addfile(info, io.BytesIO(b'x'))
        archive = self.cache / 'bad.tar.gz'
        archive.write_bytes(data.getvalue())
        with self.assertRaises(Exception):
            core.unpack(archive, self.cache / 'out')
        self.assertFalse((self.cache / 'escape.txt').exists())


class Prefixes(Cache):
    def test_an_installed_folder_is_used_as_it_is(self):
        folder = self.cache / 'opus-1.6.1/lib'
        folder.mkdir(parents=True)
        (folder / 'libopus.a').write_bytes(b'')
        with mock.patch.dict(deps.RECIPES, {'opus': mock.Mock(side_effect=AssertionError('made again'))}):
            self.assertEqual(deps.prefix('opus'), self.cache / 'opus-1.6.1')
            self.assertEqual(deps.env('opus'), {'OPUS_PREFIX': str(self.cache / 'opus-1.6.1')})

    def test_a_failed_recipe_leaves_no_prefix(self):
        def half(stage):
            (stage / 'lib').mkdir(parents=True)
            raise deps.DepsError('make failed')
        with mock.patch.dict(deps.RECIPES, {'ogg': half}), self.assertRaises(deps.DepsError):
            deps.prefix('ogg')
        self.assertFalse((self.cache / 'libogg-1.3.6').exists())

    def test_the_lock_nests_in_one_process(self):
        done = []

        def inner():
            with core.locked():
                with core.locked():          # a second flock on a new open() would wait for itself
                    done.append(True)
        thread = threading.Thread(target=inner)
        thread.start(); thread.join(5)
        self.assertEqual(done, [True])

    def test_unknown_names_say_what_exists(self):
        with self.assertRaisesRegex(deps.DepsError, 'opus'):
            deps.prefix('opsu')
        with self.assertRaisesRegex(deps.DepsError, 'xcodegen'):
            deps.program('xcodegn')

    def test_the_command_line_a_makefile_calls(self):
        (self.cache / 'libvpx-1.16.0-1024874c5919/lib').mkdir(parents=True)
        (self.cache / 'libvpx-1.16.0-1024874c5919/lib/libvpx.a').write_bytes(b'')
        env = dict(os.environ, MNG_DEPS_CACHE=str(self.cache))
        done = subprocess.run([sys.executable, str(REPO / 'tools/deps'), 'prefix', 'libvpx'], env=env, capture_output=True, text=True)
        self.assertEqual((done.returncode, done.stdout.strip()), (0, str(self.cache / 'libvpx-1.16.0-1024874c5919')))
        done = subprocess.run([sys.executable, str(REPO / 'tools/deps'), 'prefix', 'nothing'], env=env, capture_output=True, text=True)
        self.assertEqual((done.returncode, done.stdout), (1, ''))
        self.assertIn('unknown dependency', done.stderr)


if __name__ == '__main__':
    unittest.main()

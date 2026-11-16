"""Ocarina of Time's express update does not seal, clone and delete what did not change (user 2026-11-16: "极速更新 还是 太慢
看看如何优化"). Measured that day (scratch/runs/2026-11/express-speed): an App clone signed in 68.7 s with the 6.8 GB pipeline
archive in Contents/Resources and in 9.6 s without it, each seal check took 10.9 s against 0.4 s, and the shader data's
193,375 files were cloned into every new App (one App clone: 101-111 s) only to be deleted when install_app moved the
installed, identical bundle in. Now the archive is a signed bundle of its own (pipeline_bundle), signed again only when an
archive file changed, and an unchanged helper bundle goes into the new App as its signature alone (place_helper), the
installed one being moved in (build_app.keep_game_data), as update_runtime.py does for Tears of the Kingdom."""
import importlib.util
import json
import os
import plistlib
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'
sys.path.insert(0, str(ROOT))
import build_app  # noqa: E402


def load_build():
    spec = importlib.util.spec_from_file_location('oot_build_pipeline_bundle', GAME / 'build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def bundle(path, payload, signature=b'sig-1'):
    """A helper bundle as the packaging makes one: Info.plist, its seal, its resources."""
    (path / 'Contents/_CodeSignature').mkdir(parents=True)
    (path / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'org.macnativegames.oot.pipelines.oot'}))
    (path / 'Contents/_CodeSignature/CodeResources').write_bytes(signature)
    (path / 'Contents/Resources/pipelines').mkdir(parents=True)
    (path / 'Contents/Resources/pipelines/pipelines-a.metalarchive').write_bytes(payload)
    return path


class PipelineBundle(unittest.TestCase):
    def setUp(self):
        self.build = load_build()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)

    def archive(self):
        p = self.base / 'pipelines'
        p.mkdir()
        (p / 'pipelines-arch-1-8.metalarchive').write_bytes(b'archive')
        (p / 'pipelines-arch.tsv').write_text('k\t1-8\n')
        (p / 'pipelines-arch.json').write_text('{"schema": 1}\n')
        (p / 'compute-manifest.json').write_text('{"runtime_sha256": "a", "seconds": 95.0}\n')
        return p

    def test_the_stamp_follows_the_archive_not_its_record(self):
        p = self.archive()
        first = self.build.pipeline_files_stamp(p)
        (p / 'compute-manifest.json').write_text('{"runtime_sha256": "b", "seconds": 91.2}\n')   # every update rewrites it
        (p / 'pipelines-arch.tsv').write_text('k\t1-8\n')                                     # rewritten, same bytes
        self.assertEqual(self.build.pipeline_files_stamp(p), first)
        (p / 'pipelines-arch.tsv').write_text('k\t1-8\nk2\t1-8\n')                            # the index changed
        second = self.build.pipeline_files_stamp(p)
        self.assertNotEqual(second, first)
        st = (p / 'pipelines-arch-1-8.metalarchive').stat()
        os.utime(p / 'pipelines-arch-1-8.metalarchive', ns=(st.st_atime_ns, st.st_mtime_ns + 1000))   # the tool wrote it again
        self.assertNotEqual(self.build.pipeline_files_stamp(p), second)

    def test_an_unchanged_bundle_goes_in_as_its_signature(self):
        work = bundle(self.base / 'work/Pipelines-oot.bundle', b'archive')
        installed = self.base / 'Installed.app'
        bundle(installed / 'Contents/Helpers/Pipelines-oot.bundle', b'archive')
        new = self.base / 'New.app'
        self.assertTrue(self.build.place_helper(work, new, installed))
        placed = new / 'Contents/Helpers/Pipelines-oot.bundle'
        self.assertEqual((placed / 'Contents/_CodeSignature/CodeResources').read_bytes(), b'sig-1')
        self.assertFalse((placed / 'Contents/Resources').exists())   # no clone of the files: the installed bundle moves in

    def test_a_changed_or_missing_bundle_is_cloned_whole(self):
        work = bundle(self.base / 'work/Pipelines-oot.bundle', b'new archive', signature=b'sig-2')
        installed = self.base / 'Installed.app'
        bundle(installed / 'Contents/Helpers/Pipelines-oot.bundle', b'archive')
        for target, old in ((self.base / 'New.app', installed), (self.base / 'First.app', self.base / 'None.app')):
            with self.subTest(target=target.name):
                self.assertFalse(self.build.place_helper(work, target, old))
                self.assertEqual((target / 'Contents/Helpers/Pipelines-oot.bundle/Contents/Resources/pipelines/pipelines-a.metalarchive').read_bytes(), b'new archive')

    def test_install_moves_the_installed_pipeline_bundle_into_a_signature_only_one(self):
        work = bundle(self.base / 'work/Pipelines-oot.bundle', b'archive')
        installed = self.base / 'Installed.app'
        bundle(installed / 'Contents/Helpers/Pipelines-oot.bundle', b'archive')
        new = self.base / 'New.app'
        self.build.place_helper(work, new, installed)
        build_app.keep_game_data(new, installed, verify=lambda app: True)
        moved = new / 'Contents/Helpers/Pipelines-oot.bundle/Contents/Resources/pipelines/pipelines-a.metalarchive'
        self.assertEqual(moved.read_bytes(), b'archive')
        self.assertFalse((installed / 'Contents/Helpers/Pipelines-oot.bundle').exists())

    def test_a_seal_that_does_not_verify_leaves_the_installed_app_as_it_was(self):
        work = bundle(self.base / 'work/Pipelines-oot.bundle', b'archive')
        installed = self.base / 'Installed.app'
        bundle(installed / 'Contents/Helpers/Pipelines-oot.bundle', b'archive')
        new = self.base / 'New.app'
        self.build.place_helper(work, new, installed)
        with self.assertRaisesRegex(build_app.BuildError, 'pipeline archive could not be moved'):
            build_app.keep_game_data(new, installed, verify=lambda app: False)
        self.assertTrue((installed / 'Contents/Helpers/Pipelines-oot.bundle/Contents/Resources/pipelines/pipelines-a.metalarchive').is_file())

    def test_the_record_is_read_beside_the_lists(self):
        build = self.base / 'App/build'
        build.mkdir(parents=True)
        (build / 'pso_title.tsv').write_text('R\tt\n')
        (build / 'pso_manifest.tsv').write_text('R\tm\n')
        record = {'lists_sha256': {'pso_title.tsv': self.build.sha(build / 'pso_title.tsv'), 'pso_manifest.tsv': 'stale'},
                  'listed_render': 2, 'listed_mesh': 0}
        (build / 'compute-manifest.json').write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, 'pso_manifest.tsv'):
            self.build.check_archive_lists(build)


class PackageAppWiring(unittest.TestCase):
    body = (GAME / 'build.py').read_text()
    package = body[body.index('def package_app'):body.index('\ndef ', body.index('def package_app') + 1)]

    def test_the_archive_is_a_bundle_read_through_build_pipelines(self):
        self.assertIn('piped=pipeline_bundle(tree,pipelines,suffix) if pipelines else None', self.package)
        self.assertIn('kept=place_helper(piped,temporary,out)', self.package)
        self.assertIn("link=data/'build/pipelines'", self.package)
        self.assertIn("/'Contents/Resources/pipelines',link.parent))", self.package)
        self.assertIn("clone_file(pipelines/'compute-manifest.json',data/'build/compute-manifest.json')", self.package)
        self.assertNotIn("shutil.copytree(pipelines,data/'build/pipelines'", self.package)

    def test_the_shader_data_goes_in_by_signature_when_unchanged(self):
        self.assertIn('kept=place_helper(prepared,temporary,out)', self.package)
        self.assertIn('link_shader_data(temporary,data,prepared.name)', self.package)
        self.assertNotIn('place_shader_bundle(prepared', self.package)

    def test_the_seal_is_checked_once_by_install_app(self):
        tail = self.package[self.package.index('build_app.sign_app(temporary'):]
        self.assertNotIn("'codesign','--verify'", tail[:tail.index('install_app(temporary,out)')])

    def test_every_part_is_timed(self):
        for part in ('pipeline archive', 'shader data and pipeline bundles', 'App assembled', 'signed', 'installed, its seal checked'):
            self.assertIn(f"took('{part}')", self.package)

    def test_keep_game_data_moves_pipeline_bundles(self):
        source = (ROOT / 'build_app.py').read_text()
        keep = source[source.index('def keep_game_data'):source.index('\ndef ', source.index('def keep_game_data') + 1)]
        self.assertIn("helpers.glob('Pipelines-*.bundle')", keep)


if __name__ == '__main__':
    unittest.main()

"""Every Mac app's pipeline archive is a signed bundle of its own, placed by signature (user 2026-11-16: "四个游戏 都要支持"
after Ocarina of Time's express update went from 635 to 151 s). Before, Tears of the Kingdom (2.3 GB, 4,636 files),
Breath of the Wild (1.0 GB), Bananza (727 MB) and Pokopia kept it in Contents/Resources, which every update cloned and
sealed again (Tears of the Kingdom: 25 s of signing, 6 s of the seal check, scratch/runs/2026-11/express-speed)."""
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import build_app  # noqa: E402


def helper(path, payload, signature=b'sig'):
    (path / 'Contents/_CodeSignature').mkdir(parents=True)
    (path / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'x'}))
    (path / 'Contents/_CodeSignature/CodeResources').write_bytes(signature)
    (path / 'Contents/Resources/pipelines').mkdir(parents=True)
    (path / 'Contents/Resources/pipelines/pipelines-a-1.metalarchive').write_bytes(payload)
    return path


class SharedPipelineBundle(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)

    def test_the_update_clone_leaves_both_bundles_where_they_are(self):
        app = self.base / 'Game.app'
        shaders = helper(app / 'Contents/Helpers/ShaderData-v143.bundle', b's')
        pipes = helper(app / 'Contents/Helpers/Pipelines-v143.bundle', b'p')
        (app / 'Contents/Resources/totk/build').mkdir(parents=True)
        (app / 'Contents/Resources/totk/build/pso_title.tsv').write_text('R\tx\n')
        self.assertEqual(build_app.app_pipeline_bundle(app, 'v143'), pipes)
        new = self.base / 'Game.app.partial'
        build_app.clone_app_except(app, new, [shaders, pipes])
        for name in ('ShaderData-v143.bundle', 'Pipelines-v143.bundle'):
            self.assertTrue((new / 'Contents/Helpers' / name).is_symlink(), name)   # not 4,636 files cloned
        self.assertTrue((new / 'Contents/Resources/totk/build/pso_title.tsv').is_file())

    def test_build_pipelines_becomes_a_link_into_the_bundle(self):
        app = self.base / 'Game.app'
        res = app / 'Contents/Resources/totk'
        (res / 'build/pipelines').mkdir(parents=True)   # an older app's own folder
        (res / 'build/pipelines/pipelines-a-1.metalarchive').write_bytes(b'old')
        helper(app / 'Contents/Helpers/Pipelines-v143.bundle', b'new')
        removed = []
        original = build_app.remove_in_background
        build_app.remove_in_background = removed.append
        try:
            build_app.link_pipelines(app, res, 'Pipelines-v143.bundle')
        finally:
            build_app.remove_in_background = original
        link = res / 'build/pipelines'
        self.assertTrue(link.is_symlink())
        self.assertFalse(Path(link.readlink()).is_absolute())   # the app moves
        self.assertEqual((link / 'pipelines-a-1.metalarchive').read_bytes(), b'new')
        self.assertEqual(len(removed), 1)   # the old folder goes out of the app, deleted later

    def test_a_bundle_is_signed_once_and_kept_while_its_files_are(self):
        files = self.base / 'files'
        files.mkdir()
        (files / 'pipelines-a-1.metalarchive').write_bytes(b'archive')
        (files / 'pipelines-a.tsv').write_text('k\t1\n')
        signed = []
        original = build_app.run
        build_app.run = lambda cmd, log, **kw: signed.append(cmd[1]) or (Path(cmd[-1]) / 'Contents/_CodeSignature').mkdir(parents=True, exist_ok=True) or (Path(cmd[-1]) / 'Contents/_CodeSignature/CodeResources').write_bytes(b'sig')
        try:
            logs = self.base / 'logs'
            logs.mkdir()
            bundle = self.base / 'work/Pipelines-v143.bundle'
            build_app.pipeline_bundle(files, bundle, 'org.macnativegames.pipelines.v143', logs)
            build_app.pipeline_bundle(files, bundle, 'org.macnativegames.pipelines.v143', logs)
            self.assertEqual(signed.count('--force'), 1)   # the second one keeps it
            (files / 'pipelines-a.tsv').write_text('k\t1\nk2\t1\n')   # the index changed: signed again
            build_app.pipeline_bundle(files, bundle, 'org.macnativegames.pipelines.v143', logs)
            self.assertEqual(signed.count('--force'), 2)
        finally:
            build_app.run = original
        self.assertTrue((bundle / 'Contents/Resources/pipelines/pipelines-a-1.metalarchive').is_file())


class Wiring(unittest.TestCase):
    build = (ROOT / 'build_app.py').read_text()
    update = (ROOT / 'update_runtime.py').read_text()
    shader_update = (ROOT / 'shader_update.py').read_text()

    def body(self, text, head):
        start = text.index(head)
        return text[start:text.index('\ndef ', start + 1)]

    def test_mac_pipelines_places_a_bundle(self):
        mac = self.body(self.build, 'def mac_pipelines(')
        for call in ('mac_pipeline_files(work / \'cache/pipelines\', files, arch, chain)', 'pipeline_bundle(files,',
                     'place_helper(bundle, app, installed)', 'link_pipelines(app, res, bundle.name)'):
            self.assertIn(call, mac)
        self.assertNotIn("build / 'pipelines', arch, chain)", mac)

    def test_every_caller_names_the_installed_app(self):
        self.assertIn('mac_pipelines(tree, res, version, logs, installed=out if out.exists() else None)', self.build)
        self.assertIn('build_app.mac_pipelines(tree, new_res, version, logs, installed=app)', self.update)
        self.assertIn('ba.mac_pipelines(tree, new_res, version, logs, installed=app)', self.shader_update)
        for text, prefix in ((self.update, 'build_app.'), (self.shader_update, 'ba.')):
            self.assertIn(f'{prefix}clone_app_except(app, tmp, skips)', text)
            self.assertIn(f'{prefix}shader_signature_only(tmp, pipes)', text)   # the tool made none: the installed one stays

    def test_a_failed_update_puts_the_pipeline_bundle_back_too(self):
        self.assertIn("glob('Pipelines-*.bundle')", self.body(self.update, 'def restore_replaced('))


if __name__ == '__main__':
    unittest.main()

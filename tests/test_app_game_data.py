"""A repackaged app keeps the game data file of the app it replaces when the data is the same: the image keeps its inode
and times, so the runtime's RomFS verification record (runtime/romfs.c) still matches and the first start after a
packaging does not hash 16 GB again."""
import os
import errno
from unittest import mock
from pathlib import Path
import tempfile
import unittest
import build_app

IMAGE = 'Contents/Resources/romfs.bin'


def data_bundle(app, name='GameData-v143.bundle', seal=b'seal', data=b'romfs'):
    bundle = app/'Contents/Helpers'/name
    (bundle/'Contents/_CodeSignature').mkdir(parents=True)
    (bundle/'Contents/_CodeSignature/CodeResources').write_bytes(seal)
    (bundle/'Contents/Info.plist').write_bytes(b'plist')
    (bundle/'Contents/Resources').mkdir()
    (bundle/IMAGE).write_bytes(data)
    return bundle


def identity(path):
    s = os.stat(path)
    return s.st_ino, s.st_mtime_ns, s.st_ctime_ns, s.st_birthtime


class KeepGameData(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.old, self.new = self.root/'Game.app', self.root/'Game.app.partial'

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_same_data_keeps_the_old_image(self):
        before = identity(data_bundle(self.old)/IMAGE)
        fresh = data_bundle(self.new)
        # When the seal is checked, the app holds the kept bundle and nothing else (a fresh copy set aside inside it is
        # an unsealed resource: codesign --strict refused the first version this way).
        sealed = lambda app: sorted(p.name for p in (app/'Contents/Helpers').iterdir()) == ['GameData-v143.bundle']
        self.assertTrue(build_app.keep_game_data(self.new, self.old, verify=sealed))
        self.assertEqual(identity(fresh/IMAGE), before)
        self.assertEqual(sorted(p.name for p in (self.new/'Contents/Helpers').iterdir()), ['GameData-v143.bundle'])

    def test_other_data_is_not_kept(self):
        for change in [dict(seal=b'another seal'), dict(data=b'romfs, longer')]:
            with self.subTest(change=change):
                old_image = data_bundle(self.old, **change)/IMAGE
                fresh = data_bundle(self.new)
                own = identity(fresh/IMAGE)
                self.assertFalse(build_app.keep_game_data(self.new, self.old, verify=lambda app: True))
                self.assertEqual(identity(fresh/IMAGE), own)
                self.assertTrue(old_image.is_file())
                build_app.remove_tree(self.old); build_app.remove_tree(self.new)

    def test_the_shader_data_moves_into_a_signature_only_bundle(self):
        """update_runtime.py: the new app carries only the installed ShaderData bundle's signature (shader_signature_only);
        installing moves the installed bundle in, with one seal check for both bundles."""
        data_bundle(self.old)
        shaders = data_bundle(self.old, name='ShaderData-v143.bundle', seal=b'shaders', data=b'seed')
        seed = identity(shaders/IMAGE)
        data_bundle(self.new)
        (self.new/'Contents/Resources/totk').mkdir(parents=True)
        build_app.shader_signature_only(self.new, shaders)
        self.assertFalse((self.new/'Contents/Helpers/ShaderData-v143.bundle/Contents/Resources').exists())
        checks = []
        sealed = lambda app: checks.append(1) or sorted(p.name for p in (app/'Contents/Helpers').iterdir()) == ['GameData-v143.bundle', 'ShaderData-v143.bundle']
        self.assertTrue(build_app.keep_game_data(self.new, self.old, verify=sealed))
        self.assertEqual(identity(self.new/'Contents/Helpers/ShaderData-v143.bundle'/IMAGE), seed)
        self.assertEqual(checks, [1])

    def test_a_signature_only_shader_bundle_that_cannot_be_completed_stops_the_install(self):
        shaders = data_bundle(self.old, name='ShaderData-v143.bundle', seal=b'shaders', data=b'seed')
        build_app.shader_signature_only(self.new, shaders)
        (shaders/'Contents/_CodeSignature/CodeResources').write_bytes(b'changed meanwhile')   # no longer the same bundle
        with self.assertRaisesRegex(build_app.BuildError, 'shader data could not be moved'):
            build_app.keep_game_data(self.new, self.old, verify=lambda app: True)
        self.assertTrue((shaders/IMAGE).is_file())

    def test_clone_except_links_the_skipped_folder(self):
        data_bundle(self.old)
        shaders = data_bundle(self.old, name='ShaderData-v143.bundle', seal=b'shaders', data=b'seed')
        (self.old/'Contents/Resources/totk/build').mkdir(parents=True)
        (self.old/'Contents/Resources/totk/build/shader-seed').symlink_to('../../../Helpers/ShaderData-v143.bundle/Contents/Resources')
        build_app.clone_app_except(self.old, self.new, shaders)
        link = self.new/'Contents/Helpers/ShaderData-v143.bundle'
        self.assertTrue(link.is_symlink() and link.resolve() == shaders.resolve())
        self.assertTrue((self.new/'Contents/Helpers/GameData-v143.bundle'/IMAGE).is_file() and not (self.new/'Contents/Helpers/GameData-v143.bundle').is_symlink())
        self.assertEqual((self.new/'Contents/Resources/totk/build/shader-seed/romfs.bin').read_bytes(), b'seed')   # through both links

    def test_no_app_to_replace(self):
        data_bundle(self.new)
        self.assertFalse(build_app.keep_game_data(self.new, self.old, verify=lambda app: True))

    def test_a_seal_that_no_longer_verifies_puts_the_fresh_copy_back(self):
        old_image = data_bundle(self.old)/IMAGE
        fresh = data_bundle(self.new, data=b'romfs')
        own = identity(fresh/IMAGE)
        self.assertFalse(build_app.keep_game_data(self.new, self.old, verify=lambda app: False))
        self.assertEqual(identity(fresh/IMAGE), own)
        self.assertTrue(old_image.is_file())


class InstallApp(unittest.TestCase):
    """The new app's seal is checked once (by keep_game_data when it keeps the data, else here), before the installed app
    is touched; the replaced app is renamed aside and removed in the background."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.out, self.new = self.root/'Game.app', self.root/'Game.app.partial'
        self.removed, self.checked = [], []

    def tearDown(self):
        self.tmp.cleanup()

    def install(self, result=True):
        verify = lambda app: self.checked.append(app) or result
        return build_app.install_app(self.new, self.out, verify=verify, remove=self.removed.append)

    def test_kept_data_checks_the_seal_once(self):
        data_bundle(self.out); data_bundle(self.new)
        self.assertTrue(self.install())
        self.assertEqual(len(self.checked), 1)
        self.assertFalse(self.new.exists())
        self.assertEqual([p.name for p in self.removed], [f'Game.app.replaced-{os.getpid()}'])
        self.assertFalse(self.removed[0].name.endswith('.app'))   # not an app to Launch Services while it is deleted

    def test_other_data_checks_the_seal_once_and_replaces(self):
        data_bundle(self.out, seal=b'another seal'); data_bundle(self.new)
        self.assertFalse(self.install())
        self.assertEqual(len(self.checked), 1)
        self.assertEqual((self.out/'Contents/Helpers/GameData-v143.bundle/Contents/_CodeSignature/CodeResources').read_bytes(), b'seal')

    def test_first_install(self):
        data_bundle(self.new)
        self.assertFalse(self.install())
        self.assertEqual((len(self.checked), self.removed), (1, []))
        self.assertTrue(self.out.is_dir())

    def test_a_seal_that_does_not_verify_leaves_the_installed_app(self):
        old_image = data_bundle(self.out, seal=b'another seal')/IMAGE
        data_bundle(self.new)
        with self.assertRaises(build_app.BuildError):
            self.install(result=False)
        self.assertTrue(old_image.is_file())
        self.assertEqual(self.removed, [])

    def test_the_installed_app_folder_stays_and_takes_the_new_contents(self):
        # macOS keeps its first-run check of an app (Gatekeeper / XProtect, 185-200 s for the 17 GB bundle) with the .app
        # folder: a new folder with the same contents is scanned again, new Contents in the same folder are not
        # (2026-10-22). So the folder stays and only Contents is replaced.
        data_bundle(self.out, seal=b'another seal'); data_bundle(self.new)
        (self.new/'Contents/Info.plist').write_bytes(b'new app')
        folder = os.stat(self.out).st_ino
        self.install()
        self.assertEqual(os.stat(self.out).st_ino, folder)
        self.assertEqual((self.out/'Contents/Info.plist').read_bytes(), b'new app')
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ['Contents'])
        old = self.removed[0]
        self.assertEqual((old/'Contents/Helpers/GameData-v143.bundle/Contents/_CodeSignature/CodeResources').read_bytes(),
                         b'another seal')   # the replaced contents, outside the app, are what is removed

    def test_failed_moves_restore_both_apps(self):
        for same_data in (True, False):
            for fail_at in range(1, 5 if same_data else 3):
                with self.subTest(same_data=same_data, fail_at=fail_at), tempfile.TemporaryDirectory() as tmp:
                    out, new = Path(tmp)/'Game.app', Path(tmp)/'Game.app.partial'
                    old_image = data_bundle(out)/IMAGE
                    new_image = data_bundle(new, seal=b'seal' if same_data else b'other')/IMAGE
                    old_id, new_id = identity(old_image), identity(new_image)
                    folder_id = out.stat().st_ino
                    rename = Path.rename
                    calls = []
                    def failing_move(source, target):
                        calls.append((source, target))
                        if len(calls) == fail_at:
                            raise OSError(errno.EIO, 'injected move failure')
                        return rename(source, target)
                    removed = []
                    with mock.patch.object(Path, 'rename', failing_move):
                        try:
                            build_app.install_app(new, out, verify=lambda _: True, remove=removed.append)
                        except OSError:
                            self.assertEqual(identity(old_image), old_id)
                            self.assertEqual(identity(new_image), new_id)
                            self.assertEqual(out.stat().st_ino, folder_id)
                            self.assertEqual(removed, [])
                        else:
                            self.assertTrue(same_data and fail_at == 2, 'only reuse failure may fall back')
                            # Failure to reuse a data bundle may fall back to the fresh copy.
                            self.assertTrue((out/'Contents/Helpers/GameData-v143.bundle'/IMAGE).is_file())

    def test_verifier_exception_restores_reused_data(self):
        old_image = data_bundle(self.out)/IMAGE
        new_image = data_bundle(self.new)/IMAGE
        old_id, new_id = identity(old_image), identity(new_image)
        with self.assertRaisesRegex(OSError, 'verification failed'):
            build_app.install_app(self.new, self.out,
                                  verify=mock.Mock(side_effect=OSError('verification failed')),
                                  remove=self.removed.append)
        self.assertEqual(identity(old_image), old_id)
        self.assertEqual(identity(new_image), new_id)
        self.assertEqual(self.removed, [])

    def test_failure_after_contents_promotion_restores_both_apps(self):
        old_image = data_bundle(self.out)/IMAGE
        new_image = data_bundle(self.new)/IMAGE
        old_id, new_id = identity(old_image), identity(new_image)
        rmdir = Path.rmdir
        def fail(path):
            if path == self.new:
                raise OSError('cannot remove staging directory')
            return rmdir(path)
        with mock.patch.object(Path, 'rmdir', fail), self.assertRaises(OSError):
            self.install()
        self.assertEqual(identity(old_image), old_id)
        self.assertEqual(identity(new_image), new_id)
        self.assertEqual(self.removed, [])
        self.assertTrue(self.install(), 'a restored update can be retried')

    def test_verification_failure_restores_every_bundle(self):
        # one check of the seal once every bundle is moved (keep_game_data); its failure puts all of them back
        originals = []
        for name in ('GameData-v121.bundle', 'GameData-v143.bundle'):
            for app in (self.out, self.new):
                path = data_bundle(app, name=name)/IMAGE
                originals.append((path, identity(path)))
        verify = mock.Mock(side_effect=OSError('verification failed'))
        with self.assertRaises(OSError):
            build_app.install_app(self.new, self.out, verify=verify, remove=self.removed.append)
        self.assertEqual(verify.call_count, 1)
        for path, before in originals:
            self.assertEqual(identity(path), before)
        self.assertEqual(self.removed, [])

    def test_failure_after_one_new_entry_restores_all_entries(self):
        data_bundle(self.out); data_bundle(self.new)
        (self.out/'old-note').write_text('old')
        (self.new/'new-note').write_text('new')
        def snapshot(root):
            return {str(p.relative_to(root)): (p.read_bytes(), p.stat().st_ino)
                    for p in root.rglob('*') if p.is_file()}
        old_files, new_files = snapshot(self.out), snapshot(self.new)
        rename, promoted = Path.rename, []
        def fail(source, target):
            if source.parent == self.new:
                if promoted:
                    raise OSError('second entry failed')
                promoted.append(source)
            return rename(source, target)
        with mock.patch.object(Path, 'rename', fail), self.assertRaises(OSError):
            self.install()
        self.assertEqual(snapshot(self.out), old_files)
        self.assertEqual(snapshot(self.new), new_files)
        self.assertEqual(self.removed, [])

    def test_failed_first_install_keeps_staged_app(self):
        image = data_bundle(self.new)/IMAGE
        before = identity(image)
        with mock.patch.object(Path, 'rename', side_effect=OSError('cannot publish')), self.assertRaises(OSError):
            self.install()
        self.assertFalse(self.out.exists())
        self.assertEqual(identity(image), before)
        self.assertEqual(self.removed, [])

    def test_rollback_failure_preserves_files_and_reports_recovery_path(self):
        old_image = data_bundle(self.out)/IMAGE
        data_bundle(self.new)
        old_id = identity(old_image)
        rename = Path.rename
        def fail(source, target):
            if source == self.new/'Contents' or source.name == 'Contents' and '.replaced-' in source.parent.name:
                raise OSError('injected persistent I/O failure')
            return rename(source, target)
        with mock.patch.object(Path, 'rename', fail), self.assertRaisesRegex(build_app.BuildError, 'recover.*before rebuilding'):
            self.install()
        self.assertEqual(identity(self.new/'Contents/Helpers/GameData-v143.bundle'/IMAGE), old_id)
        self.assertTrue(list(self.root.glob('*.fresh')))
        self.assertTrue(list(self.root.glob('*.replaced-*')))
        self.assertEqual(self.removed, [])

    def test_fresh_copy_cleanup_failure_does_not_undo_a_complete_install(self):
        old_image = data_bundle(self.out)/IMAGE
        data_bundle(self.new)
        before = identity(old_image)
        with mock.patch.object(build_app, 'remove_tree', side_effect=OSError('cleanup failed')), \
                mock.patch.object(build_app, 'say') as warning:
            self.assertTrue(self.install())
        self.assertEqual(identity(old_image), before)
        self.assertTrue(list(self.root.glob('*.fresh')))
        self.assertIn('cleanup failed', warning.call_args[0][0])

    def test_leftovers_of_an_earlier_install_are_removed_too(self):
        (self.root/'Game.app.replaced-1').mkdir()
        data_bundle(self.new)
        self.install()
        self.assertEqual([p.name for p in self.removed], ['Game.app.replaced-1'])


if __name__ == '__main__':
    unittest.main()

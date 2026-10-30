"""Work-folder isolation, including source/cache directory symlinks."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import build_app


class WorkDirectoryTests(unittest.TestCase):
    def fixture(self, root):
        source = root/'source'
        source.mkdir()
        (source/'requirements.txt').write_text('')
        external = root/'shared-tooling'
        (external/'spirv-cross').mkdir(parents=True)
        (external/'spirv-cross/keep.txt').write_text('shared dependency must survive')
        return source, external, root/'work'

    def test_existing_tooling_link_is_detached_before_child_links_change(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            source, external, work=self.fixture(root)
            (work/'totk').mkdir(parents=True)
            (work/'totk/tooling').symlink_to(external,target_is_directory=True)
            with mock.patch.object(build_app,'GAME',source), \
                 mock.patch.object(build_app,'run'), \
                 mock.patch.object(build_app,'git_fetch'):
                tree=build_app.prepare_work(work,Path('/usr/bin/python3'),work/'logs')
            self.assertFalse((tree/'tooling').is_symlink())
            self.assertTrue((tree/'tooling/spirv-cross').is_symlink())
            self.assertEqual((external/'spirv-cross/keep.txt').read_text(),'shared dependency must survive')

    @unittest.skipUnless(shutil.which('rsync'),'rsync needed for real source-copy check')
    def test_source_cache_symlinks_are_excluded_by_the_real_copier(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            source, external, work=self.fixture(root)
            (source/'tooling').symlink_to(external,target_is_directory=True)
            (source/'extracted').symlink_to(external,target_is_directory=True)
            def run(command,*_args,**_kwargs):
                if str(command[0])=='rsync':
                    subprocess.run([str(x) for x in command],check=True,capture_output=True)
            with mock.patch.object(build_app,'GAME',source), \
                 mock.patch.object(build_app,'run',side_effect=run), \
                 mock.patch.object(build_app,'git_fetch'):
                tree=build_app.prepare_work(work,Path('/usr/bin/python3'),work/'logs')
            self.assertFalse((tree/'extracted').exists())
            self.assertFalse((tree/'extracted').is_symlink())
            self.assertFalse((tree/'tooling').is_symlink())
            self.assertEqual((external/'spirv-cross/keep.txt').read_text(),'shared dependency must survive')

    @unittest.skipUnless(shutil.which('rsync'),'rsync needed for real source-copy check')
    def test_changed_source_older_than_the_last_build_is_rebuilt(self):
        # 2026-10-25: a branch checked out before the last build in this folder kept that build's object for a changed file
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            source, external, work=self.fixture(root)
            def run(command,*_args,**_kwargs):
                if str(command[0])=='rsync':
                    subprocess.run([str(x) for x in command],check=True,capture_output=True)
            with mock.patch.object(build_app,'GAME',source), \
                 mock.patch.object(build_app,'run',side_effect=run), \
                 mock.patch.object(build_app,'git_fetch'):
                (source/'a.c').write_text('branch one'); (source/'same.c').write_text('unchanged')
                tree=build_app.prepare_work(work,Path('/usr/bin/python3'),work/'logs')
                obj=tree/'a.c.o'; obj.write_text('object'); same_before=(tree/'same.c').stat().st_mtime
                (source/'a.c').write_text('branch two')
                old=obj.stat().st_mtime-100
                for name in ('a.c','same.c'): os.utime(source/name,(old,old))   # the other branch's checkout is older than the object
                build_app.prepare_work(work,Path('/usr/bin/python3'),work/'logs')
            self.assertEqual((tree/'a.c').read_text(),'branch two')
            self.assertGreaterEqual((tree/'a.c').stat().st_mtime,obj.stat().st_mtime)   # make sees the source as newer than its object
            self.assertEqual((tree/'same.c').stat().st_mtime,same_before)               # an unchanged file keeps its time (no needless rebuild)

    def test_git_fetch_can_replace_a_dangling_cache_link(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            missing=root/'external-missing'
            cache=root/'cache'
            cache.symlink_to(missing,target_is_directory=True)
            with mock.patch.object(build_app,'run') as run:
                build_app.git_fetch('https://example.invalid/source','a'*40,cache,root/'fetch.log')
            self.assertTrue(cache.is_dir())
            self.assertFalse(cache.is_symlink())
            self.assertFalse(missing.exists())
            self.assertEqual(run.call_count,3)


if __name__=='__main__':
    unittest.main()

"""Ocarina of Time's work-folder copy of games/oot mirrors the checkout (build.py sync_tree): a source file the checkout no
longer has leaves the copy, the build's own folders stay, and linked folders (the shared graphics layer, zeldaVR) are
copied as their contents. 2026-11-13: a copy that only added files kept a branch's nvn_pool_sources.m, which the
Makefile's wildcard compiled with another checkout's headers."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'
spec = importlib.util.spec_from_file_location('oot_build_tree', GAME / 'build.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class SyncTree(unittest.TestCase):
    def test_mirrors_the_checkout_and_keeps_the_build_output(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); source = root / 'game'; tree = root / 'work/oot'; shared = root / 'shared'
            (source / 'gfx').mkdir(parents=True); (shared / 'nvn').mkdir(parents=True)
            (source / 'gfx/kept.m').write_text('both'); (source / 'gfx/branch_only.m').write_text('one branch')
            (source / 'scripts').mkdir(); (source / 'scripts/run.py').write_text('print()')
            (shared / 'nvn/layer.c').write_text('shared layer')
            (source / 'third_party').mkdir(); (source / 'third_party/d2r_gfx').symlink_to(shared, target_is_directory=True)
            build.sync_tree(source, tree)
            self.assertEqual((tree / 'third_party/d2r_gfx/nvn/layer.c').read_text(), 'shared layer')
            self.assertFalse((tree / 'third_party/d2r_gfx').is_symlink())   # its contents, not a link out of the work folder
            (tree / 'build/obj').mkdir(parents=True); (tree / 'build/obj/kept.m.o').write_text('object')
            (tree / 'extracted').mkdir(); (tree / 'extracted/romfs.bin').write_text('unpacked game data')
            (tree / 'scripts/__pycache__').mkdir(parents=True); (tree / 'scripts/__pycache__/x.pyc').write_text('cache')
            (source / 'gfx/branch_only.m').unlink(); (shared / 'nvn/gone.h').write_text('new'); (shared / 'nvn/gone.h').unlink()
            build.sync_tree(source, tree)
            self.assertFalse((tree / 'gfx/branch_only.m').exists())
            self.assertEqual((tree / 'gfx/kept.m').read_text(), 'both')
            self.assertEqual((tree / 'build/obj/kept.m.o').read_text(), 'object')
            self.assertEqual((tree / 'extracted/romfs.bin').read_text(), 'unpacked game data')
            self.assertTrue((tree / 'scripts/__pycache__/x.pyc').exists())   # ignored names are left alone, as the copy skips them

    def test_the_ios_build_output_stays(self):
        """ios/Gen is the iOS build's own folder (the device seed and its compiled-library cache, <seed>.ios-metallibs):
        2026-11-13 the mirror deleted it at every build, so all 18,585 iPad libraries were compiled again (~16 min)."""
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); source = root / 'game'; tree = root / 'work/oot'
            (source / 'ios/tools').mkdir(parents=True); (source / 'ios/tools/build_ipa.py').write_text('print()')
            build.sync_tree(source, tree)
            cache = tree / 'ios/Gen/totk/shader-seed.ios-metallibs'; cache.mkdir(parents=True)
            (cache / 'key.metallib').write_text('compiled library')
            build.sync_tree(source, tree)
            self.assertEqual((cache / 'key.metallib').read_text(), 'compiled library')

    def test_an_unchanged_file_keeps_its_time(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); source = root / 'game'; tree = root / 'work/oot'
            source.mkdir(); (source / 'a.c').write_text('same')
            build.sync_tree(source, tree)
            before = (tree / 'a.c').stat().st_mtime_ns
            build.sync_tree(source, tree)
            self.assertEqual((tree / 'a.c').stat().st_mtime_ns, before)


if __name__ == '__main__':
    unittest.main()

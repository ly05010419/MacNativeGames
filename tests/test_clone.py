"""build_app.clone / copy_tree: APFS clones made in one call (clonefile), never one program per file."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import build_app


class Clone(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.src = self.root / 'src'
        (self.src / 'sub').mkdir(parents=True)
        (self.src / 'a.metal').write_text('a')
        (self.src / 'sub/b.bin').write_bytes(b'b')
        (self.src / 'a.metal').chmod(0o640)

    def files(self, folder):
        return {str(p.relative_to(folder)): p.read_bytes() for p in folder.rglob('*') if p.is_file()}

    def test_folder_is_cloned_without_starting_programs(self):
        with patch.object(build_app.subprocess, 'run', side_effect=AssertionError('no program per file')):
            build_app.copy_tree(self.src, self.root / 'out/tree')
        self.assertEqual(self.files(self.root / 'out/tree'), self.files(self.src))
        self.assertEqual((self.root / 'out/tree/a.metal').stat().st_mode & 0o777, 0o640)

    def test_existing_folder_gets_the_files_one_by_one(self):
        out = self.root / 'out'; out.mkdir()
        (out / 'keep.txt').write_text('kept')
        build_app.copy_tree(self.src, out)
        self.assertEqual(self.files(out), {**self.files(self.src), 'keep.txt': b'kept'})

    def test_existing_file_is_replaced(self):
        dst = self.root / 'out/a.metal'; dst.parent.mkdir(); dst.write_text('old')
        build_app.clone(self.src / 'a.metal', dst)
        self.assertEqual(dst.read_text(), 'a')

    def test_copies_when_cloning_is_not_possible(self):
        with patch.object(build_app, 'clonefile', return_value=False):
            build_app.copy_tree(self.src, self.root / 'out')
            build_app.clone(self.src / 'a.metal', self.root / 'single/a.metal')
        self.assertEqual(self.files(self.root / 'out'), self.files(self.src))
        self.assertEqual((self.root / 'single/a.metal').read_text(), 'a')


if __name__ == '__main__':
    unittest.main()

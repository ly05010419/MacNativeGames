"""Work folders with spaces (issue #2284): MNG Launcher puts a port's work folder next to the game, named after it
("Super Mario Odyssey/build/odyssey"), and make splits target and prerequisite names at spaces. Every port's
`make ... WORK=` goes through the one tools/port_app/make_path.py, which hands such a folder over as a link without a
space in the temporary folder; the build's files still land in the work folder."""
import os
import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
import sys
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from tools.port_app import make_path as mp  # noqa: E402


class MakePath(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__('shutil').rmtree(self.root, ignore_errors=True))
        self.tmp = self.root / 'tmp'
        self.tmp.mkdir()
        patcher = mock.patch.object(mp.tempfile, 'gettempdir', return_value=str(self.tmp))
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_a_path_without_a_space_is_handed_over_as_it_is(self):
        work = self.root / 'work'
        self.assertEqual(mp.make_path(work, 'game'), work)
        self.assertEqual(list(self.tmp.iterdir()), [])

    def test_a_path_with_a_space_becomes_a_link_without_one_to_the_same_folder(self):
        work = self.root / 'Super Mario Odyssey' / 'build' / 'odyssey'
        work.mkdir(parents=True)
        alias = mp.make_path(work, 'odyssey')
        self.assertNotIn(' ', str(alias))
        self.assertTrue(alias.is_symlink())
        self.assertEqual(alias.resolve(), work.resolve())
        self.assertTrue(alias.name.startswith('odyssey-work-'))
        (alias / 'built.o').write_text('x')
        self.assertTrue((work / 'built.o').is_file(), 'files made through the link land in the work folder')
        self.assertEqual(mp.make_path(work, 'odyssey'), alias, 'the same folder gets the same link again')

    def test_two_work_folders_get_two_links(self):
        a, b = self.root / 'a game' / 'w', self.root / 'b game' / 'w'
        a.mkdir(parents=True); b.mkdir(parents=True)
        self.assertNotEqual(mp.make_path(a, 'g'), mp.make_path(b, 'g'))

    def test_a_link_left_pointing_elsewhere_is_replaced(self):
        work = self.root / 'a game' / 'w'
        work.mkdir(parents=True)
        alias = mp.make_path(work, 'g')
        alias.unlink()
        alias.symlink_to(self.root, target_is_directory=True)
        self.assertEqual(mp.make_path(work, 'g').resolve(), work.resolve())

    def test_a_real_folder_in_the_way_is_left_alone_and_named(self):
        work = self.root / 'a game' / 'w'
        work.mkdir(parents=True)
        alias = mp.make_path(work, 'g')
        alias.unlink()
        alias.mkdir()
        (alias / 'keep').write_text('not ours')
        with self.assertRaisesRegex(ValueError, 'in the way'):
            mp.make_path(work, 'g')
        self.assertTrue((alias / 'keep').is_file())

    def test_a_temporary_folder_with_a_space_says_so(self):
        work = self.root / 'a game' / 'w'
        work.mkdir(parents=True)
        spaced = self.root / 'my tmp'
        spaced.mkdir()
        with mock.patch.object(mp.tempfile, 'gettempdir', return_value=str(spaced)):
            with self.assertRaisesRegex(ValueError, 'TMPDIR'):
                mp.make_path(work, 'g')

    def test_a_link_another_build_made_meanwhile_is_taken(self):
        work = self.root / 'a game' / 'w'
        work.mkdir(parents=True)
        real_symlink_to = Path.symlink_to

        def racing(self_, target, target_is_directory=False):
            real_symlink_to(self_, target, target_is_directory=target_is_directory)   # the other build was first
            raise FileExistsError(str(self_))
        with mock.patch.object(Path, 'symlink_to', racing):
            alias = mp.make_path(work, 'g')
        self.assertEqual(alias.resolve(), work.resolve())


class EveryMakeCall(unittest.TestCase):
    """Every Python file that passes WORK= to make hands it over through tools/port_app/make_path.py, and carries no
    copy of its own (games/oot and games/ssbu had one each, the issue's patch put eleven more)."""
    WORK = re.compile(r"""(?:f['"]WORK=\{([^}]+)\}|'WORK='\s*\+\s*str\(([^)]*\)?)\))""")

    def files(self):
        for path in sorted((ROOT / 'games').rglob('*.py')):
            if '/tests/' in str(path) or path.name.startswith('test_'):
                continue
            text = path.read_text(errors='replace')
            if self.WORK.search(text):
                yield path, text

    def test_the_work_folder_reaches_make_through_make_path(self):
        found = []
        for path, text in self.files():
            found.append(path.relative_to(ROOT).as_posix())
            self.assertIn('from tools.port_app.make_path import make_path', text, path)
            self.assertNotRegex(text, r'\ndef make_(path|work)\(', f'{path}: a copy of its own')
            for m in self.WORK.finditer(text):
                value = (m.group(1) or m.group(2)).strip()
                if 'make_path(' in value:
                    continue
                name = re.escape(value.split('/')[0].strip())
                self.assertRegex(text, rf'\b{name}\s*=[^\n]*make_path\(', f'{path}: WORK={value} does not come from make_path')
        for expected in ('games/acnh/build_host.py', 'games/dread/build.py', 'games/dread/scripts/package_dread.py',
                         'games/eow/build.py', 'games/kirby/scripts/build_kirby.py', 'games/odyssey/build.py',
                         'games/odyssey/scripts/build_odyssey.py', 'games/oot/build.py', 'games/ot0/scripts/build_ot0.py',
                         'games/ssbu/scripts/build_ssbu.py', 'games/tmnt/scripts/build_private_bundle.py',
                         'games/tmnt/scripts/tmnt_update.py', 'games/tomodachi/build_host.py'):
            self.assertIn(expected, found)

    def test_every_makefile_quotes_the_python_it_runs(self):
        # PY= is often <work folder>/venv/bin/python, a path with the work folder's spaces (games/tmnt ran it bare)
        for makefile in sorted((ROOT / 'games').glob('*/Makefile')):
            for number, line in enumerate(makefile.read_text(errors='replace').splitlines(), 1):
                if line.startswith('\t') and re.search(r'(^|[^"])\$\(PY\)([^"]|$)', line):
                    self.fail(f'{makefile.relative_to(ROOT)}:{number}: run "$(PY)" quoted: {line.strip()[:80]}')


if __name__ == '__main__':
    unittest.main()

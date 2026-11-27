"""What MNG Launcher carries of this repository (tools/launcher_source.py; user 2026-11-22: "123 做"): its games' folders
and the code its jobs run. Nothing it carries may read what it leaves out, or a player's build would fail where a
checkout's works."""
import ast
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from tools import launcher_source  # noqa: E402
from tools.builder_jobs import repo  # noqa: E402

# A kept file that names a left-out folder in code (not a comment), and why a Launcher never runs that line
NAMED_BUT_NOT_RUN = {
    'update_runtime.py': 'games/ss/scripts: the update of a Skyward Sword HD app, which the Launcher no longer lists',
    'tools/builder_jobs/repo.py': 'NOT_IN_LAUNCHER names the folders of the games the Launcher does not build',
    'tools/launcher_source.py': 'the list itself',
}


def carried():
    """The files git archive takes with launcher_source.pathspecs()."""
    out = subprocess.run(['git', '-C', str(REPO), 'ls-files', '-z', '--', *launcher_source.pathspecs()],
                         capture_output=True, check=True).stdout
    return [name for name in out.decode().split('\0') if name]


class LauncherSource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = carried()
        cls.left_out = launcher_source.left_out_games()

    def test_every_launcher_game_has_its_folders(self):
        self.assertEqual(set(repo.GAME_FOLDERS), set(repo.GAMES))
        for game, folders in repo.GAME_FOLDERS.items():
            for folder in folders:
                self.assertTrue(any(f.startswith(f'games/{folder}/') for f in self.files), (game, folder))
        for name in ('build_app.py', 'update_runtime.py', 'shader_update.py', 'tools/builder_jobs/__main__.py',
                     'tools/deps/__init__.py', 'tools/prebuilt.py'):
            self.assertIn(name, self.files)

    def test_other_games_ios_parts_and_tests_are_left_out(self):
        for name in self.files:
            self.assertFalse(re.match(r'(tests|windows_games)/', name), name)
            self.assertFalse(re.match(r'games/[^/]+/(ios|prebuilt/ios-arm64)/', name), name)
            self.assertNotIn(name.split('/')[1] if name.startswith('games/') else None, self.left_out, name)
        self.assertIn('wonder', self.left_out)
        self.assertNotIn('totk', self.left_out)

    def test_oot_shader_packaging_from_carried_source(self):
        # Use only files selected by the release pathspecs, in a fresh Python process.
        sources = {'games/oot/build.py', 'games/oot/scripts/romfs_pin.py', 'games/oot/scripts/seed_image.py',
                   'tools/port_app/__init__.py', 'tools/port_app/make_path.py'}
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in sources.intersection(self.files):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(REPO / name, target)
            seed, libraries, output = (root / name for name in ('seed', 'libraries', 'resources'))
            seed.mkdir()
            libraries.mkdir()
            (seed / ('a' * 64 + '.variants')).write_bytes(b'[]')
            library = 'b' * 64 + '.metallib'
            (libraries / library).write_bytes(b'MTLB')
            (libraries / 'libraries-manifest.json').write_text(json.dumps({'files': [library]}))
            run = subprocess.run(
                [sys.executable, '-I', '-B', '-c',
                 'import runpy, sys\n'
                 'from pathlib import Path\n'
                 'pack = runpy.run_path(sys.argv[1])["write_shader_images"]\n'
                 'print(pack(*map(Path, sys.argv[2:])))',
                 str(root / 'games/oot/build.py'), str(seed), str(libraries), str(output)],
                cwd=root, capture_output=True, text=True,
            )
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(run.stdout.strip(), '(1, 1)')
            for name in ('shader-seed.img.000', 'shader-libs.img.000'):
                with (output / name).open('rb') as part:
                    self.assertEqual(part.read(8), b'TOTKIMG1')

    def test_no_carried_link_points_at_what_is_left_out(self):
        listed = subprocess.run(['git', '-C', str(REPO), 'ls-files', '-s', '--', *launcher_source.pathspecs()],
                                capture_output=True, text=True, check=True).stdout.splitlines()
        kept = set(self.files)
        for line in listed:
            mode, _, _, name = line.split(None, 3)
            if mode != '120000':
                continue
            target = (Path(name).parent / (REPO / name).readlink()).as_posix()
            target = str(Path(target)).replace('\\', '/')
            parts = []
            for part in target.split('/'):
                parts = parts[:-1] if part == '..' else parts + [part]
            resolved = '/'.join(parts)
            self.assertTrue(any(f == resolved or f.startswith(resolved + '/') for f in kept), f'{name} -> {resolved}')

    def test_no_carried_code_reads_a_left_out_folder(self):
        # the top-level tests/ is named only by the root Makefile (make check, the source gate of a checkout); the
        # games' own tests/ folders are kept
        pattern = re.compile(r'games/(%s)\b|\bwindows_games/' % '|'.join(map(re.escape, self.left_out)))
        # a path joined onto games/ (HERE.parent / 'by3/third_party/d2r'): the folder's name starts the string. 2026-11-27,
        # #2413: Odyssey's prepare_runtime.py read Bowser's Fury's relr.py this way, which the pattern above did not see,
        # and every Launcher build of Odyssey stopped at step 5/8 ("No module named 'relr'")
        joined = re.compile(r'^(%s)/' % '|'.join(map(re.escape, self.left_out)))
        found = []
        for name in self.files:
            path = REPO / name
            if path.is_symlink() or not path.is_file() or name in NAMED_BUT_NOT_RUN:
                continue
            if path.suffix == '.py':
                try:
                    tree = ast.parse(path.read_text())
                except (SyntaxError, UnicodeDecodeError):
                    continue
                for node in ast.walk(tree):
                    if isinstance(node, ast.Constant) and isinstance(node.value, str) and (pattern.search(node.value) or joined.match(node.value)):
                        if node.value.strip().startswith(('"""', "'''")) or len(node.value) > 200:   # a docstring
                            continue
                        found.append(f'{name}:{node.lineno} {node.value[:80]!r}')
            elif path.name == 'Makefile' or path.suffix == '.sh':
                for i, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
                    if pattern.search(line) and not line.lstrip().startswith('#'):
                        found.append(f'{name}:{i} {line.strip()[:80]}')
        self.maxDiff = None
        self.assertEqual(found, [], 'carried code reads what the Launcher leaves out (tools/launcher_source.py)')


if __name__ == '__main__':
    unittest.main()

"""tools/builder/mod_applied.py (S7.3 N2, R2) and the Mods tab's quick and full reads (R1, mod_shared), through
controller_mods against a fake Breath of the Wild 1.9.0 app and main's real install. HOME is inside the test folder,
as in test_builder_mod_library."""
import ast
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock

from test_mod_manifest import fake_romfs
from tools.builder import controller_mods, mod_add, mod_applied, mod_library, mod_tools
from tools.builder.games import BOTW
from tools.builder.ports import CommandRunner, RealClock

REPO = Path(__file__).resolve().parents[1]
FILES = {'Pack/TitleBG.pack': b'title' * 40, 'Pack/Bootup.pack': b'boot' * 40}


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp).resolve()
        home = mock.patch.dict(os.environ, HOME=str(self.tmp / 'home'))
        home.start()
        self.addCleanup(home.stop)
        self.data = self.tmp / 'home/Library/Application Support/MacNativeGames'
        self.app = self.tmp / 'Breath of the Wild.app'
        fake_romfs(self.app / 'Contents/Resources/totk/extracted/botw190/romfs', FILES)
        self.library = self.reopen()
        self.assertTrue(self.library.load.is_relative_to(self.tmp))

    def reopen(self):
        return controller_mods._library(REPO, BOTW, 'botw190', self.data)

    def state(self, full=False):
        return controller_mods.read_state(REPO, BOTW, self.app, self.data, full=full)

    def player_mod(self, name, files):
        root = self.tmp / 'mods' / name / 'romfs'
        for path, data in files.items():
            (root / path).parent.mkdir(parents=True, exist_ok=True)
            (root / path).write_bytes(data)
        [mod_id] = mod_add.add(CommandRunner(RealClock()), REPO, self.app, 'botw190', self.library, root.parent)
        mod_library.set_enabled(self.library, mod_id, True)
        return mod_id


class Applied(Base):
    def test_a_change_after_apply_says_not_applied_until_apply(self):
        self.assertFalse(self.state().not_applied)                      # nothing ever applied: not flagged
        mod_id = self.player_mod('A', {'Pack/TitleBG.pack': b'a' * 100})
        self.assertTrue(self.state().not_applied)
        self.assertTrue(controller_mods.apply(REPO, BOTW, self.app, self.data).ok)
        self.assertFalse(self.state().not_applied)
        controller_mods.change(REPO, BOTW, self.app, self.data, 'enable', mod_id, False)
        self.assertTrue(self.state().not_applied)

    def test_a_failed_install_says_not_applied(self):
        self.player_mod('A', {'Pack/TitleBG.pack': b'a' * 100})
        _, folder_tool = mod_tools.tools(REPO)
        with mock.patch.object(folder_tool, 'install', side_effect=folder_tool.InstallError('broken')):
            r = controller_mods.apply(REPO, BOTW, self.app, self.data)
        self.assertEqual((r.ok, r.reason, self.reopen().state['applied_failed']), (False, 'install_failed', True))
        self.assertTrue(self.state().not_applied)
        self.assertTrue(controller_mods.apply(REPO, BOTW, self.app, self.data).ok)
        self.assertFalse(self.state().not_applied)

    def test_an_exception_keeps_the_original_and_marks_failed(self):
        error = OSError('copy')
        record = lambda: mod_applied.apply_and_record(REPO, BOTW, self.app, 'botw190', self.library, self.data)
        with mock.patch.object(mod_library, 'apply', side_effect=error):
            with self.assertRaises(OSError) as caught:
                record()
        self.assertIs(caught.exception, error)
        self.assertTrue(self.reopen().state['applied_failed'])
        with mock.patch.object(mod_library, 'apply', side_effect=error), \
                mock.patch.object(mod_library, 'save', side_effect=PermissionError('save')):
            with self.assertRaises(OSError) as caught:
                record()
        self.assertIs(caught.exception, error)
        self.assertIsInstance(caught.exception.__cause__, PermissionError)

    def test_the_extra_state_is_part_of_the_key(self):
        self.player_mod('A', {'Pack/TitleBG.pack': b'a' * 100})
        mod_applied.apply_and_record(REPO, BOTW, self.app, 'botw190', self.library, self.data, ({'x': 1},))
        self.assertEqual([mod_applied.not_applied(self.library, e) for e in (({'x': 1},), ({'x': 2},), ())],
                         [False, True, True])

    def test_the_keys_round_trip(self):
        self.library.state.update(applied='a', applied_failed=True)
        mod_library.save(self.library)
        self.assertEqual((self.reopen().state['applied'], self.reopen().state['applied_failed']), ('a', True))


class Rules(Base):
    def test_every_apply_goes_through_the_wrapper(self):                # R2, by the AST
        calls = []
        for path in (REPO / 'tools/builder').glob('*.py'):
            tree = ast.parse(path.read_text())
            for fn in ast.walk(tree):
                if isinstance(fn, ast.FunctionDef):
                    for node in ast.walk(fn):
                        target = node.func if isinstance(node, ast.Call) else None
                        if (isinstance(target, ast.Attribute) and target.attr == 'apply'
                                and isinstance(target.value, ast.Name) and target.value.id == 'mod_library'):
                            calls.append((path.name, fn.name))
            for node in ast.walk(tree):
                if isinstance(node, ast.ImportFrom) and node.module == 'tools.builder.mod_library':
                    self.assertNotIn('apply', [a.name for a in node.names], path.name)
        self.assertEqual(sorted(set(calls)), [('mod_applied.py', 'apply_and_record')])

    def test_a_quick_read_opens_no_mod_file(self):                      # R1: stat only
        self.player_mod('A', {'Pack/TitleBG.pack': b'a' * 100})
        mods = str(self.library.root)
        opened = []
        real_open = open
        def spy(file, *args, **kwargs):
            if str(file).startswith(mods) and '/romfs/' in str(file):
                opened.append(file)
            return real_open(file, *args, **kwargs)
        with mock.patch('builtins.open', side_effect=spy), mock.patch('io.open', side_effect=spy):
            self.state()
        self.assertEqual(opened, [])

    def test_the_full_read_finds_identical_files_and_the_quick_one_keeps_it(self):    # mod_shared
        for name in ('A', 'Copy of A'):
            self.player_mod(name, {'Pack/TitleBG.pack': b'same bytes'})
        listed = lambda full: [m.conflicts for m in self.state(full).mods]
        self.assertEqual(listed(False), [('Pack/TitleBG.pack',)] * 2)
        self.assertEqual((listed(True), listed(False)), ([(), ()], [(), ()]))


if __name__ == '__main__':
    unittest.main()

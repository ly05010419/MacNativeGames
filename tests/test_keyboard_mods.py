"""The launcher's keyboard icons mod (games/totk/runtime/keyboard_mods.h): a staged one taken at start, one older than
the game's mods passed over."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

PROBE = r'''
#include "keyboard_mods.h"
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 3) return 2;
        NSString *mods = @(argv[2]);
        if (!strcmp(argv[1], "promote")) keyboard_mods_promote(mods);
        else printf("usable %d\n", (int)keyboard_mods_usable(mods));
    }
    return 0;
}
'''


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class KeyboardMods(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        source, cls.probe = Path(cls.build.name) / 'probe.m', Path(cls.build.name) / 'probe'
        source.write_text(PROBE)
        made = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', f'-I{ROOT}/games/totk/runtime',
                               str(source), '-framework', 'Foundation', '-o', str(cls.probe)], capture_output=True, text=True)
        if made.returncode:
            raise AssertionError(made.stderr[-3000:])

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.mods = Path(self.tmp.name) / 'v143'
        self.mods.mkdir()
        (self.mods / 'manifest.tsv').write_text('game mods\n')

    def tearDown(self):
        self.tmp.cleanup()

    def folder(self, suffix):
        return Path(f'{self.mods}{suffix}')

    def make(self, suffix, manifest='icons\n', made_from=None, complete=False):
        folder = self.folder(suffix); folder.mkdir()
        (folder / 'manifest.tsv').write_text(manifest)
        if made_from is not None:
            (folder / 'made-from.txt').write_text(made_from + '\n')
        if complete:
            (folder / 'complete').write_text('')
        return folder

    def probe_run(self, command):
        return subprocess.run([str(self.probe), command, str(self.mods)], capture_output=True, text=True, timeout=30,
                              check=True).stdout.strip()

    def source(self):
        return hashlib.sha256((self.mods / 'manifest.tsv').read_bytes()).hexdigest()

    def test_promote_moves_complete_next(self):
        self.make('-keyboard', 'old\n')
        self.make('-keyboard.next', 'new\n', self.source(), complete=True)
        self.probe_run('promote')
        self.assertEqual((self.folder('-keyboard') / 'manifest.tsv').read_text(), 'new\n')
        self.assertFalse((self.folder('-keyboard') / 'complete').exists())
        self.assertFalse(self.folder('-keyboard.next').exists())
        self.assertEqual((self.folder('-keyboard.previous') / 'manifest.tsv').read_text(), 'old\n')

    def test_promote_without_a_keyboard_folder(self):
        self.make('-keyboard.next', 'new\n', self.source(), complete=True)
        self.probe_run('promote')
        self.assertEqual((self.folder('-keyboard') / 'manifest.tsv').read_text(), 'new\n')
        self.assertFalse(self.folder('-keyboard.previous').exists())

    def test_promote_ignores_next_without_marker(self):
        self.make('-keyboard', 'old\n')
        self.make('-keyboard.next', 'half\n')
        self.probe_run('promote')
        self.assertEqual((self.folder('-keyboard') / 'manifest.tsv').read_text(), 'old\n')
        self.assertTrue(self.folder('-keyboard.next').exists())

    def test_promote_keeps_one_previous(self):
        self.make('-keyboard.previous', 'oldest\n')
        self.make('-keyboard', 'old\n')
        self.make('-keyboard.next', 'new\n', self.source(), complete=True)
        self.probe_run('promote')
        self.assertEqual((self.folder('-keyboard.previous') / 'manifest.tsv').read_text(), 'old\n')

    def test_usable_without_made_from_keeps_old_behaviour(self):
        self.make('-keyboard')
        self.assertEqual(self.probe_run('usable'), 'usable 1')

    def test_unusable_without_manifest(self):
        self.folder('-keyboard').mkdir()
        self.assertEqual(self.probe_run('usable'), 'usable 0')

    def test_usable_when_made_from_matches(self):
        self.make('-keyboard', made_from=self.source())
        self.assertEqual(self.probe_run('usable'), 'usable 1')

    def test_unusable_when_source_manifest_changed(self):
        self.make('-keyboard', made_from=self.source())
        (self.mods / 'manifest.tsv').write_text('a mod added\n')
        self.assertEqual(self.probe_run('usable'), 'usable 0')

    def test_unusable_when_source_appears_after_dash(self):
        shutil.rmtree(self.mods)
        self.make('-keyboard', made_from='-')
        self.assertEqual(self.probe_run('usable'), 'usable 1')
        self.mods.mkdir(); (self.mods / 'manifest.tsv').write_text('new mods\n')
        self.assertEqual(self.probe_run('usable'), 'usable 0')



class TheLauncherAsks(unittest.TestCase):
    def test_for_a_controller_only_when_there_are_keyboard_icons(self):
        # controller_connected() waits about 0.5 s with none connected: every start paid it once the check came first
        launcher = (ROOT/'games/totk/app/launcher.m').read_text()
        self.assertIn('keyboard_mods_present(mods) && !controller_connected() && keyboard_mods_usable(mods)', launcher)


if __name__ == '__main__':
    unittest.main()

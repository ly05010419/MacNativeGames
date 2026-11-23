"""MNG Launcher's "开启省内存模式" switch (user 2026-11-16: "launcher 里 增加一个 CheckBox 叫 开启省内存模式 我可以随时打开和
关闭"; on by default since the night: "目标 12g"): the card writes memory-saver.txt in the game's data folder, next to
controls.txt, where the runtime reads it at the next start (nvn_mtl.m warm_resident_mode); on is the default (no file),
off is "off". Only Ocarina of Time has it."""
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.builder_jobs import memory_saver, tabs  # noqa: E402

# the card's switches (HomePage.swift, Strings.swift) are checked in MNG-Launcher, the Launcher's own repository since
# 2026-11-20 (tests/test_porting_contract.py)


def fake_app(folder, bundle_id):
    app = Path(folder) / 'Game.app'
    (app / 'Contents').mkdir(parents=True)
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': bundle_id}))
    return app


class MemorySaverTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name) / 'home'
        self.data = self.home / 'Library/Application Support/MacNativeGames/OcarinaOfTime'

    def call(self, action, args=None, bundle='org.macnativegames.oot'):
        app = fake_app(Path(self.tmp.name) / bundle, bundle) if not (Path(self.tmp.name) / bundle).exists() else Path(self.tmp.name) / bundle / 'Game.app'
        return tabs.call('storage', action, str(app), args, home=str(self.home))

    def test_on_by_default_off_writes_the_file_the_game_reads(self):
        r = self.call('memory_saver')
        self.assertTrue(r['ok'], r)
        self.assertEqual((r['result']['available'], r['result']['on'], r['result']['default']), (True, True, True))   # no file: on
        r = self.call('set_memory_saver', {'on': False})
        self.assertTrue(r['ok'] and not r['result']['on'], r)
        self.assertEqual((self.data / 'memory-saver.txt').read_text(), 'off\n')
        self.assertEqual(Path(r['result']['file']).parent, self.data)   # next to controls.txt (games/oot/app/launcher.m)
        self.assertEqual([p.name for p in self.data.iterdir()], ['memory-saver.txt'])   # no temporary file left
        r = self.call('set_memory_saver', {'on': True})
        self.assertTrue(r['ok'] and r['result']['on'], r)
        self.assertFalse((self.data / 'memory-saver.txt').exists())   # the default: no file
        self.assertTrue(self.call('set_memory_saver', {'on': True})['ok'])   # on twice is fine

    def test_on_and_an_empty_file_read_as_on(self):
        self.data.mkdir(parents=True)
        for text in ('on\n', '', '\n'):
            (self.data / 'memory-saver.txt').write_text(text)
            self.assertTrue(self.call('memory_saver')['result']['on'], repr(text))

    def test_a_word_the_game_does_not_know_is_shown(self):
        self.data.mkdir(parents=True)
        (self.data / 'memory-saver.txt').write_text('yes\n')
        r = self.call('memory_saver')['result']
        self.assertEqual((r['on'], r['unknown_word']), (True, 'yes'))   # the game reads it as the default, on (warm_saver_word -1)

    def test_only_ocarina_of_time_has_it(self):
        r = self.call('memory_saver', bundle='org.macnativegames.totk')
        self.assertTrue(r['ok'], r)
        self.assertFalse(r['result']['available'])
        r = self.call('set_memory_saver', {'on': True}, bundle='org.macnativegames.totk')
        self.assertFalse(r['ok'])
        self.assertFalse((self.home / 'Library/Application Support/MacNativeGames/TOTK/memory-saver.txt').exists())
        self.assertFalse(self.call('set_memory_saver', {'on': 'yes'})['ok'])   # only true / false

    def test_the_diagnostics_switch_writes_its_own_file(self):   # user 2026-11-16: "诊断模式 也改成 CheckBox"
        r = self.call('diagnostics')
        self.assertTrue(r['ok'] and r['result']['available'] and not r['result']['on'], r)
        r = self.call('set_diagnostics', {'on': True})
        self.assertTrue(r['ok'] and r['result']['on'], r)
        self.assertEqual((self.data / 'diagnostics.txt').read_text(), 'on\n')
        self.assertFalse((self.data / 'memory-saver.txt').exists())   # the other switch is not touched
        self.assertTrue(self.call('set_diagnostics', {'on': False})['ok'])
        self.assertFalse((self.data / 'diagnostics.txt').exists())
        self.assertFalse(self.call('diagnostics', bundle='org.macnativegames.totk')['result']['available'])

    def test_words_match_the_runtime(self):
        self.assertEqual([memory_saver.word(w) for w in ('on\n', ' on ', '', 'off\n', 'ON')], ['on', 'on', '', 'off', None])
        # the runtime's default is the switch's (nvn_warm_resident.h)
        h = (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_warm_resident.h').read_text()
        self.assertIn('#define WARM_SAVER_DEFAULT 1', h)   # user 2026-11-16 night: "目标 12g"
        self.assertIs(memory_saver.SWITCHES['memory_saver'][3], True)
        self.assertIs(memory_saver.SWITCHES['diagnostics'][3], False)



if __name__ == '__main__':
    unittest.main()

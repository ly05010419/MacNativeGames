"""MNG Launcher's "开启省内存模式" switch (user 2026-11-16: "改成 默认 常驻， 但是 launcher 里 增加一个 CheckBox 叫 开启省内存模式
我可以随时打开和关闭"): the card writes memory-saver.txt in the game's data folder, next to controls.txt, where the runtime
reads it at the next start (nvn_mtl.m warm_resident_mode); off is no file. Only Ocarina of Time has it."""
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.builder_jobs import memory_saver, tabs  # noqa: E402

SWIFT = ROOT / 'tools/builder_app/Sources/MNGBuilder'


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

    def test_on_writes_the_file_the_game_reads_and_off_removes_it(self):
        r = self.call('memory_saver')
        self.assertTrue(r['ok'], r)
        self.assertEqual((r['result']['available'], r['result']['on']), (True, False))   # no file: off, the default
        r = self.call('set_memory_saver', {'on': True})
        self.assertTrue(r['ok'] and r['result']['on'], r)
        self.assertEqual((self.data / 'memory-saver.txt').read_text(), 'on\n')
        self.assertEqual(Path(r['result']['file']).parent, self.data)   # next to controls.txt (games/oot/app/launcher.m)
        self.assertEqual([p.name for p in self.data.iterdir()], ['memory-saver.txt'])   # no temporary file left
        r = self.call('set_memory_saver', {'on': False})
        self.assertTrue(r['ok'] and not r['result']['on'], r)
        self.assertFalse((self.data / 'memory-saver.txt').exists())
        self.assertTrue(self.call('set_memory_saver', {'on': False})['ok'])   # off twice is fine

    def test_a_word_the_game_does_not_know_is_shown(self):
        self.data.mkdir(parents=True)
        (self.data / 'memory-saver.txt').write_text('yes\n')
        r = self.call('memory_saver')['result']
        self.assertEqual((r['on'], r['unknown_word']), (False, 'yes'))   # the game reads it as off (warm_saver_word -1)

    def test_only_ocarina_of_time_has_it(self):
        r = self.call('memory_saver', bundle='org.macnativegames.totk')
        self.assertTrue(r['ok'], r)
        self.assertFalse(r['result']['available'])
        r = self.call('set_memory_saver', {'on': True}, bundle='org.macnativegames.totk')
        self.assertFalse(r['ok'])
        self.assertFalse((self.home / 'Library/Application Support/MacNativeGames/TOTK/memory-saver.txt').exists())
        self.assertFalse(self.call('set_memory_saver', {'on': 'yes'})['ok'])   # only true / false

    def test_words_match_the_runtime(self):
        self.assertEqual([memory_saver.word(w) for w in ('on\n', ' on ', '', 'off\n', 'ON')], ['on', 'on', 'off', 'off', None])

    def test_the_card_shows_the_switch(self):
        home = (SWIFT / 'HomePage.swift').read_text()
        strings = (SWIFT / 'Strings.swift').read_text()
        self.assertIn('static let memorySaver = s("Memory saver mode", "开启省内存模式")', strings)
        self.assertIn('Toggle(T.memorySaver, isOn: Binding(get: { saver.on }, set: { setSaver($0) }))', home)
        self.assertIn('"storage", "set_memory_saver", app: app.app, args: ["on": on]', home)
        self.assertIn('"storage", "memory_saver", app: app.app', home)
        self.assertIn('.task(id: app.app) { await loadSaver() }', home)
        # under the start button, not inside a page (it shows whether or not a page is open)
        body = home[home.index('Button(action: start)'):]
        self.assertLess(body.index('Toggle(T.memorySaver'), body.index('One button named after the update'))


if __name__ == '__main__':
    unittest.main()

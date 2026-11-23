"""Active players (user 2026-11-19: "统计 每日活跃用户 … 日活跃 周活跃 月活跃 流失率"; with an install id, on by default, can be
turned off in the settings). The Launcher and every game report at most once a day to mac-native-games.com/v1/ping
(MacNativeGamesWeb shader-inbox/lib/active.js counts daily / weekly / monthly installs and the churn).

Checked here: games/totk/runtime/mng_active.h on a real Mac folder (MNG_HOME): one install id for programs starting
together, the report holds the four fields and nothing else, once a day per program, nothing when turned off, nothing from
a damaged id file; every game's copy is the same file; the Launcher's Swift side uses the same files, address and fields;
each game's start calls it; the iOS settings panes have the switch, on by default."""
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'games/totk/runtime/mng_active.h'
# the Launcher's half (UsageStats.swift: the same files, address and fields) is checked in MNG-Launcher, its own
# repository since 2026-11-20 (tests/test_porting_contract.py)
# each game's start: (file, the call it makes)
STARTS = {
    'games/totk/runtime/frontend.m': 'mng_active_start(@GAME_INBOX_ID, mng_active_version());',
    'games/oot/runtime/frontend.m': 'mng_active_start(@"oot", mng_active_version());',
    'games/totk/ios/Sources/main.m': 'mng_active_start(@GAME_INBOX_ID, mng_active_version());',
    'games/oot/ios/Sources/main.m': 'mng_active_start(@"oot", mng_active_version());',
    # the other games: their id is the folder name (the same as GAME_INBOX_ID where the game has one)
    **{f'games/{g}/runtime/frontend.m': f'mng_active_start(@"{g}", mng_active_version());' for g in (
        'acnh', 'alc', 'aocx', 'by3', 'dread', 'eow', 'kirby', 'lm3', 'mj2013', 'mk8dx', 'mkw', 'mvdk', 'odyssey', 'ori', 'ot0', 'p5r', 'pla', 'pokesv',
        'rdr', 'ssbu', 'tmnt', 'tomodachi', 'unicorn', 'wonder', 'xc3', 'xcx')},
    # the games on the host runtime: front_mac.m is built in runtime/host, so their copy of the header is there
    **{f'games/{g}/runtime/host/front_mac.m': f'mng_active_start(@"{g}", mng_active_version());' for g in (
        'aoc', 'jamboree', 'la', 'mk8d', 'ss', 'worldend')},
    'games/dread/ios/Sources/main.m': 'mng_active_start(@"dread", mng_active_version());',
    'games/tmnt/ios/Sources/main.m': 'mng_active_start(@"tmnt", mng_active_version());',
}
PANES = ['games/totk/ios/Settings.bundle/Root.plist', 'games/oot/ios/Settings.bundle/Root.plist',
         'games/dread/ios/Settings.bundle/Root.plist', 'games/tmnt/ios/Settings.bundle/Root.plist']   # every iOS app that reports
# each pane is in its app: listed in the iOS project's sources (dread's project.yml is written by build_ipa.py)
PANE_PROJECTS = {'games/totk/ios/project.yml': '- Settings.bundle', 'games/oot/ios/project.yml': '- Settings.bundle',
                 'games/tmnt/ios/project.yml': '- Settings.bundle', 'games/dread/ios/tools/build_ipa.py': "- path: {GAME/'ios/Settings.bundle'}"}


class Header(unittest.TestCase):
    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'clang')
    def test_on_a_mac(self):
        with tempfile.TemporaryDirectory() as d:
            exe = Path(d) / 'probe'
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', f'-I{HEADER.parent}',
                            str(ROOT / 'tests/active_players_probe.m'), '-framework', 'Foundation', '-o', str(exe)], check=True)
            # A fresh folder each time, 20 times: the eight threads race for the first id (2026-11-19: an id file made
            # empty and written afterwards was read empty by another program, one run in a few)
            runs = [subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=60,
                                   env={'MNG_HOME': str(Path(d) / f'home{i}'), 'PATH': '/usr/bin:/bin'}).stdout.splitlines() for i in range(20)]
            left = sorted(p.name for i in range(20) for p in (Path(d) / f'home{i}').iterdir() if p.name.startswith('.install-id'))
        self.assertEqual(left, [])   # each program's own file is gone, linked or not
        for out in runs: self.assertEqual(out, [
            'one id for programs starting together: 1',
            'body keys: app,install,platform,version',            # nothing else leaves the Mac
            'body: app=oot platform=mac version=1.0.9 install-is-file=1',
            'install is 32 hex: 1',
            'sent today: 1, another program today: 1, tomorrow: 1',   # once a day per program
            'off: 0 0',                                            # the Launcher's switch stops every program
            'a damaged id file gives none: 1',
            'today: 10 chars',
        ])

    def test_every_copy_is_the_same_file(self):
        copies = sorted([*ROOT.glob('games/*/runtime/mng_active.h'), *ROOT.glob('games/*/runtime/host/mng_active.h')])
        self.assertIn(HEADER, copies)
        self.assertIn(ROOT / 'games/oot/runtime/mng_active.h', copies)
        self.assertEqual(len({p.read_bytes() for p in copies}), 1, [str(p.relative_to(ROOT)) for p in copies])


class Wiring(unittest.TestCase):
    def test_every_game_start_reports(self):
        for path, call in STARTS.items():
            text = (ROOT / path).read_text()
            self.assertIn('#import "mng_active.h"', text, path)
            self.assertIn(call, text, path)
            self.assertTrue((ROOT / path).parent.joinpath('mng_active.h').is_file() or 'ios/Sources' in path, path)

    def test_no_game_front_end_is_left_out(self):
        # a new game's Mac front end must report too: add it to STARTS
        fronts = {str(p.relative_to(ROOT)) for p in [*ROOT.glob('games/*/runtime/frontend.m'), *ROOT.glob('games/*/runtime/host/front_mac.m')]}
        self.assertEqual(sorted(fronts - set(STARTS)), [])

    def test_the_ios_panes_have_the_switch_on_by_default(self):
        for path in PANES:
            specs = plistlib.loads((ROOT / path).read_bytes())['PreferenceSpecifiers']
            switch = [s for s in specs if s.get('Key') == 'TOTK_USAGE_STATS']
            self.assertEqual(len(switch), 1, path)
            self.assertEqual(switch[0]['Type'], 'PSToggleSwitchSpecifier', path)
            self.assertIs(switch[0]['DefaultValue'], True, path)
        self.assertIn('[NSUserDefaults.standardUserDefaults objectForKey:@"TOTK_USAGE_STATS"]', HEADER.read_text())
        for path, line in PANE_PROJECTS.items():
            self.assertIn(line, (ROOT / path).read_text(), path)
        # every iOS entry that reports has a pane to turn it off in (user 2026-11-19: "设置里可以关")
        for entry in [p for p in STARTS if '/ios/' in p]:
            self.assertIn(entry.split('/ios/')[0] + '/ios/Settings.bundle/Root.plist', PANES, entry)


if __name__ == '__main__':
    unittest.main()

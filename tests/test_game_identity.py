"""A game folder made from another game's keeps none of that game's identity (user 2026-11-25: "检查 还有别的游戏 有这个问题吗",
"12都改"): games/p5r was made from Red Dead Redemption's folder and its iPhone build still declared game='rdr',
GAME_RDR, 'Red Dead Redemption' and org.macnativegames.rdr, so Persona 5 Royal had no scheme of its own; Luigi's
Mansion 3 and Ori kept their display settings in Tears of the Kingdom's settings domain, the one that game reads."""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PORT = re.compile(r'port_ipa\.Port\((.*?)\)\s*$', re.S | re.M)
SETTINGS_DOMAIN = re.compile(r'org\.macnativegames\.([a-z0-9-]+)\.display')


def port_identity(text):
    """What a shared-port iPhone build declares in port_ipa.Port(...), or None when the script is not one."""
    call = PORT.search(text)
    return dict(re.findall(r"\b(game|define|name|bundle_id)='([^']*)'", call.group(1))) if call else None


def wrong_identity(folder, text):
    """[what is wrong] in a games/<folder>/ios/tools/build_ipa.py."""
    declared = port_identity(text)
    if declared is None:
        return []
    want = {'game': folder, 'define': 'GAME_' + folder.upper(), 'bundle_id': 'org.macnativegames.' + folder}
    found = [f"{key}='{declared.get(key)}', not '{value}'" for key, value in want.items() if declared.get(key) != value]
    if not declared.get('name'):
        found.append('no name')
    # the native host it packs is the folder's own (build/<game>-native)
    found += [f'packs {host}' for host in sorted(set(re.findall(r"build/([a-z0-9]+)-native", text))) if host != folder]
    return found


class EveryGameIsItself(unittest.TestCase):
    def test_an_iphone_port_declares_its_own_folder(self):
        wrong = []
        for script in sorted((REPO / 'games').glob('*/ios/tools/build_ipa.py')):
            folder = script.parts[-4]
            wrong += [f'games/{folder}/ios/tools/build_ipa.py: {w}' for w in wrong_identity(folder, script.read_text())]
        self.assertEqual(wrong, [], 'an iPhone port carries another game\'s identity:\n' + '\n'.join(wrong))

    def test_display_settings_are_kept_under_the_game_s_own_name(self):
        wrong = []
        for header in sorted((REPO / 'games').glob('*/runtime/display_settings.h')):
            folder = header.parts[-3]
            wrong += [f'games/{folder}/runtime/display_settings.h: org.macnativegames.{name}.display'
                      for name in sorted(set(SETTINGS_DOMAIN.findall(header.read_text()))) if name != folder]
        self.assertEqual(wrong, [], 'a game keeps its display settings in another game\'s settings domain:\n' + '\n'.join(wrong))

    def test_the_check_tells_a_copied_identity(self):
        copied = ("PORT = port_ipa.Port(\n    game='rdr', define='GAME_RDR', name='Red Dead Redemption', "
                  "bundle_id='org.macnativegames.rdr', layout=layout,\n    mac_cache=mac_cache, pane=pane)\n")
        self.assertEqual(wrong_identity('p5r', copied),
                         ["game='rdr', not 'p5r'", "define='GAME_RDR', not 'GAME_P5R'", "bundle_id='org.macnativegames.rdr', not 'org.macnativegames.p5r'"])
        own = copied.replace('rdr', 'p5r').replace('GAME_RDR', 'GAME_P5R').replace('Red Dead Redemption', 'Persona 5 Royal')
        self.assertEqual(wrong_identity('p5r', own), [])
        self.assertEqual(wrong_identity('p5r', own + "host = work / 'build/rdr-native'\n"), ['packs rdr'])
        self.assertEqual(wrong_identity('p5r', own.replace("name='Persona 5 Royal', ", '')), ['no name'])
        self.assertEqual(wrong_identity('dread', "BUNDLE = 'org.macnativegames.dread'\n"), [])   # not a shared port: its own script


if __name__ == '__main__':
    unittest.main()

"""Every OOT VR hook site fits the text padding after main's code (games/oot/runtime/oot_vr.h oot_vr_hooks_install).

Update 131072's main: its code ends at 0x3c461d0 and its read-only segment starts at 0x3c47000 (the NSO's segment table), so
the installer's limit is 0x3c47000. The padding holds the earlier veneers (up to +1600), the shared hook body
(OOT_HOOK_BODY_WORDS words), five words a site for every entry of OOT_VR_HOOKS (log-only ones too: their stubs are laid out
whether installed or not) and the bombs' two float thunks (24 bytes, 16-aligned). The installer stops the game when they do
not fit; this fails first."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEXT_END, LIMIT = 0x3c461d0, 0x3c47000


def number(source, name):
    return int(re.search(name + r'\s*=\s*(\d+)', source).group(1))


class HookRoom(unittest.TestCase):
    def test_hooks_fit(self):
        vr = (ROOT / 'games/oot/runtime/oot_vr.h').read_text()
        hooks = (ROOT / 'games/oot/runtime/oot_vr_hooks.h').read_text()
        table = vr[vr.index('OOT_VR_HOOKS[] = {'):]
        table = table[:table.index('\n};')]
        sites = re.findall(r'\{ (0x[0-9a-f]+), 0x[0-9a-f]+u, [01], [01], \w+,', table)
        self.assertGreater(len(sites), 0)
        self.assertEqual(len(sites), len(set(sites)), 'a hook site is listed twice')
        pad = (TEXT_END + 15) & ~15
        body = number(vr, 'OOT_VR_HOOK_BODY')
        end = pad + body + number(hooks, 'OOT_HOOK_BODY_WORDS') * 4 + len(sites) * number(hooks, 'OOT_HOOK_STUB_WORDS') * 4
        thunks_end = ((end + 15) & ~15) + 24
        self.assertLessEqual(thunks_end, LIMIT, f'{len(sites)} hook sites: {thunks_end - LIMIT} bytes past the padding')


if __name__ == '__main__':
    unittest.main()

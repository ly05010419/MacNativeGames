"""The VR page's options all fit the table in zeldaVR/gfx/totk_vr.m (VR_OPT_MAX names of at most VR_OPT_NAME - 1 characters).
A name past the table would be dropped when it is set; a longer name would be cut and never read back."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'zeldaVR' / 'gfx'


def option_names():
    names = set()
    for path in sorted(GFX.rglob('*')):
        if path.suffix not in ('.m', '.h', '.inc', '.c'):
            continue
        names.update(re.findall(r'vr_option(?:_set)?\("([^"]+)"', path.read_text(errors='ignore')))
    return names


class OptionRoom(unittest.TestCase):
    def test_every_option_fits(self):
        source = (GFX / 'totk_vr.m').read_text()
        room = int(re.search(r'VR_OPT_MAX\s*=\s*(\d+)', source).group(1))
        name_room = int(re.search(r'VR_OPT_NAME\s*=\s*(\d+)', source).group(1))
        names = option_names()
        self.assertGreater(len(names), 0)
        self.assertLess(len(names), room, f'{len(names)} VR option names, room for {room}: raise VR_OPT_MAX')
        longer = sorted(n for n in names if len(n) >= name_room)
        self.assertEqual(longer, [], f'option names of {name_room} characters or more are cut: {longer}')


if __name__ == '__main__':
    unittest.main()

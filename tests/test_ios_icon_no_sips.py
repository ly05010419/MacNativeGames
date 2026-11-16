"""An iOS package's home-screen icon is drawn without sips (games/oot and games/totk ios/tools/build_ipa.py png_icon).
sips writes through the user's temporary folder on the system disk, which this Mac's build sandbox refuses: 2026-11-15
the Ocarina of Time iPad package stopped at "sips … Cannot write to file /var/folders/…/T/…". The icon is drawn with
the system's AppKit through osascript, as MNG Launcher's own (tools/builder_app/resize_icon.js)."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAMES = ('oot', 'totk')


def load(game):
    tools = ROOT / f'games/{game}/ios/tools'
    sys.path.insert(0, str(tools))
    try:
        spec = importlib.util.spec_from_file_location(f'build_ipa_{game}_icon', tools / 'build_ipa.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.remove(str(tools))
        for name in [n for n, m in sys.modules.items() if str(getattr(m, '__file__', '') or '').startswith(str(tools))]:
            del sys.modules[name]


class IconWithoutSips(unittest.TestCase):
    def test_no_build_ipa_calls_sips(self):
        for game in GAMES:
            text = (ROOT / f'games/{game}/ios/tools/build_ipa.py').read_text()
            self.assertNotIn("'sips'", text, game)
            self.assertIn('png_icon(', text, game)

    def test_the_icon_is_drawn_at_1024(self):
        # an .icns, as the build's build/AppIcon.icns: the system's generic app icon
        source = Path('/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericApplicationIcon.icns')
        self.assertTrue(source.is_file())
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            for game in GAMES:
                build_ipa = load(game)
                out = d / f'{game}-icon.png'
                build_ipa.png_icon(source, 1024, out, d / f'{game}.log', None)
                self.assertEqual(out.read_bytes()[:8], b'\x89PNG\r\n\x1a\n', game)
                width = int.from_bytes(out.read_bytes()[16:20], 'big'); height = int.from_bytes(out.read_bytes()[20:24], 'big')
                self.assertEqual((width, height), (1024, 1024), game)


if __name__ == '__main__':
    unittest.main()

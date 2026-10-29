import json
import plistlib
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "games/totk/ios/tools"
sys.path.insert(0, str(TOOLS))
import build_ipa  # noqa: E402
import settings_strings  # noqa: E402

ROOT = TOOLS.parent / "Settings.bundle/Root.plist"


class SettingsStrings(unittest.TestCase):
    def check(self, root):
        fr = json.loads((TOOLS / "settings_fr.json").read_text())
        t = settings_strings.tables(root, fr)
        for text in settings_strings.texts(root):
            if not settings_strings.CJK.search(text) or settings_strings.split(text) is None and text in ("繁體中文", "简体中文", "日本語"):
                continue
            self.assertIn(text, t["en"], f"no English for {text!r}")
            for lang in ("en", "fr"):
                self.assertNotRegex(t[lang][text], settings_strings.CJK, f"{lang} still has Chinese: {text!r}")
            en = t["en"][text]
            self.assertIn(en, fr, f"no French for {en!r}")

    def test_totk_pane(self):
        self.check(plistlib.loads(ROOT.read_bytes()))

    def test_botw_pane(self):
        root = build_ipa.settings_without_totk(plistlib.loads(ROOT.read_bytes()))
        self.check(root)

    def test_written_bundle(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "Root.plist").write_bytes(ROOT.read_bytes())
            settings_strings.localize(d)
            for lang in ("en", "fr", "zh-Hans", "zh-Hant"):
                table = plistlib.loads((Path(d) / f"{lang}.lproj/Root.strings").read_bytes())
                self.assertEqual(table["畫面 · Display"], {"en": "Display", "fr": "Affichage"}.get(lang, "畫面"))


if __name__ == "__main__":
    unittest.main()

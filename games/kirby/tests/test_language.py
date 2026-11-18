"""Exercise the startup language selection without loading game data."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

RUNTIME = Path(__file__).resolve().parents[1] / "runtime"


class LanguageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        source = root / "probe.c"
        source.write_text(r'''
#include "kirby_preferences.h"
int main(void) {
    kirby_language_apply();
    const char *override = getenv("TOTK_LANG");
    printf("%d|%s|%s|%s\n", kirby_language_choice(), getenv("TOTK_LOCALE"),
           override ? override : "auto", getenv("TOTK_UI_LANG"));
    return 0;
}
''')
        cls.probe = root / "probe"
        subprocess.run(["xcrun", "clang", "-std=gnu11", "-I", str(RUNTIME), str(source),
                        "-framework", "CoreFoundation", "-o", str(cls.probe)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_probe(self, **overrides):
        env = {k: v for k, v in os.environ.items() if not k.startswith(("KIRBY_", "TOTK_LANG", "TOTK_LOCALE", "TOTK_UI_LANG"))}
        env.update(overrides)
        return subprocess.run([str(self.probe)], env=env, capture_output=True, text=True)

    def test_chinese_and_english_set_game_and_host_together(self):
        for choice, want in (("zh-Hans", "1|zh-Hans|zh-Hans|zh"), ("en-US", "2|en-US|en-US|en")):
            result = self.run_probe(KIRBY_LANGUAGE=choice)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), want)

    def test_follow_system_uses_locale_and_keeps_explicit_diagnostics(self):
        result = self.run_probe(KIRBY_LANGUAGE="system", TOTK_LOCALE="zh-Hans-CN")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "0|zh-Hans-CN|auto|zh")
        result = self.run_probe(KIRBY_LANGUAGE="zh-Hans", TOTK_LANG="en-US")
        self.assertEqual(result.stdout.strip(), "1|zh-Hans|en-US|en")


if __name__ == "__main__":
    unittest.main()

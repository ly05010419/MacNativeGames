"""Super Mario Odyssey's language (games/odyssey/runtime/services.c runtime_desired_language): Chinese reaches the game
as zh-CN / zh-TW, the codes its 2017 code knows (CNzh / TWzh text); the newer zh-Hans / zh-Hant fell to its fallback,
Japanese (user 2026-11-21: "Mario 不是中文 是日语 为什么？"). The function is compiled on its own and run per locale."""
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVICES = ROOT / 'games/odyssey/runtime/services.c'
HARNESS = r'''
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void runtime_fail(const char *m) { fprintf(stderr, "%s\n", m); exit(2); }
@FUNCTION@
int main(void) { uint64_t v = runtime_desired_language(); char s[9] = {0}; memcpy(s, &v, 8); printf("%s", s); return 0; }
'''


@unittest.skipUnless(os.uname().sysname == 'Darwin', 'needs CoreFoundation')
class OdysseyLanguage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        text = SERVICES.read_text()
        start = text.index('uint64_t runtime_desired_language(void) {')
        end = text.index('\n}\n', start) + 3
        cls.tmp = tempfile.TemporaryDirectory()
        src, cls.exe = Path(cls.tmp.name, 'lang.c'), Path(cls.tmp.name, 'lang')
        src.write_text(HARNESS.replace('@FUNCTION@', text[start:end]))
        subprocess.run(['cc', '-std=gnu11', '-w', str(src), '-framework', 'CoreFoundation', '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def code(self, **env):
        clean = {k: v for k, v in os.environ.items() if k not in ('TOTK_LANG', 'TOTK_LOCALE')}
        return subprocess.run([str(self.exe)], env={**clean, **env}, capture_output=True, text=True, check=True).stdout

    def test_chinese_as_the_game_knows_it(self):
        for locale, want in (('zh-Hans-CN', 'zh-CN'), ('zh-Hans', 'zh-CN'), ('zh-Hant-TW', 'zh-TW'), ('zh-Hant', 'zh-TW'),
                             ('zh-HK', 'zh-TW'), ('zh-CN', 'zh-CN')):
            with self.subTest(locale=locale):
                self.assertEqual(self.code(TOTK_LOCALE=locale), want)

    def test_the_settings_choice_too(self):
        self.assertEqual(self.code(TOTK_LANG='zh-Hans'), 'zh-CN')
        self.assertEqual(self.code(TOTK_LANG='zh-Hant'), 'zh-TW')

    def test_other_languages_unchanged(self):
        for locale, want in (('ja-JP', 'ja'), ('en-US', 'en-US'), ('ko-KR', 'ko'), ('de-DE', 'de')):
            with self.subTest(locale=locale):
                self.assertEqual(self.code(TOTK_LOCALE=locale), want)


if __name__ == '__main__':
    unittest.main()

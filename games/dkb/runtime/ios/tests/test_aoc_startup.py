"""Run the actual iOS DLC locator (main.m aoc_locate) on macOS/Foundation with private Documents and App folders (no device or game files)."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]


@unittest.skipUnless(sys.platform == 'darwin', 'Foundation required')
class AocStartup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tools = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.tools.cleanup)
        root = Path(cls.tools.name)
        main = (REPO / 'dm/ios/Sources/main.m').read_text()
        locator = main[main.index('static void aoc_locate'):main.index('// Boot prerequisites')]
        source = root / 'aoc.m'
        source.write_text('#import <Foundation/Foundation.h>\n#include <stdio.h>\n#include <stdlib.h>\n'
                          'static NSString *fixture_doc, *fixture_bundle;\n'
                          '@interface FixtureBundle : NSObject\n+ (instancetype)mainBundle;\n- (NSString *)resourcePath;\n@end\n'
                          '@implementation FixtureBundle\n+ (instancetype)mainBundle { return [self new]; }\n'
                          '- (NSString *)resourcePath { return fixture_bundle; }\n@end\n#define NSBundle FixtureBundle\n'
                          '#define NSSearchPathForDirectoriesInDomains(a,b,c) @[fixture_doc]\n' + locator +
                          '\nint main(int argc, char **argv) { @autoreleasepool { if (argc < 3) return 2;\n'
                          'fixture_doc = @(argv[1]); fixture_bundle = @(argv[2]); aoc_locate();\n'
                          'const char *e = getenv("DM_AOC_DIR"); puts(e ? e : "(none)"); return 0; }}\n')
        cls.locator = root / 'aoc'
        subprocess.run(['clang', '-fobjc-arc', '-Wall', '-Werror', str(source), '-framework', 'Foundation', '-o', str(cls.locator)], check=True)

    def locate(self, doc, app, env=None):
        e = {k: v for k, v in os.environ.items() if k != 'DM_AOC_DIR'}; e.update(env or {})
        r = subprocess.run([str(self.locator), str(doc), str(app)], capture_output=True, text=True, env=e)
        self.assertEqual(r.returncode, 0, r.stderr); return r.stdout.strip(), r.stderr

    def test_order_documents_then_bundle_and_none(self):
        with tempfile.TemporaryDirectory() as t:
            doc = Path(t) / 'Documents'; app = Path(t) / 'App'; doc.mkdir(); app.mkdir()
            self.assertEqual(self.locate(doc, app), ('(none)', ''))
            (app / 'aoc').mkdir()
            out, log = self.locate(doc, app)
            self.assertEqual(out, str(app / 'aoc')); self.assertIn(f'boot: DLC from {app / "aoc"}', log)
            (doc / 'aoc').mkdir()
            out, log = self.locate(doc, app)
            self.assertEqual(out, str(doc / 'aoc')); self.assertIn(f'boot: DLC from {doc / "aoc"}', log)

    def test_a_file_named_aoc_does_not_count(self):
        with tempfile.TemporaryDirectory() as t:
            doc = Path(t) / 'Documents'; app = Path(t) / 'App'; doc.mkdir(); app.mkdir()
            (doc / 'aoc').write_text('x'); (app / 'aoc').mkdir()
            self.assertEqual(self.locate(doc, app)[0], str(app / 'aoc'))

    def test_preset_variable_wins(self):
        with tempfile.TemporaryDirectory() as t:
            doc = Path(t) / 'Documents'; app = Path(t) / 'App'; (doc / 'aoc').mkdir(parents=True); (app / 'aoc').mkdir(parents=True)
            out, log = self.locate(doc, app, {'DM_AOC_DIR': '/elsewhere'})
            self.assertEqual(out, '/elsewhere'); self.assertNotIn('DLC from', log)


if __name__ == '__main__': unittest.main()

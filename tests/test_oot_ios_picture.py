"""Ocarina of Time on the iPhone / iPad (user 2026-11-09): the phone offers the Mac's resolution entries 0-3 and starts at the
lowest, 720p from 480p. Only OOT's iOS copy changes; the Mac's settings and its own run of the game patches stay as they are."""
import importlib.util
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IOS = ROOT / 'games/oot/ios'
TOOLS = IOS / 'tools'
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location('oot_build_ipa_picture', TOOLS / 'build_ipa.py')
build_ipa = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_ipa)

HARNESS = r'''
#include "settings_map.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    static const struct { const char *pick, *tier, *scale, *drawable; } cases[] = {
        {"0", "2", "0.6667", "720"}, {"1", "2", "1", "720"}, {"2", "0", "0.6667", "1080"}, {"3", "0", "1", "1080"}};
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        oot_picture p;
        if (!oot_picture_build(cases[i].pick, &p)) { printf("rejected %s\n", cases[i].pick); return 1; }
        if (strcmp(p.tier, cases[i].tier) || strcmp(p.scale, cases[i].scale) || strcmp(p.drawable, cases[i].drawable)) {
            printf("%s -> tier %s, scale %s, drawable %s\n", cases[i].pick, p.tier, p.scale, p.drawable);
            return 1;
        }
    }
    static const char *bad[] = {"", "4", "9", "01", "a", "-1"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        oot_picture p;
        if (oot_picture_build(bad[i], &p)) { printf("accepted \"%s\"\n", bad[i]); return 1; }
    }
    oot_picture p;
    if (oot_picture_build(NULL, &p)) { puts("accepted NULL"); return 1; }
    puts("oot picture ok");
    return 0;
}
'''


class OOTPhonePicture(unittest.TestCase):
    def test_pick_to_canvas_tier_scale_and_rows(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('A C compiler is required')
        with tempfile.TemporaryDirectory() as tmp:
            source, exe = Path(tmp) / 'harness.c', Path(tmp) / 'harness'
            source.write_text(HARNESS)
            build = subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(IOS / 'Sources'),
                                    str(source), str(IOS / 'Sources/settings_map.c'), '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('oot picture ok', run.stdout)

    def test_pane_offers_the_macs_entries(self):
        root = plistlib.loads((IOS / 'Settings.bundle/Root.plist').read_bytes())
        rows = {s['Key']: s for s in build_ipa.oot_pane(root)['PreferenceSpecifiers'] if 'Key' in s}
        row = rows['OOT_RESOLUTION']
        self.assertEqual(row['Type'], 'PSMultiValueSpecifier')
        self.assertEqual(row['Values'], ['0', '1', '2', '3'])
        self.assertEqual(row['DefaultValue'], '0')
        self.assertTrue(row['Titles'][0].startswith('720p（480p 放大，默认）'))
        for gone in ('TOTK_DOCKED', 'TOTK_DOCKED_SCENE', 'TOTK_OUTPUT_RES'):
            self.assertNotIn(gone, rows)

    def test_same_labels_as_the_macs_row(self):
        panel = (ROOT / 'games/oot/runtime/settings_panel.h').read_text()
        start = (IOS / 'Sources/start_screen.m').read_text()
        # The Mac marks its own default entry ("，默认"); the phone's default is the lowest, so the stems are compared.
        for label in ('720p（480p 放大', '720p（原生）', '1080p（720p 放大', '1080p（原生）'):
            self.assertIn(label, panel)
            self.assertIn(label, start)
        self.assertIn('OOT_RESOLUTION', start)

    def test_phone_reads_the_pick(self):
        main = (IOS / 'Sources/main.m').read_text()
        start = main.index('#ifdef GAME_OOT', main.index('static void apply_display_settings'))
        block = main[start:main.index('#endif', start)]
        self.assertIn('OOT_RESOLUTION', block)
        self.assertIn('oot_picture_build', block)
        self.assertIn('@"0"', block)

    def test_mac_run_unchanged_export_and_ios_write_the_scale(self):
        patches = (ROOT / 'games/oot/runtime/game_patches_ns2.c').read_text()
        fixed = patches[patches.index('static void patch_oot_fixed_res'):]
        fixed = fixed[:fixed.index('\n}\n')]
        self.assertIn('if (getenv("TOTK_EXPORT_IOS")) return;', fixed)   # the Mac's own patch is as it was
        self.assertIn('if (getenv("TOTK_EXPORT_IOS")) export_oot_fixed_res_ios(m);', patches)
        self.assertIn('oot_ios_fixed_res(m);', patches)


if __name__ == '__main__':
    unittest.main()

"""Execute the production warm-up policy; no game data or Metal GPU required."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'requires macOS Foundation')
class WarmPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'probe'
        cls.compile(NVN, cls.binary)

    @staticmethod
    def compile(include, binary):
        subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Werror', '-Wno-deprecated-declarations',
                        f'-I{include}', f'-I{NVN}', f'-I{NVN / "include"}',
                        str(ROOT / 'tests/warm_policy_probe.m'), '-framework', 'Foundation', '-o', str(binary)],
                       check=True, capture_output=True, text=True)

    def test_low_memory_list_roles(self):
        result = subprocess.run([self.binary, 'lists'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_archive_hits_are_filtered_before_loading(self):
        result = subprocess.run([self.binary, 'fill'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_resident_modes_and_kind_rows(self):
        result = subprocess.run([self.binary, 'resident'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_oot_mac_keeps_everything_unless_memory_saver(self):   # user 2026-11-16: "开启省内存模式", "默认 不用 省内存模式"
        source = (NVN / 'nvn_mtl.m').read_text()
        mode = source[source.index('static int warm_resident_mode(void)'):source.index('static int warm_shared(void)')]
        iphone, oot, other = mode.index('#if TARGET_OS_IPHONE'), mode.index('#elif defined(GAME_OOT)'), mode.index('#else')
        self.assertIn('WARM_RESIDENT_TITLE_LOCAL', mode[iphone:oot])
        self.assertIn('#define WARM_SAVER_DEFAULT 0', (NVN / 'nvn_warm_resident.h').read_text())
        # no file: the default, off (every list kept)
        self.assertIn('runtime_setting_word(WARM_SAVER_FILE, word, sizeof word) ? warm_saver_word(word) : WARM_SAVER_DEFAULT', mode[oot:other])
        self.assertIn('saver = WARM_SAVER_DEFAULT; }', mode[oot:other])   # an unknown word: the default too
        self.assertIn('const int fallback = saver > 0 ? WARM_RESIDENT_TITLE_KINDS : WARM_RESIDENT_ALL;', mode[oot:other])
        self.assertIn('WARM_RESIDENT_ALL', mode[other:mode.index('#endif')])
        # the Launcher writes the file the runtime reads
        sys.path.insert(0, str(ROOT))
        from tools.builder_jobs import memory_saver
        self.assertIn(f'#define WARM_SAVER_FILE "{memory_saver.FILE}"', (NVN / 'nvn_warm_resident.h').read_text())
        start = source[source.index('static void warm_start(void) {'):]
        self.assertIn('warm_resident_lists(paths, title, kinds ? nil : user, g_par_valid, warm_shared(), &archive)', start)
        self.assertLess(start.index('warm_kind_rows(app_lists)'), start.index('warm_plan(paths,'))
        kinds = start[start.index('NSMutableArray<NSString *> *app_lists'):start.index('warm_kind_rows(app_lists)')]
        self.assertIn('![p isEqualToString:user]', kinds)              # the device's lists are not App lists
        self.assertIn('![p isEqualToString:pso_shared_path()]', kinds)

    def test_reverse_controls_detect_both_regressions(self):
        source = (NVN / 'nvn_warm_policy.h').read_text()
        cases = [
            ('lists', 'BOOL resident = (valid && shared) || [path isEqualToString:title] || (valid && [path isEqualToString:user]);',
             'BOOL resident = valid ? (shared || ![path.lastPathComponent isEqualToString:@"pso_list.tsv"]) : [path isEqualToString:title];'),
            ('fill', 'if (!recheck && have(key)) ++*hits;', 'if (NO) ++*hits;'),
        ]
        for mode, before, after in cases:
            with self.subTest(mode=mode):
                self.assertEqual(source.count(before), 1)
                folder = Path(self.tmp.name) / mode
                folder.mkdir()
                (folder / 'nvn_warm_policy.h').write_text(source.replace(before, after))
                binary = folder / 'probe'
                self.compile(folder, binary)
                result = subprocess.run([binary, mode], capture_output=True, text=True)
                self.assertEqual(result.returncode, 1, 'regression was not detected: ' + result.stdout)
                self.assertIn('FAIL:', result.stderr)

    def test_runtime_filters_before_multithreaded_preload_and_waits(self):
        source = (NVN / 'nvn_mtl.m').read_text()
        fill = source[source.index('static void warm_fill_archive('):source.index('static void offline_depth_mode(')]
        self.assertLess(fill.index('block = warm_fill_pending('), fill.index('warm_load_rows(block, dict)'))
        self.assertIn('at + processed', fill)  # filtered rows still advance startup progress
        start = source[source.index('static void warm_start(void) {'):]
        self.assertIn('warm_resident_lists(paths, title, kinds ? nil : user, g_par_valid, warm_shared(), &archive)', start)
        self.assertIn('g_par_fill_lists = archive ?: @[]', start)
        self.assertLess(start.index('dispatch_semaphore_wait(d, DISPATCH_TIME_FOREVER)'), start.index('runtime_warm_title()'))


if __name__ == '__main__':
    unittest.main()

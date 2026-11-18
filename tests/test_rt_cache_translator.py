"""A device cache folder records the translator that writes into it (2026-11-16, user: "修 1234").

The folder is named after the seed's translation context, which names the seed's translator; its translations are made
by the App's build/shader-service/totk_xlate. Ocarina of Time's express updates from 11-15 shipped an older in-game
translator than the seed's, so its folder held another translator's variants under the new one's name. This compiles
totk_rt_translator_check (totk_rt_translator.h) with totk_rt_carry_over (totk_rt_carry.h) and runs them on folders it
makes, in the order totk_rt_dir runs them: the folder another translator wrote is set aside, a new one records this
translator, and the pipeline archive and collected list still come along."""
from pathlib import Path
import hashlib
import subprocess
import tempfile
import unittest

NVN = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn'
NEW = hashlib.sha256(b'translator of today').hexdigest()
OLD = hashlib.sha256(b'translator of 11-11').hexdigest()

PROGRAM = r'''#import <Foundation/Foundation.h>
#include "totk_rt_carry.h"
#include "totk_rt_translator.h"
int main(int argc, char **argv) { @autoreleasepool {
    if (argc == 2) { NSString *h = totk_file_sha256(@(argv[1])); printf("%s\n", h ? h.UTF8String : "nil"); return 0; }
    if (argc == 3 && !strcmp(argv[1], "code")) { NSString *h = totk_code_sha256(@(argv[2])); printf("%s\n", h ? h.UTF8String : "nil"); return 0; }
    NSString *why = nil;
    NSString *retired = totk_rt_translator_check(@(argv[1]), @(argv[2]), argc > 4 ? @(argv[4]) : nil, atoi(argv[3]), &why);
    printf("retired %s\nwhy %s\n", retired ? retired.lastPathComponent.UTF8String : "-", why ? why.UTF8String : "-");
    for (NSString *from in totk_rt_carry_over(@(argv[1]))) printf("moved %s\n", from.UTF8String);
    return 0;
} }
'''


class TranslatorRecord(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.cache = self.base / 'shader-cache'
        source = self.base / 'check.m'
        source.write_text(PROGRAM)
        self.exe = self.base / 'check'
        built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Werror', '-I', str(NVN), '-framework', 'Foundation',
                                str(source), '-o', str(self.exe)], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stderr)

    def tearDown(self):
        self.temp.cleanup()

    def folder(self, name='rt-v28-ctx', record=None, translations=True, archive=True):
        d = self.cache / name
        d.mkdir(parents=True)
        if translations:
            (d / 'abc-0123456789ab_b.metal').write_text('made by some translator')
            (d / 'abc.variants').write_text('index')
        if archive:
            (d / 'pipelines').mkdir()
            (d / 'pipelines/one.metalarchive').write_text('pipelines')
            (d / 'pso_collected.tsv').write_text('R\trow\n')
        if record:
            (d / 'translator.sha256').write_text(record + '\n')
        return d

    def check(self, d, translator=NEW, untrusted=1, file=None):
        out = subprocess.run([str(self.exe), str(d), translator, str(untrusted), *([file] if file else [])], capture_output=True,
                             text=True, check=True)
        lines = dict(l.split(' ', 1) for l in out.stdout.splitlines() if not l.startswith('moved'))
        return lines['retired'], lines['why'], [l[6:] for l in out.stdout.splitlines() if l.startswith('moved')]

    def record(self, d):
        return (d / 'translator.sha256').read_text().strip()

    def test_an_unrecorded_folder_of_an_untrusted_game_is_set_aside(self):   # Ocarina of Time's rt-...-e37674ae
        d = self.folder()
        retired, why, moved = self.check(d)
        self.assertTrue(retired.startswith('rt-v28-ctx-retired-unrecorded-'), retired)
        self.assertIn('do not name their translator', why)
        self.assertEqual(self.record(d), NEW)
        self.assertFalse((d / 'abc-0123456789ab_b.metal').exists())                  # its translations are not used
        self.assertTrue((self.cache / retired / 'abc-0123456789ab_b.metal').exists())  # set aside, not deleted
        self.assertEqual(len(moved), 2, moved)                                         # archive and list come along
        self.assertTrue((d / 'pipelines/one.metalarchive').exists())
        self.assertEqual((d / 'pso_collected.tsv').read_text(), 'R\trow\n')

    def test_an_unrecorded_folder_of_another_game_is_adopted(self):   # build_app.py gives every App its seed's translator
        d = self.folder()
        retired, why, moved = self.check(d, untrusted=0)
        self.assertEqual((retired, why, moved), ('-', '-', []))
        self.assertEqual(self.record(d), NEW)
        self.assertTrue((d / 'abc-0123456789ab_b.metal').exists())

    def test_an_unrecorded_empty_folder_just_records(self):
        d = self.folder(translations=False, archive=False)
        self.assertEqual(self.check(d)[0], '-')
        self.assertEqual(self.record(d), NEW)

    def test_the_same_translator_keeps_its_folder(self):
        d = self.folder(record=NEW)
        self.assertEqual(self.check(d), ('-', '-', []))
        self.assertTrue((d / 'abc-0123456789ab_b.metal').exists())

    def test_another_recorded_translator_is_set_aside_in_every_game(self):
        d = self.folder(record=OLD)
        retired, why, moved = self.check(d, untrusted=0)
        self.assertTrue(retired.startswith('rt-v28-ctx-retired-' + OLD[:12]), retired)
        self.assertIn(OLD[:12], why)
        self.assertEqual(self.record(d), NEW)
        self.assertFalse((d / 'abc-0123456789ab_b.metal').exists())
        self.assertEqual(len(moved), 2, moved)

    def test_a_record_of_the_same_file_from_before_code_identity_keeps_its_folder(self):   # 2026-11-18
        d = self.folder(record=OLD)   # OLD: the whole-file SHA-256 the folder recorded before; NEW: the program's code
        self.assertEqual(self.check(d, untrusted=1, file=OLD), ('-', '-', []))
        self.assertEqual(self.record(d), NEW)                                   # now recorded by its code
        self.assertTrue((d / 'abc-0123456789ab_b.metal').exists())
        retired = self.check(self.folder('rt-v28-other', record=OLD), untrusted=0, file=NEW)[0]   # another file: set aside
        self.assertTrue(retired.startswith('rt-v28-other-retired-' + OLD[:12]), retired)

    def test_a_packaging_cache_is_left_alone(self):   # prepare_ns2_seed.py's caches are named by their context
        d = self.folder('e37674ae')
        self.assertEqual(self.check(d)[0], '-')
        self.assertFalse((d / 'translator.sha256').exists())
        self.assertTrue((d / 'abc-0123456789ab_b.metal').exists())

    def test_no_known_translator_changes_nothing(self):
        d = self.folder()
        self.assertEqual(self.check(d, translator='missing')[0], '-')
        self.assertFalse((d / 'translator.sha256').exists())

    def test_the_translator_is_named_by_its_sha256(self):
        program = self.base / 'totk_xlate'
        program.write_bytes(b'translator of today')
        out = subprocess.run([str(self.exe), str(program)], capture_output=True, text=True, check=True).stdout.strip()
        self.assertEqual(out, NEW)
        self.assertEqual(subprocess.run([str(self.exe), str(self.base / 'missing')], capture_output=True, text=True,
                                        check=True).stdout.strip(), 'nil')

    def test_the_runtime_checks_before_it_carries_over(self):
        rt = (NVN / 'totk_shader_rt.m').read_text()
        self.assertIn('#include "totk_rt_translator.h"', rt)
        body = rt[rt.index('NSString *totk_rt_dir(void)'):]
        self.assertLess(body.index('totk_rt_translator_check(g_dir'), body.index('totk_rt_carry_over(g_dir)'))
        self.assertIn('/build/shader-service/totk_xlate', body[:body.index('totk_rt_translator_check(g_dir')])
        oot = body[body.index('#ifdef GAME_OOT'):body.index('#endif', body.index('#ifdef GAME_OOT'))]
        self.assertIn('unrecorded_untrusted = YES', oot)
        self.assertIn('translator = totk_code_sha256(service)', body)   # recorded by its code, any signature (2026-11-18)
        self.assertIn('totk_rt_translator_check(g_dir, translator, file,', body)


if __name__ == '__main__':
    unittest.main()

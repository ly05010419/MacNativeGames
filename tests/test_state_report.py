"""The state reports for Apple's performance tools (games/totk/third_party/d2r_gfx/gfx/nvn/totk_state_report.m,
TOTK_STATE_REPORTING=1): tests/state_report_probe.m against a fake SRStateReporter (any macOS), with the framework
missing (as on macOS 26) and against the real framework (macOS 27); and source checks that keep the reports off the
build and off iOS."""
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT/'games/totk'
NVN = TOTK/'third_party/d2r_gfx/gfx/nvn'
PROBE = ROOT/'tests/state_report_probe.m'
BUILDS = {
    'fake': ['-DTOTK_SR_FRAMEWORK="/usr/lib/libSystem.B.dylib"', '-DTOTK_SR_CLASS=@"FakeSRStateReporter"'],
    'missing': ['-DTOTK_SR_FRAMEWORK="/nonexistent/StateReporting.framework/StateReporting"'],
    'real': [],
}


def mac_major():
    try:
        return int(platform.mac_ver()[0].split('.')[0])
    except ValueError:
        return 0


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs macOS and Xcode command line tools')
class StateReportProbe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='totk-state-report-')
        cls.bins = {}
        for name, defines in BUILDS.items():
            out = Path(cls.tmp.name)/name
            subprocess.run(['xcrun', 'clang', '-std=gnu11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                            '-I' + str(NVN/'include'), *defines, str(PROBE), '-framework', 'Foundation', '-o', str(out)],
                           check=True, capture_output=True, text=True)
            cls.bins[name] = out

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_probe(self, build, scenario, on=True, **extra):
        env = {k: v for k, v in os.environ.items() if k != 'TOTK_STATE_REPORTING' and k != 'TOTK_SOURCE_COMMIT'}
        if on:
            env['TOTK_STATE_REPORTING'] = '1'
        env.update(extra)
        r = subprocess.run([str(self.bins[build]), scenario], env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f'pass: state reports {scenario}', r.stdout)
        return r.stderr

    def test_off_by_default(self):
        self.run_probe('fake', 'off', on=False)

    def test_flag_needs_the_game_window(self):
        self.assertIn('only a game with its window', self.run_probe('fake', 'headless'))

    def test_lifecycle_sequence_and_metadata(self):
        self.run_probe('fake', 'lifecycle')

    def test_runtime_uuid_is_the_executables(self):
        # an inserted library (DYLD_INSERT_LIBRARIES, as Xcode's checkers and capture tools do) comes first in dyld's list
        lib = Path(self.tmp.name)/'inserted.dylib'
        src = Path(self.tmp.name)/'inserted.c'
        src.write_text('int inserted_probe_library(void) { return 1; }\n')
        subprocess.run(['xcrun', 'clang', '-dynamiclib', str(src), '-o', str(lib)], check=True, capture_output=True, text=True)
        uuid = re.search(r'UUID: ([0-9A-F-]{36})', subprocess.run(['xcrun', 'dwarfdump', '--uuid', str(self.bins['fake'])],
                                                                    capture_output=True, text=True, check=True).stdout).group(1)
        self.run_probe('fake', 'lifecycle', PROBE_UUID=uuid, DYLD_INSERT_LIBRARIES=str(lib))

    def test_graphics_settles_and_labels(self):
        self.run_probe('fake', 'graphics')

    def test_switch_off_and_on_while_playing(self):
        self.run_probe('fake', 'toggle')

    def test_refused_report_stops_reports(self):
        self.assertIn('refused a report', self.run_probe('fake', 'refusal'))

    def test_without_the_framework_the_game_goes_on(self):
        self.assertIn('StateReporting is not there', self.run_probe('missing', 'missing'))

    @unittest.skipUnless(mac_major() >= 27, 'StateReporting needs macOS 27')
    def test_real_framework_accepts_the_reports(self):
        err = self.run_probe('real', 'real')
        self.assertNotIn('refused', err)
        self.assertIn('Lifecycle: focused', err)


class StateReportSources(unittest.TestCase):
    def test_no_link_to_the_framework(self):
        # Opened at run time: the build works with any SDK and the binary starts on macOS 26.
        for path in [TOTK/'Makefile', ROOT/'build_app.py', ROOT/'update_runtime.py', TOTK/'ios/project.yml',
                     *TOTK.glob('ios/tools/*.py'), *TOTK.glob('scripts/*.py')]:
            self.assertNotRegex(path.read_text(errors='replace'), r'StateReporting', str(path))
        users = [p.name for p in [*NVN.glob('*.[chm]'), *(TOTK/'runtime').glob('*.[chm]')]
                 if 'SRStateReporter' in p.read_text(errors='replace') or '<StateReporting' in p.read_text(errors='replace')]
        self.assertEqual(users, ['totk_state_report.m'])

    def test_mac_only(self):
        body = (NVN/'totk_state_report.m').read_text()
        self.assertRegex(body, r'#include <TargetConditionals.h>\n#if TARGET_OS_OSX\n')
        self.assertTrue(body.rstrip().endswith('#endif'))
        header = (NVN/'totk_state_report.h').read_text()
        other = header.split('#else', 1)[1]
        for name in ('totk_sr_on', 'totk_sr_set', 'totk_sr_warmup', 'totk_sr_frame'):   # what the shared (iOS) code calls
            self.assertRegex(other, rf'static inline \w+ {name}\(', name)

    def test_hooks_only_while_on(self):
        hle = (NVN/'hle_nvn.c').read_text()
        self.assertEqual(re.findall(r'.*\bsr_frame\(o\);', hle), ['    if (totk_sr_on()) sr_frame(o);'])
        mtl = (NVN/'nvn_mtl.m').read_text()
        stores = re.findall(r'.*g_taa_last, ', mtl)   # key 5's encode and the temporal upscale (tup_dock)
        self.assertEqual(len(stores), 2)
        for s in stores: self.assertRegex(s, r'^\s+if \(totk_sr_on\(\)\) atomic_store\(&g_taa_last, $')
        front = (TOTK/'runtime/frontend.m').read_text() + (TOTK/'runtime/settings_panel.h').read_text()   # weak, checked: tests include them
        names = re.findall(r'extern void (totk_sr_\w+)\([^;]*\) __attribute__\(\(weak_import\)\);', front)
        self.assertEqual(sorted(names), sorted(set(re.findall(r'\b(totk_sr_\w+)\(', front))))
        for name in names:
            self.assertRegex(front, rf'if\(!?{name}[)|&]', name)

if __name__ == '__main__':
    unittest.main()

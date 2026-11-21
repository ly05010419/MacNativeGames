"""The tested Metal compiler as a requirement (GitHub issue #242): dm/gfx/tools/own_compilers.py requires the pinned spirv-cross, and stops on another Metal
compiler (macOS or iOS) with a message naming the Xcode to install, unless DM_ALLOW_OTHER_METAL=1; a missing compiler or another translator still fails as dmsh_build.py reports it.
The NSP script checks the Metal compiler before an NVN game's shader steps (check_metal), and ALLOW_OTHER_METAL / --allow-other-metal skips the check and passes the override to the
shader step. No Xcode, translator or game files needed: the tools' fingerprints are stand-ins."""
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from contextlib import redirect_stdout
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_chain  # noqa: E402
import own_compilers  # noqa: E402
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec and spec.loader
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)

with open(dmsh_chain.LOCK) as _f: LOCKED = dict(l.rstrip('\n').split('\t', 1) for l in _f if '\t' in l and not l.startswith('#'))
OTHER = 'Apple metal version 32023.850 (metalfe-32023.850.10)'   # Xcode 26's, say


def mismatch(key, actual): return f'{key}: lock {LOCKED[key]} / actual {actual}'


class CheckTools(unittest.TestCase):
    def check(self, bad, allow=False):
        env = {k: v for k, v in os.environ.items() if k != own_compilers.ALLOW}
        if allow: env[own_compilers.ALLOW] = '1'
        out = io.StringIO()
        with mock.patch.object(own_compilers, 'strict', lambda: list(bad)), mock.patch.dict(os.environ, env, clear=True), redirect_stdout(out):
            return own_compilers.check_tools(), out.getvalue()

    def test_other_metal_stops(self):
        for key in ('metal_macosx', 'metal_iphoneos'):
            with self.assertRaises(SystemExit) as e: self.check([mismatch(key, OTHER)])
            msg = str(e.exception.code)
            self.assertTrue(msg.startswith('[RED] '), msg)
            for want in (key, OTHER, LOCKED[key], f'Xcode {own_compilers.XCODE_VERSION}', 'macOS 26.6', 'xcodebuild -downloadComponent MetalToolchain', 'DM_ALLOW_OTHER_METAL=1', '#242'): self.assertIn(want, msg)

    def test_other_metal_says_to_select_the_tested_xcode(self):   # a newer Xcode selected downloads its own toolchain again (GitHub issue #717)
        sel = ' (selected: Xcode 27.3 in /Applications/Xcode.app/Contents/Developer)'
        with mock.patch.object(own_compilers, 'selected_xcode', lambda: sel):
            msg = own_compilers.other_metal_message([('metal_macosx', LOCKED['metal_macosx'], OTHER)])
        app = own_compilers.XCODE_APP
        for want in (sel, 'https://developer.apple.com/download/all/', f'sudo xcode-select -s {app}', f'DEVELOPER_DIR={app}/Contents/Developer'): self.assertIn(want, msg)
        self.assertLess(msg.index('xcode-select -s'), msg.index('-downloadComponent'), 'select the tested Xcode before downloading its toolchain')

    def test_selected_xcode_without_xcode(self):   # no xcode-select or xcodebuild: the message goes without the selected Xcode
        with mock.patch.object(own_compilers.subprocess, 'run', side_effect=FileNotFoundError):
            self.assertEqual(own_compilers.selected_xcode(), '')

    def test_other_metal_allowed(self):
        bad, out = self.check([mismatch('metal_macosx', OTHER), mismatch('metal_iphoneos', OTHER)], allow=True)
        self.assertEqual(bad, [])
        self.assertTrue(out.startswith('note: built with this Mac\'s compilers'), out)
        self.assertIn(f'metal_macosx {OTHER} (tested: {LOCKED["metal_macosx"]})', out); self.assertIn('DM_ALLOW_OTHER_METAL=1', out)

    def test_other_spirv_cross_stops_even_with_metal_override(self):
        failure = mismatch('spirv-cross', '0123456789abcdef')
        for allow in (False, True):
            bad, out = self.check([failure], allow=allow)
            self.assertEqual(bad, [failure]); self.assertEqual(out, '')

    def test_missing_or_translator_still_fail(self):   # returned to dmsh_build.py, which stops with its own message
        xlate = mismatch('dmsh_xlate.dll', '0' * 16)
        for bad, want in (([mismatch('metal_macosx', 'missing')],) * 2, ([mismatch('spirv-cross', 'missing')],) * 2, ([xlate, mismatch('metal_macosx', OTHER)], [xlate])):
            got, out = self.check(bad)
            self.assertEqual(got, want); self.assertEqual(out, '')

    def test_missing_ios_compiler_fails_only_for_ios(self):   # a Mac-only build (DMSH_TARGETS=mac) on a Mac without Xcode's iOS platform
        missing = mismatch('metal_iphoneos', 'missing')
        for targets, want in (('mac', []), ('mac,ios', [missing]), ('ios', [missing])):
            with self.subTest(targets=targets), mock.patch.dict(os.environ, {'DMSH_TARGETS': targets}):
                self.assertEqual(self.check([missing])[0], want)
        with mock.patch.dict(os.environ, {'DMSH_TARGETS': 'mac'}): self.assertEqual(self.check([mismatch('metal_macosx', 'missing')])[0], [mismatch('metal_macosx', 'missing')])

    def test_check_metal(self):   # --check-metal, as the NSP script runs it: the message when it would stop, '' otherwise
        for actual, allow, stops in ((LOCKED['metal_macosx'], False, False), (OTHER, False, True), (OTHER, True, False), ('missing', False, False)):
            env = {k: v for k, v in os.environ.items() if k != own_compilers.ALLOW}
            if allow: env[own_compilers.ALLOW] = '1'
            with mock.patch.object(own_compilers, 'metal_version', lambda sdk, a=actual: a), mock.patch.dict(os.environ, env, clear=True):
                msg = own_compilers.check_metal()
            self.assertEqual(bool(msg), stops, (actual, allow))
            if stops: self.assertIn(f'metal_macosx {OTHER}', msg); self.assertIn(f'metal_iphoneos {OTHER}', msg)


class NspScript(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)

    def test_stops_with_the_script_override(self):
        msg = own_compilers.other_metal_message([('metal_macosx', LOCKED['metal_macosx'], OTHER)])
        done = subprocess.CompletedProcess([], 1, stdout=msg + '\n', stderr='')
        with mock.patch.object(nsp, 'ALLOW_OTHER_METAL', False), mock.patch.object(nsp.subprocess, 'run', return_value=done) as run, self.assertRaises(nsp.BuildError) as e:
            nsp.check_metal(sys.executable, REPO, {})
        self.assertIn('--check-metal', run.call_args[0][0])
        text = str(e.exception)
        self.assertIn(f'Xcode {own_compilers.XCODE_VERSION}', text); self.assertIn('ALLOW_OTHER_METAL = True (--allow-other-metal)', text); self.assertFalse(text.startswith('[RED]'))

    def test_tested_metal_or_override_goes_on(self):
        with mock.patch.object(nsp, 'ALLOW_OTHER_METAL', False), mock.patch.object(nsp.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '', '')):
            nsp.check_metal(sys.executable, REPO, {})
        with mock.patch.object(nsp, 'ALLOW_OTHER_METAL', True), mock.patch.object(nsp.subprocess, 'run') as run:
            nsp.check_metal(sys.executable, REPO, {})
        run.assert_not_called()

    def test_check_metal_runs(self):   # the real tool with the real lock: exit 0 or 1 with the message, never a traceback (no Xcode: no compiler, which dmsh_build.py reports)
        r = subprocess.run([sys.executable, str(REPO / 'dm/gfx/tools/own_compilers.py'), '--check-metal'], capture_output=True, text=True,
                           env={k: v for k, v in os.environ.items() if k != own_compilers.ALLOW})
        self.assertIn(r.returncode, (0, 1), r.stderr)
        if r.returncode: self.assertIn(f'Xcode {own_compilers.XCODE_VERSION}', r.stdout)

    def test_flag(self):
        self.assertTrue(nsp.parse_args(['x', '--allow-other-metal']).allow_other_metal)
        self.assertFalse(nsp.parse_args(['x']).allow_other_metal)

    def test_override_reaches_the_shader_step(self):
        ran = []
        def fake_run(cmd, log, env=None, cwd=None, on_line=None):
            ran.append(dict(env or {})); raise nsp.BuildError('stop here')
        g = types.SimpleNamespace(table=lambda n: self.tmp / n, dir=self.tmp)
        for allow in (False, True):
            with mock.patch.object(nsp, 'ALLOW_OTHER_METAL', allow), mock.patch.object(nsp, 'run', fake_run), mock.patch.object(nsp, 'PROG', mock.Mock()), \
                    mock.patch.object(nsp, 'chain_of', lambda repo: '0123456789ab'), self.assertRaises(nsp.BuildError):
                nsp.build_shaders(sys.executable, REPO, self.tmp / 'shader', REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate', {'WORK': str(self.tmp)}, self.tmp, g, pkg_key=None, mac=True)
            self.assertEqual(ran[-1].get('DM_ALLOW_OTHER_METAL'), '1' if allow else None)


if __name__ == '__main__':
    unittest.main()

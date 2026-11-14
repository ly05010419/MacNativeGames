"""NSP script environment checks (scripts/build_ipa_from_nsp.py), all before the extraction: an iOS build without Xcode's iOS SDK, a shader translator that does not start
with the .NET it is given, a missing glslangValidator, and a Python under Rosetta each stop the build at once instead of at a shader step minutes later.
Every input (xcrun, xcode-select, sysctl, the translator, PATH) is mocked: no Xcode, .NET, glslang or Rosetta needed."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec and spec.loader
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_chain  # noqa: E402
import tool_lock  # noqa: E402


def fake_run(sdk_rc=0, devdir='/Applications/Xcode.app/Contents/Developer', xlate=(0, 'f21af1efa737a270\n', ''), translated='0', calls=None):
    def run(cmd, **kw):
        cmd = [str(c) for c in cmd]
        if calls is not None: calls.append(cmd)
        if cmd[:2] == ['xcrun', '--sdk']: return subprocess.CompletedProcess(cmd, sdk_rc, '' if sdk_rc else '27.1\n', 'xcrun: error: SDK "iphoneos" cannot be located' if sdk_rc else '')
        if cmd[0] == 'xcode-select': return subprocess.CompletedProcess(cmd, 0, devdir + '\n', '')
        if cmd[0] == 'sysctl': return subprocess.CompletedProcess(cmd, 0, translated + '\n', '')
        if cmd[-1] == '--source-sha':
            if isinstance(xlate, BaseException): raise xlate
            return subprocess.CompletedProcess(cmd, *xlate)
        return subprocess.CompletedProcess(cmd, 0, '', '')   # xcrun metal --version
    return run


def fake_capstone(major):   # the installed capstone's version does not matter to these tests
    return types.SimpleNamespace(__version__=f'{major}.0.0', cs_version=lambda: (major, 0, major << 8))


class Prereqs(unittest.TestCase):
    def check(self, mac=False, which=lambda *a, **k: '/x', capstone=5, **kw):
        calls = []
        with tempfile.TemporaryDirectory() as d, mock.patch.object(nsp, '_importable', lambda m: True), mock.patch.object(nsp.shutil, 'which', which), \
                mock.patch.object(nsp.subprocess, 'run', fake_run(calls=calls, **kw)), mock.patch.dict(sys.modules, capstone=fake_capstone(capstone)):
            xlate = Path(d) / 'dmsh_xlate'; xlate.write_text('')
            try: nsp.check_prereqs({'PATH': '/usr/bin', 'DOTNET_ROOT': '/stale'}, xlate, mac)
            finally: self.calls = calls

    def test_ios_without_sdk_stops_before_the_metal_check(self):   # whose message would blame the Metal toolchain
        with self.assertRaisesRegex(nsp.BuildError, r'iOS SDK is not installed.*Components.*downloadPlatform iOS'): self.check(sdk_rc=1)
        self.assertFalse(any('metal' in c for c in self.calls))

    def test_command_line_tools_selected(self):
        with self.assertRaisesRegex(nsp.BuildError, r'Command Line Tools.*xcode-select -s /Applications/Xcode.app'): self.check(sdk_rc=1, devdir='/Library/Developer/CommandLineTools')

    def test_mac_build_does_not_ask_for_the_ios_sdk(self):   # own_compilers.py lets a Mac-only shader build go on without the iOS compiler
        self.check(mac=True, sdk_rc=1)
        self.assertFalse(any('iphoneos' in c for c in self.calls))

    def test_translator_runs_once_with_the_scripts_environment(self):
        run = mock.Mock(side_effect=fake_run())
        with tempfile.TemporaryDirectory() as d, mock.patch.object(nsp.subprocess, 'run', run):
            xlate = Path(d) / 'dmsh_xlate'; nsp.check_translator(xlate, {'PATH': '/bin', 'DOTNET_ROOT': '/stale'})
        self.assertEqual(run.call_args.args[0], [str(xlate), '--source-sha'])
        self.assertEqual(run.call_args.kwargs['env']['DOTNET_ROOT'], '/stale'); self.assertTrue(run.call_args.kwargs['timeout'])

    def test_translator_without_its_runtime_stops(self):   # the apphost's exit 131: its first line, not an empty `xlate_source` at the shader step
        with self.assertRaises(nsp.BuildError) as e: self.check(xlate=(131, '', 'You must install .NET to run this application.\n\nApp: x'))
        self.assertIn('does not run (You must install .NET to run this application.)', str(e.exception)); self.assertIn('DOTNET_ROOT', str(e.exception))
        with self.assertRaisesRegex(nsp.BuildError, r'does not run \(exit code 0\)'): self.check(xlate=(0, ' \n', ''))

    def test_translator_that_cannot_start_or_hangs_stops(self):
        for exc in (OSError('Exec format error'), subprocess.TimeoutExpired('dmsh_xlate', 60)):
            with self.subTest(exc=exc), self.assertRaisesRegex(nsp.BuildError, 'does not start'): self.check(xlate=exc)

    def test_glslang_is_a_prerequisite(self):   # README installs it; an OpenGL game would otherwise stop only after its run-time capture
        with self.assertRaisesRegex(nsp.BuildError, r'glslangValidator \(brew install glslang\)'): self.check(which=lambda t, **k: None if t == 'glslangValidator' else '/x')

    def test_capstone_5_passes(self):
        with mock.patch.dict(sys.modules, capstone=fake_capstone(5)): nsp.check_capstone()

    def test_other_capstone_stops_with_the_install_command(self):   # Capstone 6 renamed CS_ARCH_ARM64 and the operand constants patch_nso.py uses
        for major in (4, 6):
            with self.subTest(major=major), mock.patch.dict(sys.modules, capstone=fake_capstone(major)):
                with self.assertRaisesRegex(nsp.BuildError, rf'Capstone {major}\.0\.0 is unsupported.*-m pip install .capstone>=5,<6'): nsp.check_capstone()
        with self.assertRaisesRegex(nsp.BuildError, 'Capstone 6'): self.check(capstone=6, which=lambda *a, **k: None)   # before the tools and the Metal check


class Rosetta(unittest.TestCase):
    def check(self, translated, platform='darwin'):
        with mock.patch.object(nsp.sys, 'platform', platform), mock.patch.object(nsp.subprocess, 'run', fake_run(translated=translated)):
            nsp.check_native_python()

    def test_translated_stops(self):
        with self.assertRaisesRegex(nsp.BuildError, r'running under Rosetta.*arm64 Python'): self.check('1')

    def test_native_or_intel_mac_passes(self):   # an Intel Mac has no sysctl.proc_translated
        self.check('0'); self.check('')

    def test_other_systems_are_not_asked(self):
        with mock.patch.object(nsp.sys, 'platform', 'linux'), mock.patch.object(nsp.subprocess, 'run', side_effect=AssertionError): nsp.check_native_python()


class ChainCheck(unittest.TestCase):   # dmsh_build.py run directly (tool_lock.check_tools) names the translator that does not run, not an empty `xlate_source`
    def embedded(self, xlate):
        with mock.patch.object(tool_lock.subprocess, 'run', fake_run(xlate=xlate)): return tool_lock.xlate_embedded()

    def test_embedded(self):
        self.assertEqual(self.embedded((0, 'f21af1efa737a270\n', '')), 'f21af1efa737a270')
        self.assertEqual(self.embedded((131, '', 'You must install .NET to run this application.\n\nApp: x')), 'does not run (You must install .NET to run this application.)')
        self.assertEqual(self.embedded((0, ' \n', '')), 'does not run (exit code 0)')
        for exc in (OSError('Exec format error'), subprocess.TimeoutExpired('dmsh_xlate', 60)):
            with self.subTest(exc=exc): self.assertTrue(self.embedded(exc).startswith('does not run ('))

    def check(self, xlate_source):
        with open(dmsh_chain.LOCK) as f: lock = dict(l.rstrip('\n').split('\t', 1) for l in f if '\t' in l and not l.startswith('#'))
        with mock.patch.object(tool_lock, 'tools_now', lambda: {**lock, 'xlate_source': xlate_source}): return tool_lock.check_tools()

    def test_checks_are_outside_the_chain(self):   # a fix to a check changes neither V nor any shader's cache key (dmsh_build.py CODE_FP takes dmsh_chain.py's code form)
        self.assertNotIn('tools/tool_lock.py', dmsh_chain.FILES)
        for name in ('tools_now', 'check_tools', 'xlate_embedded'): self.assertFalse(hasattr(dmsh_chain, name), name)

    def test_check_tools(self):
        bad = self.check('does not run (You must install .NET to run this application.)')
        self.assertEqual(len(bad), 1, bad); self.assertIn('the translator does not run (You must install .NET', bad[0]); self.assertIn('.NET 10 runtime', bad[0])
        bad = self.check('0123456789abcdef')   # a translator that runs but was built from other source: the lock line and the rebuild line
        self.assertEqual(len(bad), 2, bad); self.assertIn('xlate_source: lock', bad[0]); self.assertIn('rebuild dmsh_xlate', bad[1])


if __name__ == '__main__': unittest.main()

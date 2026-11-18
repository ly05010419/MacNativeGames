"""games/la/scripts/la_env.py: every build step gets zstd, spirv-cross and the .NET SDK from tools/deps (pinned, made once per
Mac; no Homebrew) first on PATH, and DOTNET_ROOT naming that SDK, so a published apphost (nx_extract, the shader translator)
finds its runtime (#2073: without DOTNET_ROOT, extract_la.py failed with "You must install .NET to run this application").
The caller's PATH and DOTNET_ROOT never reach the steps. tools/deps is stubbed: nothing is downloaded here."""
import importlib.util
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('la_env', ROOT / 'games/la/scripts/la_env.py')
la_env = importlib.util.module_from_spec(spec)
spec.loader.exec_module(la_env)


class PinnedTools(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.folders = [str(self.tmp / 'deps/zstd-1.5.7/bin'), str(self.tmp / 'deps/spirv-cross/bin'), str(self.tmp / 'deps/dotnet-sdk-10.0.401')]
        self.sdk = str(self.tmp / 'deps/dotnet-sdk-10.0.401')
        self.pinned = mock.patch.object(la_env, 'pinned_tools', return_value=(self.folders, self.sdk))
        self.pinned.start()
        self.addCleanup(mock.patch.stopall)

    def test_the_pinned_tools_come_first_then_only_the_system(self):
        env = la_env.environment(self.tmp / 'work', 'TEST')
        self.assertEqual(env['PATH'].split(os.pathsep), [*self.folders, *la_env.PATH])
        self.assertNotIn('/opt/homebrew/bin', env['PATH'])

    def test_dotnet_root_names_the_pinned_sdk(self):
        self.assertEqual(la_env.environment(self.tmp / 'work', 'TEST')['DOTNET_ROOT'], self.sdk)

    def test_the_callers_path_and_dotnet_root_are_not_used(self):
        with mock.patch.dict(os.environ, {'PATH': '/opt/homebrew/bin', 'DOTNET_ROOT': '/opt/homebrew/opt/dotnet/libexec'}):
            env = la_env.environment(self.tmp / 'work', 'TEST')
        self.assertEqual(env['DOTNET_ROOT'], self.sdk)
        self.assertNotIn('/opt/homebrew', env['PATH'])

    def test_a_tool_that_cannot_be_made_stops_the_build_with_the_reason(self):
        self.pinned.stop()
        sys.path.insert(0, str(ROOT))
        import tools.deps as deps
        with mock.patch.object(deps, 'path', side_effect=deps.DepsError('curl failed (exit 6)')), \
             self.assertRaisesRegex(ValueError, 'pinned source: curl failed'):
            la_env.environment(self.tmp / 'work', 'TEST')

    def test_it_asks_tools_deps_for_the_tools_the_steps_run(self):
        self.assertEqual(la_env.TOOLS, ('zstd', 'spirv-cross', 'dotnet'))


if __name__ == '__main__':
    unittest.main()

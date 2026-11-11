"""games/la/scripts/la_env.py (#2073): the steps get DOTNET_ROOT for the dotnet on their PATH, so a published apphost
(nx_extract) finds a Homebrew .NET. Without it extract_la.py failed with "You must install .NET to run this application"."""
import importlib.util
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('la_env', ROOT / 'games/la/scripts/la_env.py')
la_env = importlib.util.module_from_spec(spec)
spec.loader.exec_module(la_env)


class DotnetRoot(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.bin = self.tmp / 'homebrew/bin'
        self.bin.mkdir(parents=True)

    def command(self, path, text=''):
        path.write_text(text)
        path.chmod(0o755)
        return path

    def runtime(self, folder):
        (folder / 'host/fxr/10.0.0').mkdir(parents=True)
        return folder

    def test_link_to_libexec_dotnet(self):
        libexec = self.runtime(self.tmp / 'homebrew/Cellar/dotnet/10.0.103/libexec')
        self.command(libexec / 'dotnet')
        (self.bin / 'dotnet').symlink_to(libexec / 'dotnet')
        self.assertEqual(la_env.dotnet_root(('/nonexistent', str(self.bin))), str(libexec.resolve()))

    def test_script_beside_libexec(self):
        keg = self.tmp / 'homebrew/Cellar/dotnet/10.0.103'
        libexec = self.runtime(keg / 'libexec')
        (keg / 'bin').mkdir()
        self.command(keg / 'bin/dotnet', '#!/bin/bash\nexec "$DOTNET_ROOT/dotnet" "$@"\n')
        (self.bin / 'dotnet').symlink_to(keg / 'bin/dotnet')
        self.assertEqual(la_env.dotnet_root((str(self.bin),)), str(libexec.resolve()))

    def test_microsoft_install(self):
        share = self.runtime(self.tmp / 'share/dotnet')
        self.command(share / 'dotnet')
        self.assertEqual(la_env.dotnet_root((str(self.bin), str(share))), str(share.resolve()))

    def test_no_dotnet_or_no_runtime(self):
        self.assertIsNone(la_env.dotnet_root((str(self.bin),)))
        self.command(self.bin / 'dotnet')
        self.assertIsNone(la_env.dotnet_root((str(self.bin),)))

    def test_only_the_dotnet_the_steps_would_run(self):
        share = self.runtime(self.tmp / 'share/dotnet')
        self.command(share / 'dotnet')
        (self.bin / 'dotnet').write_text('')   # not executable: never run, so skipped
        self.assertEqual(la_env.dotnet_root((str(self.bin), str(share))), str(share.resolve()))
        self.command(self.bin / 'dotnet')      # executable without a runtime: it is the one run, so none is found
        self.assertIsNone(la_env.dotnet_root((str(self.bin), str(share))))

    def test_the_callers_path_and_dotnet_root_are_not_used(self):
        share = self.runtime(self.tmp / 'share/dotnet')
        self.command(share / 'dotnet')
        with mock.patch.dict(os.environ, {'PATH': str(share), 'DOTNET_ROOT': str(share)}), \
             mock.patch.object(la_env.dotnet_root, '__defaults__', ((str(self.tmp / 'empty'),),)):
            self.assertNotIn('DOTNET_ROOT', la_env.environment(self.tmp / 'work', 'TEST'))

    def test_environment_sets_it_only_when_found(self):
        with mock.patch.object(la_env, 'dotnet_root', return_value='/opt/homebrew/opt/dotnet/libexec'):
            self.assertEqual(la_env.environment(self.tmp / 'work', 'TEST')['DOTNET_ROOT'], '/opt/homebrew/opt/dotnet/libexec')
        with mock.patch.object(la_env, 'dotnet_root', return_value=None):
            self.assertNotIn('DOTNET_ROOT', la_env.environment(self.tmp / 'work', 'TEST'))

if __name__ == '__main__':
    unittest.main()

"""Compaction and the following start use fresh processes and disk-built shaders."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(__name__ == '__main__', 'run test_pso_archive.py with the native t_pso_archive path')
class ArchiveRestartTests(unittest.TestCase):
    def test_compaction_with_fresh_bases_and_restart(self):
        for constants in (False, True):
            with self.subTest(constants=constants), tempfile.TemporaryDirectory(prefix='pso-restart-') as folder:
                env = dict(os.environ, DM_PSO_MTL4='1', MTL_DEBUG_LAYER='1', DM_TEST_ARCHIVE_MIXED='1')
                for name in ('DM_GPU_CAPTURE', 'MTL_CAPTURE_ENABLED', 'DM_TEST_ARCHIVE_CONSTANTS'):
                    env.pop(name, None)
                if constants:
                    env['DM_TEST_ARCHIVE_CONSTANTS'] = '1'
                for phase, files in (('seed', 9), ('replay', 1), ('verify', 1)):
                    result = subprocess.run([executable, '--compact-' + phase, folder],
                                            env=env, capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, phase + '\n' + result.stdout + result.stderr)
                    self.assertEqual(len(list(Path(folder).glob('*.mtl4a'))), files, phase)
                    self.assertEqual((Path(folder) / 'compacting').exists(), phase == 'seed', phase)


if __name__ == '__main__':
    executable = sys.argv.pop(1)
    unittest.main()

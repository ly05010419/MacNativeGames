"""Lossless recorder compression across all shared NVN configurations."""
from pathlib import Path
import shutil
import tempfile
import unittest
import test_nvn_semantics as semantics


@unittest.skipUnless(shutil.which('xcrun'), 'requires Apple native toolchain')
class RecordCompact(unittest.TestCase):
    def test_published_commands_and_hardware_budget(self):
        original = semantics.PROBE
        semantics.PROBE = Path(__file__).with_name('record_compact_probe.c')
        try:
            for name, defines in semantics.CONFIGS.items():
                with self.subTest(game=name), tempfile.TemporaryDirectory() as directory:
                    exe = semantics.build(semantics.TOTK_GFX, defines, Path(directory))
                    outputs = []
                    for mode in ('0', '1'):
                        result = semantics.run([str(exe), mode], timeout=20)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        outputs.append(tuple(map(int, result.stdout.strip().splitlines()[-1].split())))
                    old, new = outputs
                    self.assertEqual(old[1:], new[1:], 'canonical bytes, metadata and guest budget must be unchanged')
                    self.assertLess(new[0], old[0] * .6, 'recorder scratch must shrink')
                    print(name, 'scratch', old[0], '->', new[0], 'canonical/hardware/hash', new[1:])
        finally:
            semantics.PROBE = original


if __name__ == '__main__':
    unittest.main()

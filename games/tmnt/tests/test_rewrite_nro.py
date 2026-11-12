import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import rewrite_nro
from rewrite_modules import OUTPUTS


class NroRewriteTests(unittest.TestCase):
    def test_input_and_complete_output_are_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            prepared = root / 'prepared'
            prepared.mkdir()
            for name in ('module.nro', 'synthetic.nso'):
                (prepared / name).write_bytes(name.encode())
            (prepared / 'manifest.json').write_text(json.dumps({'status': 'pass',
                'nro_sha256': hashlib.sha256(b'module.nro').hexdigest(),
                'synthetic_nso_sha256': hashlib.sha256(b'synthetic.nso').hexdigest()}))
            def run_incomplete(cmd, **kwargs):
                for suffix in OUTPUTS[1:]:
                    (Path(cmd[3]) / f'nro.{suffix}').write_bytes(b'test')
                return SimpleNamespace(returncode=0)
            with patch.object(rewrite_nro.subprocess, 'run', side_effect=run_incomplete):
                self.assertEqual(rewrite_nro.rewrite(prepared, root / 'out'), 1)
            self.assertEqual(json.loads((root / 'out/report.json').read_text())['exit_code'], 1)
            with self.assertRaisesRegex(ValueError, 'failed'):
                rewrite_nro.verify_nro_report(prepared, root / 'out')
            def run_complete(cmd, **kwargs):
                for suffix in OUTPUTS:
                    (Path(cmd[3]) / f'nro.{suffix}').write_bytes(b'test')
                return SimpleNamespace(returncode=0)
            with patch.object(rewrite_nro.subprocess, 'run', side_effect=run_complete):
                self.assertEqual(rewrite_nro.rewrite(prepared, root / 'complete'), 0)
            rewrite_nro.verify_nro_report(prepared, root / 'complete')
            (root / 'complete/nro.patched.bin').write_bytes(b'tampered')
            with self.assertRaisesRegex(ValueError, 'outputs differ'):
                rewrite_nro.verify_nro_report(prepared, root / 'complete')
            (prepared / 'module.nro').write_bytes(b'tampered')
            with self.assertRaisesRegex(ValueError, 'differs'):
                rewrite_nro.rewrite(prepared, root / 'other')
            self.assertFalse((root / 'other').exists())


if __name__ == '__main__':
    unittest.main()

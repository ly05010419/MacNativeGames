"""Small synthetic inputs exercise the signed NRO section generator."""
import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / 'tools/gen_nro_embed.py'
SPEC = importlib.util.spec_from_file_location('gen_nro_embed', TOOL)
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


def sha(data):
    return hashlib.sha256(data).hexdigest()


class NroEmbedTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prepared = self.root / 'prepared'
        self.rewritten = self.root / 'rewritten'
        self.prepared.mkdir()
        self.rewritten.mkdir()
        nro = b'N' * 0x8000
        nso = b'S' * 0x8100
        (self.prepared / 'module.nro').write_bytes(nro)
        (self.prepared / 'synthetic.nso').write_bytes(nso)
        (self.prepared / 'manifest.json').write_text(json.dumps({
            'status': 'pass', 'nro_sha256': sha(nro),
            'synthetic_nso_sha256': sha(nso)}))
        outputs = {
            'patched.bin': b'X' * 0x4000 + b'D' * 0x4000 + b'K' * 0x4000 + b'P' * 0x4000,
            'fixups.tsv': (b'# TEXT_END\t0x4000\n# DATA_OFF\t0x4000\n'
                           b'# KEY_PAGE_OFF\t0x8000\n# PATCH_OFF\t0xc000\n'),
            'gates.tsv': b'',
        }
        for name, data in outputs.items():
            (self.rewritten / f'nro.{name}').write_bytes(data)
        self.report = {
            'input_sha256': {'nro': sha(nro), 'nso': sha(nso)},
            'exit_code': 0,
            'options': {'ruleset': 'native', 'private_gates': False},
            'outputs': {name: sha(data) for name, data in outputs.items()},
        }
        self.save_report()

    def save_report(self):
        (self.rewritten / 'report.json').write_text(json.dumps(self.report))

    def test_signed_sections_and_fingerprints(self):
        output = self.root / 'embed.S'
        GENERATOR.generate(self.prepared, self.rewritten, output)
        assembly = output.read_text()
        self.assertIn('__TMNTNROX,__nrotext', assembly)
        self.assertIn('__TMNTNROW,__image', assembly)
        self.assertIn('__TMNTNROX,__nropatch', assembly)
        header = (self.root / 'ios_nro_embed.h').read_text()
        self.assertIn(sha(b'X' * 0x4000), header)
        self.assertIn(sha(b'D' * 0x4000 + b'K' * 0x4000), header)
        self.assertIn(sha(b'P' * 0x4000), header)

    def test_modified_rewrite_rejected_before_output(self):
        (self.rewritten / 'nro.patched.bin').write_bytes(b'Z' * 0x10000)
        output = self.root / 'embed.S'
        with self.assertRaisesRegex(ValueError, 'differs'):
            GENERATOR.generate(self.prepared, self.rewritten, output)
        self.assertFalse(output.exists())

    def test_host_gates_rejected(self):
        (self.rewritten / 'nro.gates.tsv').write_bytes(b'gate\n')
        self.report['outputs']['gates.tsv'] = sha(b'gate\n')
        self.save_report()
        with self.assertRaisesRegex(ValueError, 'host gates'):
            GENERATOR.generate(self.prepared, self.rewritten, self.root / 'embed.S')

    def test_repository_output_rejected(self):
        with self.assertRaisesRegex(ValueError, 'outside the repository'):
            GENERATOR.generate(self.prepared, self.rewritten, TOOL.parent / 'private.S')


if __name__ == '__main__':
    unittest.main()

import struct
import json
import hashlib
import importlib.metadata
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from module_metadata import parse_metadata, validate_layout, verify_rewrite_report
from rewrite_modules import GAME, MODULES, OUTPUTS, MAIN_RULE_OUTPUT, TITLE, TOOL_SOURCES


def sample():
    data = bytearray(0x400)
    struct.pack_into('<I', data, 4, 0x40)
    data[0x40:0x44] = b'MOD0'
    struct.pack_into('<i', data, 0x44, 0x40)
    tags = [(4, 0x200), (5, 0x240), (6, 0x280), (10, 7), (11, 24),
            (7, 0x300), (8, 48), (9, 24), (0, 0)]
    for index, (tag, value) in enumerate(tags):
        struct.pack_into('<qQ', data, 0x80 + index * 16, tag, value)
    struct.pack_into('<II', data, 0x200, 1, 2)
    data[0x240:0x247] = b'\0value\0'
    struct.pack_into('<IBBHQQ', data, 0x280 + 24, 1, 0x20, 0, 0, 0, 0)
    struct.pack_into('<QQq', data, 0x300, 0x380, 1027, 0x20)
    struct.pack_into('<QQq', data, 0x318, 0x388, (1 << 32) | 1026, 0)
    return data


class MetadataTests(unittest.TestCase):
    def test_layout_matches_original_data_and_patch_extent(self):
        layout = {'TEXT_END': 0x100, 'RO_OFF': 0x200, 'DATA_OFF': 0x300,
                  'KEY_PAGE_OFF': 0x4000, 'IMAGE_SIZE': 0x8000, 'PATCH_OFF': 0x8000,
                  'PATCH_SIZE': 0x20, 'PATCH_LEAD': 0}
        validate_layout(layout, 0x300, 0x4000, 0x8020)
        with self.assertRaisesRegex(ValueError, 'segments'):
            validate_layout(layout, 0x301, 0x4000, 0x8020)
        with self.assertRaisesRegex(ValueError, 'patch layout'):
            validate_layout(layout, 0x300, 0x4000, 0x8010)
        layout['PATCH_LEAD'] = 0x4000
        with self.assertRaisesRegex(ValueError, 'unsupported'):
            validate_layout(layout, 0x300, 0x4000, 0x8020)

    def test_rewrite_report_refuses_failures_and_tampering(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            exefs, rewritten = root / 'exefs', root / 'rewritten'
            exefs.mkdir()
            rewritten.mkdir()
            records, modules = [], {}
            for name in (*MODULES, 'main.npdm'):
                data = name.encode()
                (exefs / name).write_bytes(data)
                digest = hashlib.sha256(data).hexdigest()
                records.append({'name': name, 'sha256': digest})
                if name in MODULES:
                    outputs = {}
                    for suffix in (*OUTPUTS, *((MAIN_RULE_OUTPUT,) if name == 'main' else ())):
                        value = (name + suffix).encode()
                        (rewritten / f'{name}.{suffix}').write_bytes(value)
                        outputs[suffix] = hashlib.sha256(value).hexdigest()
                    modules[name] = {'input_sha256': digest, 'exit_code': 0, 'outputs': outputs}
            (root / 'extraction-manifest.json').write_text(json.dumps({'program': {'title_id': TITLE}, 'files': records}))
            tools = GAME / 'third_party/d2r'
            report = {'title_id': TITLE, 'options': {'ruleset': 'native', 'private_gates': False,
                                                    'tmnt_graphics_gates': True},
                      'dependencies': {name: importlib.metadata.version(name) for name in ('capstone', 'lz4')},
                      'python': sys.version.split()[0],
                      'tools': {name: hashlib.sha256((tools / name).read_bytes()).hexdigest() for name in TOOL_SOURCES},
                      'modules': modules}
            report_path = rewritten / 'report.json'
            report_path.write_text(json.dumps(report))
            synthetic_hashes = {row['name']: row['sha256'] for row in records}
            with patch('module_metadata.verify_exefs', return_value=synthetic_hashes):
                verify_rewrite_report(exefs, rewritten)
                (rewritten / 'main.patched.bin').write_bytes(b'tampered')
                with self.assertRaisesRegex(ValueError, 'output hash mismatch'):
                    verify_rewrite_report(exefs, rewritten)
                (rewritten / 'main.patched.bin').write_bytes(b'mainpatched.bin')
                report['modules']['subsdk1']['exit_code'] = 1
                report_path.write_text(json.dumps(report))
                with self.assertRaisesRegex(ValueError, 'rewrite is missing, failed'):
                    verify_rewrite_report(exefs, rewritten)

    def test_records_symbol_indices_and_weak_binding(self):
        result = parse_metadata(Path('unused'), sample(), 0x380, 0x400)
        self.assertEqual(result['symbols'][1]['binding'], 2)
        self.assertFalse(result['symbols'][1]['defined'])
        self.assertEqual(result['relocations'][1]['symbol'], 1)
        self.assertEqual(result['relocations'][0]['kind'], 1027)

    def test_rejects_bad_symbol_and_overlapping_relocations(self):
        data = sample()
        struct.pack_into('<Q', data, 0x320, (2 << 32) | 1026)
        with self.assertRaisesRegex(ValueError, 'symbol index'):
            parse_metadata(Path('unused'), data, 0x380, 0x400)
        data = sample()
        struct.pack_into('<Q', data, 0x318, 0x384)
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            parse_metadata(Path('unused'), data, 0x380, 0x400)


if __name__ == '__main__':
    unittest.main()

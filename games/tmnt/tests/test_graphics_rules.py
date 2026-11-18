"""Original-input and collision guards; synthetic data contains no game image."""
import hashlib
import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'third_party/d2r'))
import tmnt_graphics_rules as rules


def fixture():
    raw = bytearray(0x50)
    raw[:4] = b'NSO0'
    raw[0x40:0x50] = bytes.fromhex(rules.MAIN_BUILD_ID)
    image = bytearray(max(offset for offset, _ in rules.SITE_WORDS) + 4)
    for offset, word in rules.SITE_WORDS:
        struct.pack_into('<I', image, offset, word)
    return bytes(raw), image


class GraphicsRuleTests(unittest.TestCase):
    def test_every_gate_preserves_original_operation_and_present_paths(self):
        expected = {
            0xfd34c: [0x90, 0x95], 0xfd28c: [0x96, 0x91],
            0xfdf70: [0x92], 0xfdf74: [0x93],
            0x8a8920: [0x98], 0x8a9034: [0x97],
            0x8a9060: [0x99],
        }
        for site, word in rules.SITE_WORDS:
            emitted = []
            gates = []
            def gate(at, op):
                self.assertEqual(at, site)
                gates.append(op)
            def emit(value): emitted.append(value)
            def branch(source, target): return ('B', source, target)
            def here(): return 0x1000000 + 4 * len(emitted)
            rules.emit_site_plan(site, word, gate, emit, here, branch)
            self.assertEqual(gates, expected[site])
            if site == 0xfdf74:
                self.assertEqual(emitted[0], 0x34000049)
                self.assertEqual([x[2] for x in emitted[1:]],
                                 [0xfdf78, 0xfdff8])
            else:
                self.assertEqual(emitted[-1][2], site + 4)
                self.assertEqual(word in emitted, site != 0x8a8920)

    def test_supported_input_and_exact_words(self):
        raw, image = fixture()
        with patch.object(rules, 'MAIN_SHA256', hashlib.sha256(raw).hexdigest()):
            self.assertTrue(rules.preflight(raw, image, 'main', True))
            rules.require_clear_sites(image, len(image), set(), [])
            self.assertFalse(rules.preflight(raw, image, 'sdk', True))
            self.assertFalse(rules.preflight(raw, image, 'main', False))
            image[rules.SITE_WORDS[0][0]] ^= 1
            with self.assertRaisesRegex(ValueError, 'instruction mismatch'):
                rules.preflight(raw, image, 'main', True)

    def test_version_hash_and_collision_fail_closed(self):
        raw, image = fixture()
        with self.assertRaisesRegex(ValueError, 'input hash'):
            rules.preflight(raw, image, 'main', True)
        with patch.object(rules, 'MAIN_SHA256', hashlib.sha256(raw).hexdigest()):
            bad = bytearray(raw); bad[0x40] ^= 1
            with self.assertRaisesRegex(ValueError, 'build ID'):
                rules.preflight(bad, image, 'main', True)
        offset = rules.SITE_WORDS[1][0]
        with self.assertRaisesRegex(ValueError, 'collides'):
            rules.require_clear_sites(image, len(image), {offset}, [])
        with self.assertRaisesRegex(ValueError, 'collides'):
            rules.require_clear_sites(image, len(image), set(), [(offset, offset + 4)])
        with self.assertRaisesRegex(ValueError, 'outside text'):
            rules.require_clear_sites(image, offset, set(), [])


if __name__ == '__main__':
    unittest.main()

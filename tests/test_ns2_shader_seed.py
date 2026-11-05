"""Changing the seed request producer or translator must reject stale translations."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1] / 'games/totk/scripts'
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location('prepare_ns2_seed', SCRIPTS / 'prepare_ns2_seed.py')
seed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seed)


class TranslationContext(unittest.TestCase):
    def test_changed_request_producer_or_translator_rejects_old_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary, translator = Path(tmp) / 'runtime', Path(tmp) / 'translator'
            binary.write_bytes(b'near=0')
            translator.write_bytes(b'translator-v1')
            old = seed.translation_context(binary, translator, 'minus-w')
            binary.write_bytes(b'near=-w')
            corrected = seed.translation_context(binary, translator, 'minus-w')
            self.assertNotEqual(old, corrected)
            self.assertNotEqual(corrected, seed.translation_context(binary, translator, 'zero'))
            self.assertEqual(corrected, seed.translation_context(binary, translator, 'minus-w'))
            translator.write_bytes(b'translator-v2')
            self.assertNotEqual(corrected, seed.translation_context(binary, translator, 'minus-w'))
            copy = Path(tmp) / 'other-runtime-path'
            copy.write_bytes(binary.read_bytes())
            self.assertEqual(seed.translation_context(copy, translator, 'minus-w'), seed.translation_context(binary, translator, 'minus-w'))


if __name__ == '__main__':
    unittest.main()

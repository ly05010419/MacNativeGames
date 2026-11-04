"""XC3's real shader readers and translation cache provenance, using synthetic inputs."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('xc3_shader_cases', ROOT / 'games/xc3/tests/test_xc3_shaders.py')
cases = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cases)
KeyTests, NameTests, ContainerTests = cases.KeyTests, cases.NameTests, cases.ContainerTests
ts = cases.ts


class Provenance(unittest.TestCase):
    def test_translation_requires_chain_stage_and_nonempty_pair(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            metal, reflection = out / 'v_b.metal', out / 'v_b.json'
            metal.write_text('shader')
            for data, expected in (({'chain': 'current', 'stage': 'vs'}, True),
                                   ({'chain': 'old', 'stage': 'vs'}, False),
                                   ({'chain': 'current', 'stage': 'ps'}, False), ([], False)):
                reflection.write_text(json.dumps(data))
                self.assertEqual(ts.done(out, 'v', 'current', 'vs'), expected)
            reflection.write_text('{partial')
            self.assertFalse(ts.done(out, 'v', 'current', 'vs'))
            reflection.write_text('{"chain":"current","stage":"vs"}')
            metal.write_text('')
            self.assertFalse(ts.done(out, 'v', 'current', 'vs'))

    def test_changed_tools_retire_cache_without_deleting_old_results(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            translator, cross = root / 'translator', root / 'cross'
            translator.write_bytes(b'tool 1'); cross.write_bytes(b'cross 1')
            out = root / 'prepared'
            ts.prepare_output(out, translator, cross, 'chain')
            (out / 'keep.metal').write_text('old translation')
            ts.prepare_output(out, translator, cross, 'chain')
            self.assertTrue((out / 'keep.metal').exists())
            translator.write_bytes(b'tool 2')
            ts.prepare_output(out, translator, cross, 'chain')
            self.assertFalse((out / 'keep.metal').exists())
            retired = list(root.glob('prepared.stale-*'))
            self.assertEqual(len(retired), 1)
            self.assertEqual((retired[0] / 'keep.metal').read_text(), 'old translation')
            self.assertEqual(json.loads((out / 'translation-build.json').read_text())['chain'], 'chain')

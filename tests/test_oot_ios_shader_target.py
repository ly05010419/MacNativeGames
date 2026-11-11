"""Ocarina of Time's iOS / visionOS shader libraries target the app's lowest OS, not the compiler's SDK."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'games/oot/ios/tools'
spec = importlib.util.spec_from_file_location('oot_compile_seed', TOOLS / 'compile_seed.py')
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)


class OotShaderTarget(unittest.TestCase):
    def test_flags_follow_the_platform_and_the_project(self):
        self.assertEqual(compiler.target_flags('iphoneos'), ['-mtargetos=ios26.0'])
        self.assertEqual(compiler.target_flags('xros'), ['-mtargetos=xros26.0'])
        with self.assertRaises(KeyError):
            compiler.target_flags('macosx')
        project = (TOOLS.parent / 'project.yml').read_text()
        self.assertIn(f'    iOS: "{compiler.MIN_OS}"', project)
        self.assertIn(f'IPHONEOS_DEPLOYMENT_TARGET: "{compiler.MIN_OS}"', project)
        for tool in ('fill_shaders.py', 'device_pipelines.py'):
            self.assertIn('target_flags(', (TOOLS / tool).read_text(), tool)

    def test_compile_and_manifest_carry_the_target(self):
        for sdk in ('iphoneos', 'xros'):
            with self.subTest(sdk=sdk), tempfile.TemporaryDirectory() as tmp:
                seed = Path(tmp) / 'seed'; seed.mkdir()
                name = '0' * 64 + '_b.metal'
                (seed / name).write_bytes(b'one shader')
                (seed / 'seed-manifest.json').write_text(json.dumps({'chain': 'test', 'files': {name: hashlib.sha256(b'one shader').hexdigest()}}))
                compiles = []
                def run(args, **kwargs):
                    if '--version' in args:
                        return SimpleNamespace(stdout='test Metal compiler')
                    if '-c' in args:
                        compiles.append(args)
                    Path(args[args.index('-o') + 1]).write_bytes(b'compiled library')
                    return SimpleNamespace(returncode=0)
                with patch.object(sys, 'argv', ['compile_seed.py', str(seed), sdk]), patch.object(compiler.subprocess, 'run', side_effect=run):
                    compiler.main()
                self.assertEqual(len(compiles), 1)
                self.assertIn(compiler.target_flags(sdk)[0], compiles[0])
                self.assertEqual(json.loads((seed / 'seed-manifest.json').read_text())['ios_metal_flags'], compiler.seed_flags(sdk))


if __name__ == '__main__':
    unittest.main()

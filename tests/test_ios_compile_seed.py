"""Identical shader variants share one compile, retain every output name and remain reusable."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'games/totk/ios/tools'
spec = importlib.util.spec_from_file_location('compile_seed', TOOLS / 'compile_seed.py')
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)


class CompileSeed(unittest.TestCase):
    def test_duplicate_sources_compile_once_and_all_aliases_reuse_the_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            seed = Path(tmp) / 'seed'; seed.mkdir()
            files = {}
            for i in range(20):
                name = f'{i:064x}_b.metal'
                data = b'one shader' if i < 19 else b'another shader'
                (seed / name).write_bytes(data)
                files[name] = hashlib.sha256(data).hexdigest()
            (seed / 'seed-manifest.json').write_text(json.dumps({'chain': 'test', 'files': files}))
            calls = []
            def run(args, **kwargs):
                if '--version' in args:
                    return SimpleNamespace(stdout='test Metal compiler')
                if '-c' in args:
                    calls.append(args[args.index('-c') + 1])
                Path(args[args.index('-o') + 1]).write_bytes(b'compiled library')
                return SimpleNamespace(returncode=0)
            with patch.object(sys, 'argv', ['compile_seed.py', str(seed), 'iphoneos']), patch.object(compiler.subprocess, 'run', side_effect=run):
                compiler.main()
                self.assertEqual(len(calls), 2)
                manifest = json.loads((seed / 'seed-manifest.json').read_text())
                self.assertEqual(manifest['ios_metallibs'], 20)
                for name in files:
                    library = name.removesuffix('.metal') + '.metallib'
                    self.assertEqual(hashlib.sha256((seed / library).read_bytes()).hexdigest(), manifest['files'][library])
                calls.clear()
                compiler.main()
                self.assertEqual(calls, [])


if __name__ == '__main__':
    unittest.main()

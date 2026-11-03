"""Patched shader variants the game compiled from rewritten source while it was played (kept by the runtime as
variants/<SHA-256>.metal in the shader cache) are compiled ahead with the startup shaders and go into the app."""
import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch
import build_app


class VariantLibs(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.tree, self.logs = self.root/'tree', self.root/'logs'
        seed = self.tree/'build/shader-seed/v143'; seed.mkdir(parents=True)
        (seed/'a_b.metal').write_text('// startup shader')
        self.variants = self.root/'variants'; self.variants.mkdir()
        self.good = self.put('// patched variant')
        (self.variants/('0'*64+'.metal')).write_text('// a name that is not its digest')   # skipped
        compiler = self.root/'metal'   # copies the source to the output, as a stand-in for the Metal compiler
        compiler.write_text('#!/bin/sh\nwhile [ "$1" != "-o" ]; do shift; done\ncp "$3" "$2"\n')
        compiler.chmod(compiler.stat().st_mode | stat.S_IEXEC)
        self.compiler = compiler

    def put(self, text):
        digest = hashlib.sha256(text.encode()).hexdigest()
        (self.variants/f'{digest}.metal').write_text(text)
        return digest

    def build(self):
        with patch.object(build_app, 'metal_compiler', return_value=(str(self.compiler), 'fake 1', dict(os.environ))), \
                patch.object(build_app, 'variant_folder', return_value=self.variants), patch.object(build_app, 'say'):
            build_app.step_shader_libs(self.tree, self.logs, version='v143')

    def test_variants_are_compiled_with_the_startup_shaders_and_listed(self):
        self.build()
        libs = self.tree/'build/shader-libs'
        self.assertTrue((libs/f'{self.good}.metallib').is_file())
        self.assertEqual((libs/f'{self.good}.metallib').read_text(), '// patched variant')
        self.assertTrue((libs/(hashlib.sha256(b'// startup shader').hexdigest()+'.metallib')).is_file())
        self.assertFalse((libs/('0'*64+'.metallib')).exists())
        self.assertEqual(json.loads((libs/'variants-v143.json').read_text()), [self.good])

    def test_the_app_takes_the_listed_variant_libraries(self):
        self.build()   # what the app's shader data bundle carries (build_app.shader_bundle): the startup shaders' and the variants'
        startup = hashlib.sha256(b'// startup shader').hexdigest()
        libs = build_app.shader_libs_of(self.tree, 'v143', {'files': {'a_b.metal': startup}})
        self.assertEqual([p.name for p in libs], [f'{startup}.metallib', f'{self.good}.metallib'])

    def test_no_recorded_variants(self):
        for p in self.variants.iterdir():
            p.unlink()
        self.build()
        self.assertEqual(json.loads((self.tree/'build/shader-libs/variants-v143.json').read_text()), [])


if __name__ == '__main__':
    unittest.main()

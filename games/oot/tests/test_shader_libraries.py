"""A default-only rebuild must preserve verified libraries of recorded bindings."""
import contextlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

GAME=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('oot_libraries',GAME/'scripts/build_shader_libs.py')
libraries=importlib.util.module_from_spec(spec);spec.loader.exec_module(libraries)


class LibraryTests(unittest.TestCase):
    def test_default_only_rebuild_keeps_verified_recorded_library(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);cache=root/'cache';cache.mkdir();out=root/'libs';out.mkdir()
            source=cache/'fixture_b.metal';source.write_text('kernel void main0() {}')
            base=out/(libraries.digest(source)+'.metallib');base.write_bytes(b'default fixture')
            recorded=out/('a'*64+'.metallib');recorded.write_bytes(b'recorded binding fixture')
            damaged=out/('b'*64+'.metallib');damaged.write_bytes(b'changed after verification')
            identity=' '.join(['fixture',*libraries.METAL_FLAGS])
            (out/'compiler.txt').write_text(identity)
            expected={p.name:libraries.digest(p)for p in (base,recorded)}
            (out/'libraries-manifest.json').write_text(json.dumps(dict(
                compiler=identity,files={**expected,damaged.name:'0'*64})))
            with mock.patch.object(sys,'argv',['build_shader_libs.py','--cache',str(cache),'--out',str(out)]), \
                 mock.patch.object(libraries,'metal_compiler',return_value=('unused','fixture',{})), \
                 mock.patch.object(libraries,'GameSlot',return_value=contextlib.nullcontext()), \
                 mock.patch.object(libraries.subprocess,'run') as compile_call:
                with self.assertRaises(SystemExit) as result:
                    libraries.main()
                self.assertEqual(result.exception.code,0)
                compile_call.assert_not_called()
            manifest=json.loads((out/'libraries-manifest.json').read_text())
            self.assertTrue(manifest['complete'])
            self.assertEqual(manifest['files'],expected)

    def test_an_update_that_compiles_nothing_reads_no_library(self):
        """2026-11-16 (user: "极速更新 还是 太慢"): with nothing to compile, every library was read again for the manifest (most
        of the step's 41 s). A second run over unchanged libraries now reads none and writes the same manifest."""
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);cache=root/'cache';cache.mkdir();out=root/'libs';out.mkdir()
            source=cache/'fixture_b.metal';source.write_text('kernel void main0() {}')
            base=out/(libraries.digest(source)+'.metallib');base.write_bytes(b'default fixture')
            recorded=out/('a'*64+'.metallib');recorded.write_bytes(b'recorded binding fixture')
            identity=' '.join(['fixture',*libraries.METAL_FLAGS])
            (out/'compiler.txt').write_text(identity)
            expected={p.name:libraries.digest(p)for p in (base,recorded)}
            (out/'libraries-manifest.json').write_text(json.dumps(dict(compiler=identity,files=expected)))
            def run():
                with mock.patch.object(sys,'argv',['build_shader_libs.py','--cache',str(cache),'--out',str(out)]), \
                     mock.patch.object(libraries,'metal_compiler',return_value=('unused','fixture',{})), \
                     mock.patch.object(libraries,'GameSlot',return_value=contextlib.nullcontext()), \
                     mock.patch.object(libraries.subprocess,'run') as compile_call, \
                     mock.patch.object(libraries,'digest',side_effect=libraries.digest) as read:
                    with self.assertRaises(SystemExit) as result:
                        libraries.main()
                    self.assertEqual(result.exception.code,0)
                    compile_call.assert_not_called()
                    return [c.args[0].name for c in read.call_args_list if c.args[0].suffix=='.metallib']
            run()   # the first run checks them and records them (libraries-verified.json)
            self.assertEqual(run(),[])   # the second reads none of them
            self.assertEqual(json.loads((out/'libraries-manifest.json').read_text())['files'],expected)


if __name__=='__main__':
    unittest.main()

"""Ocarina of Time's runtime is rebuilt when its sources' content changes, whatever the file times, and a runtime of another
translation chain than the seed's is refused before the pipeline archive.

2026-11-19: the nxport work folder held objects of a v29 checkout (03:24-03:36); an update from the Launcher's bundled
fab08820 (v28, files dated 03:13) ran make, which said "Nothing to be done", so the v29 program stayed while the seed was
re-made as v28. The pipeline-archive tool refused the seed ("Startup shader seed identity is invalid") and all 29,124
pipelines failed. These tests run games/oot/build.py's own functions.
"""
import importlib.util
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'


def load_build():
    spec = importlib.util.spec_from_file_location('oot_build_runtime_rebuild', GAME / 'build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RuntimeRebuild(unittest.TestCase):
    def setUp(self):
        self.build = load_build()
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.game = base / 'game'
        (self.game / 'runtime').mkdir(parents=True)
        (self.game / 'tests').mkdir()
        shared = base / 'shared/nvn'
        shared.mkdir(parents=True)
        (shared / 'dmsh_chain.h').write_text('#define DMSH_CHAIN "oot-native-v29"\n')
        (self.game / 'third_party').symlink_to(base / 'shared')
        (self.game / 'runtime/frontend.m').write_text('int a;\n')
        (self.game / 'Makefile').write_text('runtime:\n')
        (self.game / 'tests/t.c').write_text('int t;\n')
        self.work = base / 'work'
        (self.work / 'build').mkdir(parents=True)
        self.calls = []

    def tearDown(self):
        self.tmp.cleanup()

    def make(self):
        def run(cmd, **_):
            self.calls.append(cmd)
        with mock.patch.object(self.build, 'GAME', self.game), mock.patch.object(self.build.subprocess, 'run', run), \
                mock.patch.object(self.build, 'make_path', lambda work, name: str(work)):
            self.build.build_runtime(self.work, self.work / 'native-runtime.log', 4, {})
        return self.calls[-1]

    def test_older_source_of_other_content_rebuilds(self):
        self.assertIn('-B', self.make())            # no record of the last build's sources
        self.assertNotIn('-B', self.make())         # the same sources: make decides by times as before
        header = self.game / 'third_party/nvn/dmsh_chain.h'
        header.write_text('#define DMSH_CHAIN "oot-native-v28"\n')   # an older checkout's file (through the link) ...
        os.utime(header, (1, 1))                    # ... dated before every object, as the Launcher's 03:13 sources
        self.assertIn('-B', self.make())

    def test_files_make_never_reads_do_not_rebuild(self):
        self.make()
        (self.game / 'tests/t.c').write_text('int changed;\n')
        (self.game / 'runtime/notes.txt').write_text('not a source\n')
        self.assertNotIn('-B', self.make())

    def test_digest_is_by_content(self):
        before = self.build.runtime_source_digest(self.game)
        (self.game / 'runtime/frontend.m').write_text('int b;\n')
        self.assertNotEqual(before, self.build.runtime_source_digest(self.game))


class RuntimeChainCheck(unittest.TestCase):
    def setUp(self):
        self.build = load_build()
        self.tmp = tempfile.TemporaryDirectory()
        self.program = Path(self.tmp.name) / 'oot-native'
        self.program.write_bytes(b'\0\0MNG_SHADER_CHAIN=oot-native-v29-mesh-blocks-mulext\0more')

    def tearDown(self):
        self.tmp.cleanup()

    def test_reads_the_mark(self):
        self.assertEqual(self.build.binary_chain(self.program), 'oot-native-v29-mesh-blocks-mulext')

    def test_other_chain_is_refused(self):
        with self.assertRaises(ValueError) as e:
            self.build.check_runtime_chain(self.program, 'oot-native-v28-mesh-primitive-blocks')
        self.assertIn('v29-mesh-blocks-mulext', str(e.exception))
        self.assertIn('v28-mesh-primitive-blocks', str(e.exception))

    def test_same_chain_passes(self):
        self.build.check_runtime_chain(self.program, 'oot-native-v29-mesh-blocks-mulext')

    def test_archive_checks_before_building(self):
        source = (GAME / 'build.py').read_text()
        body = source.split('def compute_pipeline_archive', 1)[1]
        self.assertLess(body.index('check_runtime_chain('), body.index("'pipeline-archive'"))
        self.assertEqual(source.count("['make','-C'"), 1)   # every runtime build goes through build_runtime


if __name__ == '__main__':
    unittest.main()

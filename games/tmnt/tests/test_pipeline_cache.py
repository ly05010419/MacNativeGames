"""Archive rebuilding checks without Metal or private game bytes."""
import contextlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))


class PipelineCacheTests(unittest.TestCase):
    def module(self):
        spec = importlib.util.spec_from_file_location('_pipeline_cache_subject', SCRIPTS / 'tmnt_pipelines.py')
        module = importlib.util.module_from_spec(spec)
        # Only the external native runner is substituted, never archive logic.
        stub = types.SimpleNamespace(run_outside_bundle=lambda *args: contextlib.nullcontext(args[0]))
        with mock.patch.dict(sys.modules, {'build_app': stub}):
            spec.loader.exec_module(module)
        return module

    def build(self, root, incomplete=False):
        module = self.module(); build = root / 'build'; build.mkdir()
        stale = build / 'pipeline-archive'; stale.mkdir(); (stale / 'obsolete.metalarchive').write_text('old')
        cache = build / 'mac-pipelines/0/cache/pipelines'; cache.mkdir(parents=True)
        (cache / 'obsolete.metalarchive').write_text('old')
        def run(argv, **kwargs):
            if 'resolve' in argv:
                (build / 'pso_manifest.tsv').write_text('R\tfixture\n')
                (build / 'pso_title.tsv').write_text('')
                return subprocess.CompletedProcess(argv, 0)
            output = Path(kwargs['env']['TOTK_SHADER_CACHE']) / 'pipelines'
            self.assertFalse((output / 'obsolete.metalarchive').exists())
            output.mkdir(parents=True, exist_ok=True)
            (output / 'current.metalarchive').write_text('archive')
            report = {'render': 0 if incomplete else 1, 'compute': 0, 'write_failed': 0}
            return subprocess.CompletedProcess(argv, 0, json.dumps(report), '')
        with mock.patch.object(module.subprocess, 'run', side_effect=run):
            archive = module.mac_pipelines(root / 'native', root / 'rewritten', build)
        return module, build, archive

    def test_rebuild_removes_stale_archives_and_counts_reported_work(self):
        with tempfile.TemporaryDirectory() as directory:
            module, build, archive = self.build(Path(directory))
            report = module.check_archive_lists(archive, build)
            self.assertEqual((report['requested'], report['built']), (1, 1))
            self.assertEqual(set(report['files']), {'current.metalarchive'})
            self.assertFalse((archive / 'obsolete.metalarchive').exists())
            (archive / 'unlisted.metalarchive').write_text('stale')
            with self.assertRaisesRegex(ValueError, 'stale'):
                module.check_archive_lists(archive, build)

    def test_incomplete_runtime_report_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, 'incomplete'):
                self.build(Path(directory), incomplete=True)

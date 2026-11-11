"""Ocarina of Time's pipeline archive is built from the warm-up lists the App carries, never from the seed's own copies.

2026-11-11: a fast update keeps the seed, whose pso_manifest.tsv was resolved from the shader list of 11-09, while the App's
build/ lists were resolved from the current one. The archive held 2,957 render pipelines, the App's lists named 7,516, and
4,559 were compiled at the first start. These tests run games/oot/build.py's own functions on a stale-seed tree.
"""
import importlib.util
import json
import shutil
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'
ARCH = 'applegpu_test'
COMPUTE = ['C\tcompute-variant']


def load_build():
    spec = importlib.util.spec_from_file_location('oot_build_archive_lists', GAME / 'build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('# Generated from shader_list.tsv\n' + ''.join(r + '\n' for r in rows))
    return path


class _Slot:
    def __init__(self, **_):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False


class ArchiveLists(unittest.TestCase):
    def setUp(self):
        self.build = load_build()
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.work, self.tree = base / 'work', base / 'work/oot'
        (self.work / 'build').mkdir(parents=True)
        (self.work / 'build/oot-native').write_bytes(b'runtime')
        seed = self.tree / 'build/shader-seed/oot'
        seed.mkdir(parents=True)
        (seed / 'seed-manifest.json').write_text(json.dumps({'chain': 'test-chain', 'files': {}}))
        # the seed's own list: resolved from an older shared list (one render row)
        self.seed_list = write(seed / 'pso_manifest.tsv', ['R\told-1'])
        write(seed / 'pso_title.tsv', [])
        # the App's lists: the current shared list (four render rows and one tile row)
        lists = self.tree / 'build/warm-lists'
        self.title = write(lists / 'pso_title.tsv', ['R\ttitle-1'])
        self.manifest = write(lists / 'pso_manifest.tsv', ['R\told-1', 'R\tnew-2', 'R\tnew-3', 'T\tnew-4', 'S\tshader\t0',
                                                          'M\t-\tmesh-1', 'M\ttask\tmesh-2'])
        self.argv = None
        self.mesh_archived = 2   # mesh pipelines the fake tool puts in the archive

    def tearDown(self):
        self.tmp.cleanup()

    def fake_run(self, argv, stdout=None, **_):
        self.argv = [str(a) for a in argv]
        render = sum(1 for p in argv[4:] for l in Path(p).read_text().splitlines() if l.startswith(('R\t', 'B\t', 'T\t')))
        stdout.write(json.dumps({'render': render, 'compute': len(COMPUTE), 'mesh': self.mesh_archived, 'kept': 0, 'failed': 0,
                                 'write_failed': 0, 'arch': ARCH}) + '\n')
        return types.SimpleNamespace(returncode=0)

    def fake_files(self, source, target, arch, chain):
        target.mkdir(parents=True, exist_ok=True)
        ids = ['c' + __import__('hashlib').sha256(row[2:].encode()).hexdigest()[:40] for row in COMPUTE]
        ids += [f'm{n:040x}' for n in range(self.mesh_archived)]   # mesh keys: 'm' + 40 hex (mtk_archive_key)
        (target / f'pipelines-{arch}.tsv').write_text(''.join(i + '\t' + i + '\n' for i in ids))
        return len(ids)

    def archive(self):
        modules = {'prepare_compute_pipelines': types.SimpleNamespace(compute_rows=lambda seed: list(COMPUTE)),
                   'dev': types.ModuleType('dev'), 'dev.game_slot': types.SimpleNamespace(GameSlot=_Slot)}
        helpers = types.SimpleNamespace(mac_pipeline_files=self.fake_files)
        with mock.patch.dict(sys.modules, modules), \
             mock.patch.object(self.build.subprocess, 'run', side_effect=self.fake_run), \
             mock.patch.object(self.build, 'build_helpers', return_value=helpers), \
             mock.patch('builtins.print'):
            return self.build.compute_pipeline_archive(self.work, self.tree, (self.title, self.manifest))

    def app_build(self, pipelines, title, manifest):
        """An App's build/ folder as package_app makes it: the archive and the two lists beside the shader map."""
        build = Path(self.tmp.name) / 'App/build'
        shutil.rmtree(build, ignore_errors=True)
        shutil.copytree(pipelines, build / 'pipelines')
        shutil.copyfile(title, build / 'pso_title.tsv')
        shutil.copyfile(manifest, build / 'pso_manifest.tsv')
        return build

    def test_the_tool_gets_the_apps_lists_not_the_seeds(self):
        self.archive()
        self.assertIn(str(self.title), self.argv)
        self.assertIn(str(self.manifest), self.argv)
        self.assertNotIn(str(self.seed_list), self.argv)

    def test_the_record_names_the_lists_and_their_render_rows(self):
        record = json.loads((self.archive() / 'compute-manifest.json').read_text())
        self.assertEqual(record['listed_render'], 5)
        self.assertEqual(record['listed_mesh'], 2)
        self.assertEqual(record['mesh_indexed'], 2)
        self.assertEqual(record['lists_sha256'], {'pso_title.tsv': self.build.sha(self.title),
                                                  'pso_manifest.tsv': self.build.sha(self.manifest)})

    def test_an_app_with_the_archives_lists_passes(self):
        build = self.app_build(self.archive(), self.title, self.manifest)
        self.build.check_archive_lists(build)

    def test_an_app_whose_lists_are_not_the_archives_is_refused(self):
        # the 11-10 App: archive from the seed's list, the App carrying the current list
        stale = Path(self.tmp.name) / 'stale'
        write(stale / 'pso_manifest.tsv', ['R\told-1'])
        build = self.app_build(self.archive(), self.title, stale / 'pso_manifest.tsv')
        with self.assertRaisesRegex(ValueError, 'pso_manifest.tsv'):
            self.build.check_archive_lists(build)

    def test_an_archive_record_without_its_lists_is_refused(self):
        build = self.app_build(self.archive(), self.title, self.manifest)
        record = json.loads((build / 'pipelines/compute-manifest.json').read_text())
        del record['lists_sha256']
        (build / 'pipelines/compute-manifest.json').write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, 'lists_sha256'):
            self.build.check_archive_lists(build)

    def test_a_render_count_that_differs_is_refused(self):
        build = self.app_build(self.archive(), self.title, self.manifest)
        record = json.loads((build / 'pipelines/compute-manifest.json').read_text())
        record['listed_render'] = 2957
        (build / 'pipelines/compute-manifest.json').write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, '2957'):
            self.build.check_archive_lists(build)

    def test_an_archive_without_the_listed_mesh_pipelines_is_refused(self):
        # 2026-11-11: the tool left every M row out, so the 124 listed mesh pipelines were compiled at every start
        self.mesh_archived = 0
        with self.assertRaisesRegex(ValueError, 'mesh'):
            self.archive()

    def test_a_mesh_count_that_differs_is_refused(self):
        build = self.app_build(self.archive(), self.title, self.manifest)
        record = json.loads((build / 'pipelines/compute-manifest.json').read_text())
        record['listed_mesh'] = 0
        (build / 'pipelines/compute-manifest.json').write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, 'mesh'):
            self.build.check_archive_lists(build)

    def test_an_app_without_an_archive_has_nothing_to_check(self):
        self.build.check_archive_lists(Path(self.tmp.name) / 'no-archive')

    def test_packaging_checks_the_app_before_it_is_installed(self):
        body = (GAME / 'build.py').read_text()
        package = body[body.index('def package_app'):body.index('\ndef ', body.index('def package_app') + 1)]
        check = package.index("check_archive_lists(data/'build')")
        self.assertLess(package.index("clone_file(manifest,data/'build/pso_manifest.tsv')"), check)
        self.assertLess(check, package.index('install_app(temporary,out)'))



NVN = GAME / 'third_party/d2r_gfx/gfx/nvn'


def function(text, head):
    start = text.index(head)
    return text[start:text.index('\n}\n', start)]


class MeshPipelinesInTheArchive(unittest.TestCase):
    """2026-11-11: the archive tool left the M rows out and a mesh pipeline never looked in the archive, so OOT's 124 listed
    mesh pipelines were compiled at every start. The GPU check is probe-gpu-pipeline-archive (par_probe_mesh); these keep
    the wiring in place."""
    mtl = (NVN / 'nvn_mtl.m').read_text()
    mesh = (NVN / 'nvn_mesh_tasks.inc').read_text()
    archive = (NVN / 'totk_pipeline_archive.h').read_text()

    def test_the_archive_tool_takes_mesh_rows(self):
        self.assertIn("t != 'M'", function(self.mtl, 'static void warm_fill_archive'))
        self.assertIn('mtk_fill_row(c, dict)', function(self.mtl, 'static void fill_row'))
        self.assertIn('\\"mesh\\"', function(self.mtl, 'int mtl_pipeline_archive_tool'))

    def test_a_mesh_pipeline_is_looked_up_in_the_archive_and_written_to_it(self):
        build = function(self.mesh, 'static id<MTLRenderPipelineState> mtk_build')
        self.assertIn('mtk_archive_key(ts,ms,fs,key)', build)
        self.assertIn('MTLPipelineOptionFailOnBinaryArchiveMiss', build)
        self.assertIn('par_add(g_dev,akey,m)', build)

    def test_the_archive_writes_mesh_descriptors(self):
        self.assertIn('addMeshRenderPipelineFunctionsWithDescriptor', function(self.archive, 'static void par_write'))

    def test_the_probe_checks_a_mesh_pipeline(self):
        self.assertIn('par_probe_mesh()', function(self.mtl, 'int mtl_probe_pipeline_archive'))


if __name__ == '__main__':
    unittest.main()

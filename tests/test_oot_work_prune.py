"""games/oot/work_prune.py: after a build only what no later build reads leaves the work folder (user 2026-11-17: "每次
更新完 要清理 … 不影响极速更新"); package_app calls it once the App is installed."""
from pathlib import Path
import json
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'games/oot'))
import work_prune  # noqa: E402

CURRENT = 'e3' * 32
OLD = ['1b' * 32, '42' * 32]


def make(tree, rel, files=('f',)):
    d = tree / rel
    d.mkdir(parents=True, exist_ok=True)
    for f in files:
        (d / f).write_text('x')
    return d


class WorkPrune(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.tree = Path(self.tmp.name) / 'oot'
        b = 'build/'
        for seed in ('shader-seed', 'shader-seed-title'):
            make(self.tree, f'{b}{seed}/oot', ()).joinpath('seed-manifest.json').write_text(
                json.dumps({'translation_context_sha256': CURRENT}))
        for ctx in [CURRENT, *OLD]:
            make(self.tree, f'{b}ns2-seed-cache/oot/{ctx}')
        make(self.tree, f'{b}shader-bundles/ShaderData-oot.bundle')
        make(self.tree, f'{b}shader-bundles/ShaderData-oot.bundle.previous-1794532982490033000')
        make(self.tree, f'{b}shader-bundles/ShaderData-oot-title.bundle.previous-1794753787415604000')
        make(self.tree, f'{b}shader-libs')
        make(self.tree, f'{b}shader-libs.previous-23556')
        make(self.tree, f'{b}compute-pipelines/archive/pipelines')
        make(self.tree, f'{b}compute-pipelines/oot-native-v26-raw-span-all-stages/pipelines')
        make(self.tree, f'{b}compute-pipelines/oot-native-v27-blend-zero/pipelines')
        make(self.tree, f'{b}compute-pipelines/oot-native-v27-blend-zero-title/pipelines')
        make(self.tree, f'{b}compute-pipelines/oot-native-v28-mesh-primitive-blocks-title/pipelines')
        newer = self.tree / f'{b}compute-pipelines/oot-native-v28-mesh-primitive-blocks-title/pipelines'
        older = self.tree / f'{b}compute-pipelines/oot-native-v27-blend-zero-title/pipelines'
        import os
        os.utime(older, (1, 1)); os.utime(newer, (2, 2))
        make(self.tree, 'extracted/oot/romfs')
        make(self.tree, f'{b}data-bundles/GameData-oot.bundle')
        make(self.tree, f'{b}pipeline-bundles/Pipelines-oot.bundle')
        make(self.tree, f'{b}pipelines')

    def tearDown(self):
        self.tmp.cleanup()

    def names(self):
        return {str(p.relative_to(self.tree)) for p in work_prune.superseded(self.tree)}

    def test_only_superseded_folders(self):
        self.assertEqual(self.names(), {
            *(f'build/ns2-seed-cache/oot/{c}' for c in OLD),
            'build/shader-bundles/ShaderData-oot.bundle.previous-1794532982490033000',
            'build/shader-bundles/ShaderData-oot-title.bundle.previous-1794753787415604000',
            'build/shader-libs.previous-23556',
            'build/compute-pipelines/oot-native-v26-raw-span-all-stages',
            'build/compute-pipelines/oot-native-v27-blend-zero',
            'build/compute-pipelines/oot-native-v27-blend-zero-title',   # older title folder; the newest is taken over
        })

    def test_prune_keeps_what_an_express_update_reuses(self):
        moved = work_prune.prune(self.tree, log=lambda line: None, delete=False)
        self.assertEqual(len(moved), 8)   # 2 contexts, 2 bundles, 1 library folder, 3 chain folders (test_only_superseded_folders)
        trash = work_prune.trash_folders(self.tree)
        self.assertEqual(len(trash), 1, 'everything moved into one trash folder, to be deleted in the background')
        self.assertEqual(len(list(trash[0].iterdir())), 8)
        for path in moved:
            self.assertFalse(path.exists(), path)
        for keep in (f'build/ns2-seed-cache/oot/{CURRENT}', 'build/shader-bundles/ShaderData-oot.bundle', 'build/shader-libs',
                     'build/compute-pipelines/archive', 'build/compute-pipelines/oot-native-v28-mesh-primitive-blocks-title',
                     'extracted/oot/romfs', 'build/data-bundles/GameData-oot.bundle', 'build/pipeline-bundles/Pipelines-oot.bundle',
                     'build/pipelines', 'build/shader-seed/oot/seed-manifest.json'):
            self.assertTrue((self.tree / keep).exists(), keep)
        self.assertEqual(self.names(), set(), 'a second run finds nothing')

    def test_trash_is_deleted_and_a_left_one_too(self):
        work_prune.prune(self.tree, log=lambda line: None, delete=False)
        left = work_prune.trash_folders(self.tree)
        work_prune.delete_trash(left)
        self.assertEqual(work_prune.trash_folders(self.tree), [])
        outside = Path(self.tmp.name) / 'not-trash'
        outside.mkdir()
        work_prune.delete_trash([outside])   # only .trash-* folders are ever deleted
        self.assertTrue(outside.exists())

    def test_parallel_delete_removes_nested_trees_and_links_without_following(self):
        trash = self.tree / 'build/.trash-1'
        for i in range(40):
            make(trash, f'ctx{i % 4}/sub{i}', ('a.spv', 'b_b.metal'))
        outside = Path(self.tmp.name) / 'kept'
        outside.mkdir(); (outside / 'x').write_text('x')
        (trash / 'link').symlink_to(outside, target_is_directory=True)
        work_prune.delete_trash([trash])
        self.assertFalse(trash.exists())
        self.assertTrue((outside / 'x').exists(), 'a link is removed, not followed')

    def test_old_spv_only(self):
        ctx = self.tree / f'build/ns2-seed-cache/oot/{CURRENT}'
        old, new, metal = ctx / 'a.spv', ctx / 'b.spv', ctx / 'a_b.metal'
        for p in (old, new, metal):
            p.write_text('x')
        import os
        os.utime(old, (1, 1))
        self.assertEqual(work_prune.old_spv(self.tree), [str(old)], 'a fresh .spv (a translation in progress) and the MSL stay')
        self.assertEqual(len(work_prune.old_spv(self.tree, limit=1)), 1)

    def test_runtime_drops_the_spirv_after_translating(self):
        src = (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m').read_text()
        body = src[src.index('static NSDictionary *translate(rt_svc *svc'):]
        body = body[:body.index('\n}\n')]
        self.assertIn('for (NSString *end in @[@".spv", @".reader.spv"])', body)
        self.assertLess(body.index('reflection missing'), body.index('@".reader.spv"'), 'only after the reply and its reflection')

    def test_an_archive_title_folder_supersedes_every_title_chain_folder(self):
        make(self.tree, 'build/compute-pipelines/archive-title/pipelines')
        self.assertIn('build/compute-pipelines/oot-native-v28-mesh-primitive-blocks-title', self.names())

    def test_no_seed_manifest_keeps_every_translation_context(self):
        for m in (self.tree / 'build').glob('shader-seed*/oot/seed-manifest.json'):
            m.unlink()
        self.assertFalse(any('ns2-seed-cache' in n for n in self.names()))

    def test_package_app_prunes_after_installing(self):
        src = (ROOT / 'games/oot/build.py').read_text()
        body = src[src.index('def package_app('):src.index('def missing_compute(')]
        self.assertLess(body.index('build_app.install_app(temporary,out)'), body.index('prune(tree,'))


if __name__ == '__main__':
    unittest.main()

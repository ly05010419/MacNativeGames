"""update_runtime.py --plan for an Ocarina of Time app answers what its update would do (games/oot/build.py update_blocker,
the checks update_runtime makes first): "full" when the work folder cannot take an express update. 2026-11-13: --plan said
"express" for every OOT app, so after the chain moved to v27 MNG Launcher and tools/mng.py started an express update that
stopped at once ("the shaders need the full build")."""
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import update_runtime  # noqa: E402


def oot_app(root, work):
    app = root / 'Ocarina of Time.app'
    (app / 'Contents').mkdir(parents=True)
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable': 'OcarinaOfTime', 'MNGWorkFolder': str(work)}))
    return app


class OcarinaPlan(unittest.TestCase):
    def setUp(self):
        # a Mac with Xcode's Metal compiler (the plan asks for it first: OotNeedsMetal below)
        patch = mock.patch.object(update_runtime.build_app, 'metal_compiler', return_value=('metal', 'v', {}))
        patch.start()
        self.addCleanup(patch.stop)

    def test_a_work_folder_without_its_build_files_needs_the_full_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); work = root / 'work'; work.mkdir()
            plan = update_runtime.plan(oot_app(root, work))
        self.assertEqual((plan['kind'], plan['game']), ('full', 'oot'))
        self.assertIn('is missing', plan['reason'])

    def test_the_plan_takes_the_update_s_own_answer(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); work = root / 'work'; work.mkdir()
            app = oot_app(root, work)
            for answer, kind in (('the translation chain or the translator changed', 'full'), ('', 'express')):
                with self.subTest(answer=answer), mock.patch.object(update_runtime, 'oot_update_blocker', return_value=answer):
                    plan = update_runtime.plan(app)
                    self.assertEqual(plan['kind'], kind, plan)

    def test_the_update_refuses_with_the_same_check(self):
        text = (ROOT / 'games/oot/build.py').read_text()
        self.assertIn('blocker=update_blocker(work,mode,remake=remake)\n    if blocker:raise ValueError(blocker)', text)

    def test_a_new_chain_is_a_fast_update(self):
        """User 2026-11-17 "oot 加上": when only the chain or translator stops the express update and the work folder can
        translate every shader again, the plan is the fast update (--seed), not the full build."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); work = root / 'work'; work.mkdir()
            app = oot_app(root, work)
            chain = 'the translation chain or the translator changed'
            def answer(folder, app=None, remake=False):
                return '' if remake else chain
            with mock.patch.object(update_runtime, 'oot_update_blocker', side_effect=answer):
                plan = update_runtime.plan(app)
            self.assertEqual(plan['kind'], 'fast', plan)
            self.assertIn('translated again from the work folder', plan['reason'])
            self.assertNotIn('full build', plan['reason'])


class OotNeedsMetal(unittest.TestCase):
    """Ocarina of Time compiles its shader libraries with Xcode's Metal compiler (build_shader_libs.py stops without it,
    after the translation); the command line tools alone have none. MNG Launcher says so before a build or an update
    starts (user 2026-11-17: "修"), and only for Ocarina of Time."""

    def test_an_update_says_so_instead_of_starting(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); work = root / 'work'; work.mkdir()
            with mock.patch.object(update_runtime.build_app, 'metal_compiler', return_value=None), \
                    mock.patch.object(update_runtime, 'oot_update_blocker', return_value='') as blocker:
                plan = update_runtime.plan(oot_app(root, work))
            self.assertEqual(plan['kind'], 'error', plan)
            self.assertIn('Metal toolchain', plan['reason'])
            blocker.assert_not_called()

    def test_a_build_says_so_in_step_three_for_ocarina_of_time_only(self):
        from tools.builder_jobs import classify
        with tempfile.TemporaryDirectory() as tmp:
            dump = str(Path(tmp) / 'dump')
            for found, game, blocked in ((None, 'oot', True), (('metal', 'v', {}), 'oot', False), (None, 'dkb', False)):
                with self.subTest(found=found, game=game), \
                        mock.patch.object(classify, 'no_metal_compiler', return_value=found is None):
                    p = classify.plan([classify.Item(dump, 'ns2_dump', game)])
                    self.assertEqual(any('Metal' in b for b in p.blocks), blocked, p.blocks)
                    # the Launcher shows it on its build page, with this Mac's needs, not with the files (tool_blocks)
                    self.assertEqual(p.tool_blocks, [b for b in p.blocks if 'Metal' in b])


def oot_build():
    import importlib.util
    spec = importlib.util.spec_from_file_location('oot_build_fast_test', ROOT / 'games/oot/build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FastUpdateBlocker(unittest.TestCase):
    """games/oot/build.py update_blocker(remake=True): the fast update needs the work folder's game files and this
    repository's rewritten modules, not the old seed or the same chain."""

    def work_folder(self, root, context='an older chain'):
        import json
        tree = root / 'work/oot'
        (tree / 'extracted/oot/romfs').mkdir(parents=True)
        (tree / 'extracted/oot/romfs/index.json').write_text('{}')
        (tree / 'rewritten/oot').mkdir(parents=True)
        build = oot_build()
        seed, libraries, _ = build.shader_dirs(tree, 'full')
        seed.mkdir(parents=True); libraries.mkdir(parents=True)
        (seed / 'seed-manifest.json').write_text(json.dumps({'translation_context_sha256': context, 'nvn_depth_mode': 'minus-w'}))
        (libraries / 'libraries-manifest.json').write_text('{}')
        expected = json.loads((ROOT / 'games/oot/assets/expected-v131072.json').read_text())['rewritten_sha256']
        return build, root / 'work', expected

    def test_a_changed_chain_stops_the_express_update_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            build, work, expected = self.work_folder(Path(tmp))
            with mock.patch.object(build, 'sha', side_effect=lambda p: expected[Path(p).name]):
                self.assertIn('translation chain', build.update_blocker(work, 'full'))
                self.assertEqual(build.update_blocker(work, 'full', remake=True), '')

    def test_the_fast_update_still_needs_the_game_files_and_the_rewritten_modules(self):
        with tempfile.TemporaryDirectory() as tmp:
            build, work, expected = self.work_folder(Path(tmp))
            with mock.patch.object(build, 'sha', side_effect=lambda p: 'other'):
                self.assertIn('rewritten module differs', build.update_blocker(work, 'full', remake=True))
            (work / 'oot/extracted/oot/romfs/index.json').unlink()
            self.assertIn('is missing', build.update_blocker(work, 'full', remake=True))

    def test_update_runtime_takes_seed_for_ocarina_of_time(self):
        text = (ROOT / 'update_runtime.py').read_text()
        self.assertIn('return update_ocarina(app, work, remake=args.seed)', text)
        self.assertNotIn("'Ocarina of Time: --seed and --native-5k are not supported", text)


if __name__ == '__main__':
    unittest.main()

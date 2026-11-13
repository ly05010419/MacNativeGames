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
        self.assertIn('blocker=update_blocker(work)\n    if blocker:raise ValueError(blocker)', text)


if __name__ == '__main__':
    unittest.main()

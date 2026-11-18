"""The time a build has left comes from what this build is doing (user 2026-11-17: "预估 时间 也不对 没有根据 真实情况来
总是上次的情况"): the running step from its own counter (translation, shader libraries, pipelines print "done / total"), the
steps after it from this Mac's last build of the same kind until they start and count. A step without a counter that has
run longer than last time has no honest number left, so none is given instead of one that stands still."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.builder_jobs import history  # noqa: E402

SWIFT = ROOT / 'tools/builder_app/Sources/MNGBuilder'
# this Mac's express update of Ocarina of Time, 2026-11-17 08:26 (Builder/history/oot-macos-express.json, rounded)
LAST = [('native-runtime', 7.0), ('prepare-shader-seed', 1.0), ('ns2-seed-oot', 31.0), ('compile-shader-libraries', 42.0),
        ('shader-list-resolve', 25.0), ('compute-pipelines', 75.0), ('sign-app', 4.0)]
AFTER_SEED = 42.0 + 25.0 + 75.0 + 4.0


class RunningStepFromItsCounter(unittest.TestCase):
    def test_a_slower_translation_than_last_time_moves_the_time_left(self):
        e = history.Estimator(LAST)
        # 120 s in, 400 of 42848 translated: this step alone needs ~12,700 s more; last time's 31 s says nothing
        _, left, measured, step_left = e.update('ns2-seed-oot', 120, 3, (400, 42848))
        self.assertAlmostEqual(step_left, 120 * (42848 - 400) / 400)
        self.assertAlmostEqual(left, step_left + AFTER_SEED)
        self.assertTrue(measured)

    def test_a_faster_step_than_last_time_too(self):
        e = history.Estimator(LAST)
        _, left, _, step_left = e.update('compute-pipelines', 10, 3, (2000, 2500))
        self.assertAlmostEqual(step_left, 10 * 500 / 2000)
        self.assertAlmostEqual(left, step_left + 4.0)

    def test_the_counter_is_trusted_after_a_few_seconds_and_a_first_item(self):
        e = history.Estimator(LAST)
        self.assertEqual(e.update('ns2-seed-oot', 2, 3, (5, 42848))[3], 31.0 - 2)        # too early: last time's
        self.assertEqual(e.update('ns2-seed-oot', 20, 3, (0, 42848))[3], 31.0 - 20)      # nothing counted yet
        self.assertGreater(e.update('ns2-seed-oot', 20, 3, (10, 42848))[3], 1000)


class WithoutACounter(unittest.TestCase):
    def test_last_time_until_it_runs_longer_then_no_number(self):
        e = history.Estimator(LAST)
        _, left, _, step_left = e.update('native-runtime', 3, 1)
        self.assertEqual((step_left, left), (4.0, 4.0 + sum(s for _, s in LAST[1:])))
        _, left, measured, step_left = e.update('native-runtime', 30, 1)   # last time 7 s
        self.assertEqual((left, step_left, measured), (None, None, True))

    def test_a_step_new_to_the_record_counts_but_gives_no_total(self):
        e = history.Estimator(LAST)
        _, left, _, step_left = e.update('something-new', 20, 3, (100, 300))
        self.assertAlmostEqual(step_left, 40.0)
        self.assertIsNone(left)

    def test_a_first_build_still_reports_its_running_step(self):
        e = history.Estimator(None)
        fraction, left, measured, step_left = e.update('ns2-seed-oot', 60, 3, (600, 1800))
        self.assertEqual((fraction, left, measured), (history.STAGE_STARTS[3], None, False))
        self.assertAlmostEqual(step_left, 120.0)


class JobAndWindow(unittest.TestCase):
    def test_the_job_passes_the_counter_and_sends_the_step_s_time_left(self):
        source = (ROOT / 'tools/builder_jobs/build_job.py').read_text()
        self.assertIn("estimator.update(state['step'], since, state['stage'], counted)", source)
        self.assertIn('step_left=None if step_left is None else round(step_left)', source)

    def test_the_window_shows_the_step_s_time_left(self):
        self.assertIn('step_left: Double?', (SWIFT / 'Reports.swift').read_text())
        self.assertIn('stepLeft = e.step_left', (SWIFT / 'Model.swift').read_text())
        action = (SWIFT / 'ActionSection.swift').read_text()
        self.assertIn('T.stepLeft(T.minutes(stepLeft))', action)
        strings = (SWIFT / 'Strings.swift').read_text()
        self.assertIn('"这一步还剩\\(text)"', strings)   # T.minutes gives "约 N 分钟"


if __name__ == '__main__':
    unittest.main()

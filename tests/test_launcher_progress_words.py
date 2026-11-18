"""MNG Launcher shows where a build is in words, not with a bar (user 2026-11-16: "进度条 根本不动 改成 描述 比如 完成第一步
共五步 现在第三步 400/40000 个着色器"): the step's number, the steps the same kind of build took last time, and the running
step's own counter, which the long steps now print as "done / total"."""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.builder_jobs import build_job, history  # noqa: E402

NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m'
SWIFT = ROOT / 'tools/builder_app/Sources/MNGBuilder'


class StepCounter(unittest.TestCase):
    def count(self, *lines):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'step.log').write_text(''.join(line + '\n' for line in lines))
            return build_job.Steps(tmp, 0).count('step')

    def test_the_translation_counter(self):
        self.assertEqual(self.count('[dm] [nvn-mtl] [seed] 42848 shaders in build/ns2-corpus/oot.bin: 42848 queued',
                                    '[dm] [nvn-mtl] [seed] 400 / 42848 shaders translated (42448 left, 4 s)'), (400, 42848))

    def test_the_pipeline_counter(self):
        self.assertEqual(self.count('[dm] [nvn-mtl] [pipeline-archive] 1536 / 28714 pipelines checked or built'), (1536, 28714))

    def test_the_library_counter(self):
        self.assertEqual(self.count('42911 translated shaders, 300 to compile', '  250 / 300'), (250, 300))


class RuntimePrintsTheCounters(unittest.TestCase):
    """The packaging tools' lines the counter above reads (nvn_mtl.m), in the form it reads."""
    def test_translation_and_pipeline_archive_print_done_over_total(self):
        source = NVN.read_text()
        self.assertIn('"[seed] %lu / %lu shaders translated', source)
        self.assertIn('"[pipeline-archive] %lu / %lu pipelines checked or built"', source)
        self.assertRegex(source, r'if \(\+\+said % 4 == 0\) MLOG\("\[seed\] %lu / %lu')   # every 2 s (0.5 s sleeps)


class PlanNamesTheLastStepCount(unittest.TestCase):
    def test_steps_last_comes_from_this_mac_s_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = history.record_path(tmp, 'oot', 'macos-express')
            history.record(path, [('native-runtime', 0), ('ns2-seed-oot', 10), ('compute-pipelines', 30)], 60)
            est = history.Estimator(history.load(path))
            self.assertEqual(len(est.last), 3)
        source = (ROOT / 'tools/builder_jobs/build_job.py').read_text()
        self.assertIn("steps_last=len(estimator.last) if estimator.last else None", source)


class WindowShowsWords(unittest.TestCase):
    def test_no_overall_bar_and_the_words_are_there(self):
        action = (SWIFT / 'ActionSection.swift').read_text()
        self.assertNotIn('ProgressView(value: model.fraction)', action)
        self.assertIn('T.stepLine(model.stepIndex, model.stepsLast', action)
        self.assertIn('T.counted(done, total, T.stepUnit(model.step))', action)
        strings = (SWIFT / 'Strings.swift').read_text()
        self.assertIn('现在第 \\(i) 步：\\(what)', strings)
        self.assertIn('上次共 \\(n) 步', strings)
        self.assertIn('("ns2-seed", "Translating the shaders", "翻译着色器")', strings)
        self.assertIn('steps_last: Int?', (SWIFT / 'Reports.swift').read_text())

    def test_every_step_log_of_these_builds_has_words(self):
        """The step names the Mac builds of this repository give their logs (the ones in this Mac's work folders on
        2026-11-16) each match a rule of Strings.stepName."""
        strings = (SWIFT / 'Strings.swift').read_text()
        prefixes = re.findall(r'\("([^"]+)", "[^"]+", "[^"]+"\)', strings)
        for log in ('native-runtime', 'prepare-shader-seed', 'ns2-seed-oot', 'compile-shader-libraries',
                    'shader-list-resolve', 'compute-pipelines', 'sign-app', 'import-original-dump', 'rewrite-main',
                    'verify-rewrite-sdk', 'prepare-shared-fonts', 'mac-pipelines', 'shader-libs', 'prepare-ns2-seed-dkb',
                    'ns2-corpus-dkb', 'sign-data-dkb', 'verify-data-v143', 'register-app', 'runtime', 'app_icon',
                    'warm-metal-cache-build', 'import_dump-dkb', 'prepare_modules-dkb', 'rewrite_dkb_main', 'venv'):
            self.assertTrue(any(log.startswith(p) for p in prefixes), log)


if __name__ == '__main__':
    unittest.main()

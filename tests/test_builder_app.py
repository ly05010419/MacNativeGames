"""tools/builder/app.py (S2.1, T3): the composition root wires the real adapters under the given home."""
from pathlib import Path
import tempfile
import unittest

from tools.builder.app import build_controller
from tools.builder.checks_inputs import INPUT_CHECKS
from tools.builder.checks_system import SYSTEM_CHECKS
from tools.builder.config import default_settings_path
from tools.builder.jobs import DaemonJobs, Jobs
from tools.builder.ports import CommandRunner, DiskInfo

REPO = Path(__file__).resolve().parents[1]


def stamp(path):
    return (path.exists(), path.stat().st_mtime_ns if path.exists() else None)


class Composition(unittest.TestCase):
    def test_wiring_under_a_temporary_home(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        home, real = Path(tmp.name), default_settings_path(Path.home())
        before = stamp(real)
        ctl = build_controller(home, REPO)
        self.addCleanup(ctl.close)
        self.assertTrue(ctl.store.path.is_relative_to(home))
        self.assertEqual(ctl.logs_dir, home / 'Library' / 'Logs' / 'MacNativeGames')
        self.assertEqual(ctl.checks, SYSTEM_CHECKS + INPUT_CHECKS)
        self.assertIsInstance(ctl.jobs, Jobs)
        self.assertIsInstance(ctl.load_jobs, DaemonJobs)        # S3.4 f4: the load off the single worker
        self.assertIsInstance(ctl.runner, CommandRunner)
        self.assertIsInstance(ctl.base_context.disk, DiskInfo)
        self.assertEqual(ctl.base_context.repo_root, REPO)
        first, second = ctl.process_factory(), ctl.process_factory()
        self.assertIsNot(first, second)                         # one BuildProcess per build
        self.assertEqual(stamp(real), before)                   # the real settings file is not touched

    def test_given_jobs_are_used(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        jobs = Jobs()
        ctl = build_controller(Path(tmp.name), REPO, jobs=jobs)
        self.addCleanup(ctl.close)
        self.assertIs(ctl.jobs, jobs)


if __name__ == '__main__':
    unittest.main()

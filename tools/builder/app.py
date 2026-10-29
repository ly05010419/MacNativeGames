"""The composition root (S2.1, T3): the one place that creates the real adapters and wires the controller.

Separate from __main__ so that tests can build the real composition with a temporary home without running the entry
point."""
from pathlib import Path

from tools.builder.check_report import logs_dir
from tools.builder.checks import CheckContext
from tools.builder.checks_inputs import INPUT_CHECKS
from tools.builder.checks_system import SYSTEM_CHECKS
from tools.builder.config import SettingsStore, default_settings_path
from tools.builder.controller import BuildController
from tools.builder.jobs import DaemonJobs, Jobs
from tools.builder.ports import CommandRunner, DiskInfo, RealClock
from tools.builder.process import BuildProcess


def build_controller(home, repo_root, jobs=None):
    clock = RealClock()
    runner = CommandRunner(clock)
    return BuildController(base_context=CheckContext(runner, Path(repo_root), disk=DiskInfo()),
                           system_checks=SYSTEM_CHECKS, input_checks=INPUT_CHECKS,
                           store=SettingsStore(default_settings_path(home), clock),
                           process_factory=lambda: BuildProcess(clock),       # one BuildProcess runs one build
                           jobs=jobs if jobs is not None else Jobs(), runner=runner, clock=clock,
                           repo_root=Path(repo_root), logs_dir=logs_dir(home), load_jobs=DaemonJobs())

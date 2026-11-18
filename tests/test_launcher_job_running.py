"""MNG Launcher: a finished job leaves nothing disabled. `Model.updating` names the app the last job was for and stays after
it ends (the card keeps showing the result and the log), so it must never be read as "a job is running": a reporter
(2026-11-17) found the card's Start grey after a `tools/mng.py build` until the Launcher was reopened, and the Storage
page's folder buttons stayed off the same way. Busy is `Model.jobRunning` (the job's phase)."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SWIFT = ROOT / 'tools/builder_app/Sources/MNGBuilder'


class UpdatingIsNotBusy(unittest.TestCase):
    def test_job_running_is_the_phase(self):
        model = (SWIFT / 'Model.swift').read_text()
        self.assertIn('var jobRunning: Bool { phase == .building || phase == .cancelling }', model)

    def test_no_view_disables_on_updating_alone(self):
        """Every `.disabled(...)` and every busy flag that reads `updating` also needs `jobRunning`."""
        bad = []
        for path in sorted(SWIFT.glob('*.swift')):
            for n, line in enumerate(path.read_text().splitlines(), 1):
                if 'updating' not in line or 'jobRunning' in line:
                    continue
                if '.disabled(' in line or re.search(r'var (building|busy)\b', line):
                    bad.append(f'{path.name}:{n}: {line.strip()}')
        self.assertEqual(bad, [])

    def test_start_and_storage_use_it(self):
        home = (SWIFT / 'HomePage.swift').read_text()
        self.assertIn('.disabled(starting || (model.jobRunning && model.updating == app.app))', home)
        storage = (SWIFT / 'StorageTab.swift').read_text()
        self.assertIn('private var building: Bool { model.jobRunning }', storage)


if __name__ == '__main__':
    sys.exit(unittest.main())

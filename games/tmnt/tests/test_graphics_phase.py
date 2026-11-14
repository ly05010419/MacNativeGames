"""Compile and exercise the actual graphics phase helper without game files."""
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class GraphicsPhaseTests(unittest.TestCase):
    def run_probe(self, probe, repeats, args=(), expected=0):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'graphics_phase_probe'
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-I', str(ROOT / 'runtime'),
                str(ROOT / 'runtime/graphics_phase.c'),
                str(ROOT / 'tests' / probe),
                '-o', str(binary),
            ], check=True)
            for _ in range(repeats):
                result = subprocess.run([str(binary), *args], timeout=5,
                                        stdout=subprocess.DEVNULL)
                self.assertEqual(result.returncode, expected)

    def test_concurrent_and_exit_paths(self):
        self.run_probe('graphics_phase_probe.c', 100)

    def test_caught_exception_without_clear(self):
        self.run_probe('graphics_phase_exception_probe.c', 10)

    def test_postcleanup_caught_return(self):
        self.run_probe('graphics_phase_postcleanup_probe.c', 25)

    def test_postcleanup_rethrow(self):
        self.run_probe('graphics_phase_postcleanup_probe.c', 25, ('r',))

    def test_host_dispatch_contract(self):
        self.run_probe('graphics_phase_dispatch_probe.c', 1)

    def test_two_active_devices_and_reuse(self):
        self.run_probe('graphics_phase_multidevice_probe.c', 25)

    def test_active_device_capacity_fails_closed(self):
        self.run_probe('graphics_phase_capacity_probe.c', 1, expected=7)

    def test_ambiguous_exception_owner_fails_closed(self):
        self.run_probe('graphics_phase_ambiguity_probe.c', 1, expected=8)

    def test_rethrow_discards_caught_mark(self):
        self.run_probe('graphics_phase_rethrow_mark_probe.c', 25)


if __name__ == '__main__':
    unittest.main()

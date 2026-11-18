"""A crash at the scheduled close time must never become a passing session."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('session_result', Path(__file__).resolve().parents[1] /
                                            'games/oot/ios/tools/session_result.py')
result = importlib.util.module_from_spec(spec)
spec.loader.exec_module(result)


class SessionResultTests(unittest.TestCase):
    def test_gpu_oom_at_close_is_failure(self):
        log = 'Insufficient Memory (00000008:kIOGPUCommandBufferCallbackErrorOutOfMemory)\nFATAL descriptor input synchronization failed'
        self.assertFalse(result.classify(log)['passed'])

    def test_exit_alone_is_not_success(self):
        self.assertFalse(result.classify('frame 2091')['passed'])

    def test_crash_after_close_request_is_failure(self):
        self.assertFalse(result.classify(result.CLOSE_MARKER, crash_reports=['TOTK.ips'])['passed'])
        self.assertFalse(result.classify(result.CLOSE_MARKER + '\nGPU submit failed')['passed'])

    def test_cleanup_must_finish(self):
        self.assertFalse(result.classify(result.CLOSE_MARKER, remaining=[3243])['passed'])

    def test_only_completed_save_safe_close_passes(self):
        self.assertTrue(result.classify('[frontend] ' + result.CLOSE_MARKER)['passed'])


if __name__ == '__main__':
    unittest.main()

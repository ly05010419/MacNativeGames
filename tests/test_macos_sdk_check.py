"""The developer tools' macOS SDK must have Metal 4 (macOS 26 SDK or later): an older Xcode on macOS 26+ passed every
check, and the build stopped minutes later at "cannot find protocol declaration for 'MTL4CommandBuffer'" (#2250)."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import build_app  # noqa: E402
from tools.builder_jobs import checks  # noqa: E402


def tools(sdk):
    """subprocess.run as the developer tools answer it, with `sdk` as xcrun --show-sdk-version."""
    def run(cmd, **kw):
        out = sdk if cmd[:2] == ['xcrun', '--show-sdk-version'] else ''
        return mock.Mock(returncode=0, stdout=out if kw.get('text') else out.encode(), stderr='')
    return run


class SdkCheck(unittest.TestCase):
    def sdk_check(self, sdk):
        with mock.patch.object(checks.subprocess, 'run', side_effect=tools(sdk)):
            return next(c for c in checks.checks(tempfile.gettempdir()) if c['id'] == 'sdk')

    def test_the_window_says_an_old_sdk_before_the_build(self):
        c = self.sdk_check('15.5\n')
        self.assertFalse(c['ok'])
        self.assertIn('15.5', c['text'])
        self.assertTrue(c['fix'])

    def test_a_metal_4_sdk_passes(self):
        for sdk in ('26.0', '27.2\n'):
            self.assertTrue(self.sdk_check(sdk)['ok'], sdk)

    def test_the_build_stops_at_once_on_an_old_sdk(self):
        args = mock.Mock(platform='macos')
        with mock.patch.object(build_app.subprocess, 'run', side_effect=tools('15.5')), \
             mock.patch.object(build_app.shutil, 'which', return_value='/usr/bin/x'), \
             mock.patch.object(build_app, 'codec_libraries'):
            with self.assertRaises(build_app.BuildError) as e:
                build_app.prerequisites(args)
        self.assertIn('15.5', str(e.exception))
        self.assertIn('Metal 4', str(e.exception))


if __name__ == '__main__':
    unittest.main()

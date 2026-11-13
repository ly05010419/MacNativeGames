"""A pipeline archive is mapped, not read into memory, from an external drive (nvn_archive_map.h; 2026-11-13: Ocarina of
Time from a USB drive held its 5.7 GB archive as 2,946 NSData, a footprint of 18.2 GB). The hook acts only while the host
opens an archive, only on that thread and only for .metalarchive files; with real archives on an external drive the
footprint they add is compared, read whole against mapped."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
PROBE = ROOT / 'tests/archive_map_probe.m'
APP_ARCHIVES = Path('/Volumes/开发/MacNativeGames-artifacts/keep/apps/Ocarina of Time.app/Contents/Resources/totk/build/pipelines')


def external_archives(n=40):
    """Real archives on an external drive of this Mac, when there are any."""
    if not APP_ARCHIVES.is_dir():
        return []
    mount = subprocess.run(['df', str(APP_ARCHIVES)], capture_output=True, text=True).stdout.splitlines()[-1].split()[-1]   # diskutil takes the volume, not a path inside it
    info = subprocess.run(['diskutil', 'info', mount], capture_output=True, text=True).stdout
    if 'External' not in info:
        return []
    return [str(p) for p in sorted(APP_ARCHIVES.glob('*.metalarchive'))[:n]]


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools and Metal')
class ArchiveMap(unittest.TestCase):
    def test_the_hook_on_this_mac(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'probe'
            build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-w', f'-I{NVN}', str(PROBE), '-framework', 'Foundation',
                                    '-framework', 'Metal', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            r = subprocess.run([str(exe), *external_archives()], capture_output=True, text=True, timeout=300)
        lines = r.stdout.splitlines()
        self.assertTrue(lines and lines[-1].startswith('done '), r.stdout[-2000:] + r.stderr[-2000:])
        self.assertEqual([l for l in lines if l.startswith('FAIL')], [])
        self.assertEqual(lines[-1], 'done 0')

    def test_every_archive_open_is_in_a_window(self):
        text = (NVN / 'totk_pipeline_archive.h').read_text()
        opens = text.count('newBinaryArchiveWithDescriptor:d ')
        self.assertEqual(opens, text.count('arch_map_begin(); a = [dev newBinaryArchiveWithDescriptor:d error:&e]; arch_map_end();'))
        self.assertGreater(opens, 0)


if __name__ == '__main__':
    unittest.main()

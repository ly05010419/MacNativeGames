"""tools/builder/checks_inputs.py (S1.2b): disk_space, output_volume, the full run with work and out left None, and
the drift test against build_app.py's thresholds and cache paths (read as text, never imported)."""
import errno
from pathlib import Path
import tempfile
import unittest

from tools.builder import checks_inputs
from tools.builder.checks import BuildPaths, CheckContext, Status, run_checks
from tools.builder.checks_inputs import GB, INPUT_CHECKS, DiskSpace, OutputVolume
from tools.builder.games import BOTW, TOTK
from tools.builder.ports import VolumeInfo

REPO = Path(__file__).resolve().parents[1]


class RecordingDisk:
    """DiskInfo with a fixed free size, a device per path (default 1), and the list of probed targets."""

    def __init__(self, free_gb=100.0, devices=None, error=None):
        self.free, self.devices, self.error, self.probed = int(free_gb * GB), devices or {}, error, []

    def probe(self, path):
        self.probed.append(Path(path))
        if self.error:
            raise self.error
        return VolumeInfo(Path(path), Path(path), self.free, self.devices.get(Path(path), 1))


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name).resolve()
        self.games = self.tmp / 'Games'
        self.games.mkdir()
        self.game = self.games / 'g.xci'
        data = bytearray(0x200)
        data[0x100:0x104] = b'HEAD'
        self.game.write_bytes(bytes(data))
        self.keys = self.tmp / 'prod.keys'
        self.keys.write_text('k')

    def ctx(self, disk, spec=TOTK, **paths):
        return CheckContext(runner=None, repo_root=REPO, spec=spec, disk=disk,
                            paths=BuildPaths(self.game, self.keys, **paths))

    def cache(self, work, spec):
        path = work / checks_inputs.ROMFS_CACHES[spec.id][0]
        path.parent.mkdir(parents=True)
        path.write_bytes(b'')


class DiskSpaceCheck(Base):
    def status(self, free_gb, spec=TOTK, **paths):
        return DiskSpace().run(self.ctx(RecordingDisk(free_gb), spec, **paths)).status

    def test_thresholds(self):
        update = self.games / 'u.nsp'
        update.write_bytes(b'PFS0')
        cases = [(TOTK, {}, 24.9, Status.BLOCK), (TOTK, {}, 25.0, Status.OK),
                 (TOTK, {'update': update}, 39.9, Status.BLOCK), (TOTK, {'update': update}, 40.0, Status.OK),
                 (BOTW, {}, 21.9, Status.BLOCK), (BOTW, {}, 22.0, Status.OK), (BOTW, {'update': update}, 22.0, Status.OK)]
        for spec, paths, free, expected in cases:
            with self.subTest(spec=spec.id, update='update' in paths, free=free):
                self.assertEqual(self.status(free, spec, **paths), expected)

    def test_block_detail(self):
        r = DiskSpace().run(self.ctx(RecordingDisk(24.9)))
        self.assertEqual((r.message_id, r.detail, r.fix), ('disk_space', f'24.9 GB free on {self.games}, 25 GB needed', None))

    def test_each_game_counts_only_its_own_cache(self):
        work = self.tmp / 'w'
        self.cache(work, TOTK)
        self.assertEqual(self.status(1, TOTK, work=work), Status.OK)
        self.assertEqual(self.status(1, BOTW, work=work), Status.BLOCK)
        self.cache(work, BOTW)
        self.assertEqual(self.status(1, BOTW, work=work), Status.OK)

    def test_probe_target(self):
        disk = RecordingDisk()
        DiskSpace().run(self.ctx(disk, work=self.tmp / 'exists' / 'w'))      # work.parent missing → game folder
        (self.tmp / 'exists').mkdir()
        DiskSpace().run(self.ctx(disk, work=self.tmp / 'exists' / 'w'))      # work.parent exists → it
        self.assertEqual(disk.probed, [self.games, self.tmp / 'exists'])

    def test_probe_errors_propagate(self):
        disk = RecordingDisk(error=OSError(errno.EIO, 'I/O error'))
        self.assertRaises(OSError, DiskSpace().run, self.ctx(disk))


class OutputVolumeCheck(Base):
    def test_same_and_other_volume(self):
        out_parent = self.tmp / 'Apps'
        out_parent.mkdir()
        self.assertEqual(OutputVolume().run(self.ctx(RecordingDisk(), out=out_parent / 'T.app')).status, Status.OK)
        r = OutputVolume().run(self.ctx(RecordingDisk(devices={out_parent: 2}), out=out_parent / 'T.app'))
        self.assertEqual((r.status, r.message_id, r.fix), (Status.WARN, 'output_other_volume', None))
        self.assertEqual(r.detail, f'{self.games} | {out_parent}')

    def test_missing_out_parent_uses_the_game_folder(self):
        disk = RecordingDisk()
        OutputVolume().run(self.ctx(disk, out=self.tmp / 'nowhere' / 'T.app'))
        self.assertEqual(disk.probed, [self.games, self.games])


class FullRunWithDefaultPaths(Base):
    """Review point 1: every check derives work and out from the same defaults when they are left None."""

    def test_each_game(self):
        # Inputs that need fonts, so the fonts_cached note proves which work folder was read: a TOTK base NSP,
        # and a BotW card image (BotW always needs fonts; a BotW NSP would also need an update).
        nsp = self.games / 'g.nsp'
        nsp.write_bytes(b'PFS0' + bytes(16))
        for spec, game, work_name, app in ((TOTK, nsp, 'TOTK_build', 'Tears of the Kingdom.app'),
                                           (BOTW, self.game, 'BotW_build', 'Breath of the Wild.app')):
            with self.subTest(spec=spec.id):
                self.game = game
                fonts = self.games / work_name / checks_inputs.FONT_CACHE
                fonts.mkdir(parents=True, exist_ok=True)
                for name in (*checks_inputs.font_file_names(REPO), 'manifest.json'):
                    (fonts / name).write_bytes(b'f')
                default_disk, explicit_disk = RecordingDisk(), RecordingDisk()
                default = run_checks(INPUT_CHECKS, self.ctx(default_disk, spec))
                explicit = run_checks(INPUT_CHECKS, self.ctx(explicit_disk, spec, work=self.games / work_name,
                                                             out=self.games / app))
                self.assertEqual(default, explicit)
                self.assertEqual(default_disk.probed, explicit_disk.probed)
                self.assertEqual(default_disk.probed, [self.games, self.games, self.games])
                rules = {r.id: r for r in default}['input_rules']
                self.assertEqual((rules.status, rules.detail), (Status.OK, 'fonts_cached'))   # the default work's cache


class DriftAgainstBuildApp(unittest.TestCase):
    def test_thresholds_and_cache_paths_are_still_there(self):
        source = (REPO / 'build_app.py').read_text()
        for text in ('free<40 and not base_cached', 'free<25 and not base_cached', '.free / 2**30 < 22',
                     "'totk/extracted/v0/romfs/romfs.bin'", "f'totk/extracted/{version}/romfs/romfs.bin'",
                     "'totk/extracted/firmware/shared-fonts'"):
            with self.subTest(text=text):
                self.assertIn(text, source, f'build_app.py changed: {text!r} is gone; update checks_inputs.py')
        self.assertEqual(DiskSpace.REQUIRED_GB, {'totk': (25, 40), 'botw': (22, 22)})


if __name__ == '__main__':
    unittest.main()

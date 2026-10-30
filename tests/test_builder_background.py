"""tools/builder/background.py (S4.3, K2 and K4): the copy of the player's background image and the PNG drawn from it
by macOS's sips, in a temporary folder. The images are made here: a PNG written byte by byte, a JPEG by sips."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zlib

from tools.builder import background
from tools.builder.background import SIPS, Background, focus, remove, save, saved
from tools.builder.ports import CommandResult, CommandRunner, Outcome, RealClock

SCREEN = (64, 36)


def write_png(path, width, height):
    raw = b''.join(b'\x00' + b'\xff\x00\x00' * width for _ in range(height))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
    return path


@unittest.skipUnless(Path(SIPS).exists(), 'needs macOS sips')
class Copy(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp)
        self.folder = self.tmp / 'Builder'
        self.runner = CommandRunner(RealClock())

    def png(self, name, width=160, height=40):
        return write_png(self.tmp / name, width, height)

    def names(self):
        return sorted(p.name for p in self.folder.iterdir())

    def dims(self, path):
        return background._dims(self.runner, path)

    def test_nothing_saved(self):
        self.assertEqual(saved(self.runner, self.folder, SCREEN), Background(None))

    def test_a_jpeg_is_copied_and_drawn_as_a_png_covering_the_screen(self):
        source = self.tmp / 'Mine.JPG'
        subprocess.run([SIPS, '-s', 'format', 'jpeg', str(self.png('m.png')), '--out', str(source)],
                       check=True, capture_output=True)
        result = save(self.runner, self.folder, source, SCREEN)
        data = source.read_bytes()
        source.unlink()                                                     # the original can go
        self.assertEqual(result, Background(self.folder / 'background.jpg', self.folder / 'background-shown-64x36.png'))
        self.assertEqual((result.path.read_bytes(), self.dims(result.shown)), (data, SCREEN))
        self.assertEqual(saved(self.runner, self.folder, SCREEN), result)

    def test_a_new_screen_size_draws_it_again(self):
        save(self.runner, self.folder, self.png('a.png', 40, 160), SCREEN)
        result = saved(self.runner, self.folder, (30, 50))
        self.assertEqual((result.shown.name, self.dims(result.shown)), ('background-shown-30x50.png', (30, 50)))
        self.assertEqual(self.names(), ['background-shown-30x50.png', 'background.png'])

    def test_a_new_choice_replaces_the_old_one_and_settings_stay(self):
        self.folder.mkdir()
        (self.folder / 'settings.json').write_text('{}')
        save(self.runner, self.folder, self.png('a.png'), SCREEN)
        save(self.runner, self.folder, self.png('b.tiff'), SCREEN)
        self.assertEqual(self.names(), ['background-shown-64x36.png', 'background.tiff', 'settings.json'])
        remove(self.runner, self.folder)
        self.assertEqual(self.names(), ['settings.json'])

    def test_refusals_keep_the_earlier_one(self):
        first = save(self.runner, self.folder, self.png('a.png'), SCREEN)
        notes = self.tmp / 'notes.txt'
        notes.write_text('text')
        broken = self.tmp / 'broken.jpg'
        broken.write_text('not an image')
        self.assertEqual(save(self.runner, self.folder, notes, SCREEN), Background(first.path, first.shown, 'kind'))
        self.assertEqual(save(self.runner, self.folder, broken, SCREEN),
                         Background(first.path, first.shown, 'unreadable'))
        with patch.object(background, 'MAX_BYTES', 4):
            self.assertEqual(save(self.runner, self.folder, self.png('c.png'), SCREEN).refused, 'too_big')
        self.assertEqual(self.names(), ['background-shown-64x36.png', 'background.png'])

    def test_a_failed_copy_keeps_the_earlier_one_and_leaves_no_partial(self):
        first = save(self.runner, self.folder, self.png('a.png'), SCREEN)
        with patch.object(background.shutil, 'copyfile', side_effect=OSError('disk full')):
            self.assertRaises(OSError, save, self.runner, self.folder, self.png('b.png'), SCREEN)
        self.assertEqual(saved(self.runner, self.folder, SCREEN), first)
        self.assertEqual(self.names(), ['background-shown-64x36.png', 'background.png'])

    def test_a_saved_copy_sips_cannot_read_is_reported(self):
        self.folder.mkdir()
        (self.folder / 'background.png').write_text('damaged')
        self.assertEqual(saved(self.runner, self.folder, SCREEN),
                         Background(self.folder / 'background.png', None, 'unreadable'))
        self.assertEqual(self.names(), ['background.png'])

    def test_the_focus_is_kept_checked_and_removed_with_the_picture(self):         # S4.4
        first = save(self.runner, self.folder, self.png('a.png'), SCREEN)
        self.assertEqual(first.focus, 'center')
        self.assertEqual(focus(self.runner, self.folder, SCREEN, 'right'), Background(first.path, first.shown,
                                                                                      focus='right'))
        self.assertEqual(saved(self.runner, self.folder, SCREEN).focus, 'right')
        self.assertRaises(ValueError, focus, self.runner, self.folder, SCREEN, 'top')
        remove(self.runner, self.folder)
        self.assertEqual(self.names(), [])

    def test_the_drawing_keeps_the_proportions(self):                         # S4.4b, E5
        calls = []
        class Recorder:
            def run(self, argv, timeout_s):
                calls.append(argv)
                out = 'pixelWidth: 400\npixelHeight: 100\n' if '-g' in argv else ''
                return CommandResult(Outcome.OK, 0, out, '', 0.0, 1, True)
        background._draw(Recorder(), self.tmp / 'x.png', self.tmp / 'y.png', (64, 36))
        draw = calls[1]
        high, wide = (int(v) for v in draw[draw.index('--resampleHeightWidth') + 1:][:2])
        self.assertEqual((high, wide), (36, 144))                    # 400x100 scaled to cover 64x36, not stretched
        self.assertEqual(draw[draw.index('--cropToHeightWidth') + 1:][:2], ['36', '64'])

    def test_a_missing_source_raises(self):
        self.assertRaises(FileNotFoundError, save, self.runner, self.folder, self.tmp / 'gone.png', SCREEN)


if __name__ == '__main__':
    unittest.main()

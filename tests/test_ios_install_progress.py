"""The iPhone / iPad install shows its progress (user 2026-11-03: "每次 安装ios的时候 都要 给用户 显示 进度 包括 传 着色器
传 游戏本体"): install_device.py's Progress lines for the game data and the shader pack, and the pack's total, which
must be the bytes write_pack really sends or the percentage would be wrong."""
import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'games/totk/ios/tools/install_device.py'
spec = importlib.util.spec_from_file_location('totk_install_device_progress', SOURCE)
install_device = importlib.util.module_from_spec(spec)
spec.loader.exec_module(install_device)


class InstallProgress(unittest.TestCase):
    def test_parts_add_up_to_the_whole(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            p = install_device.Progress('game data', 3 << 30)
            p.skip('part 1/3', 1 << 30)
            p.part('part 2/3', 1 << 30, lambda: None)
            p.part('part 3/3', 1 << 30, lambda: None)
        lines = out.getvalue().strip().splitlines()
        self.assertEqual(len(lines), 3)
        self.assertIn('game data: 1.0 / 3.0 GiB (33%)', lines[0])
        self.assertIn('part 1/3 already there', lines[0])
        self.assertIn('game data: 2.0 / 3.0 GiB (67%)', lines[1])
        self.assertIn('game data: 3.0 / 3.0 GiB (100%)', lines[2])
        self.assertNotIn('\r', out.getvalue(), 'a log (not a terminal) gets whole lines only')

    def test_speed_and_time_left_once_measured(self):
        p = install_device.Progress('shaders', 4 << 30)
        p.t0 -= 10; p.copied = p.done = 1 << 30   # 1 GiB sent in 10 s
        p.t_sent = p.t0 + 10
        line = p.line(p.done)
        self.assertIn('102 MB/s', line)
        self.assertIn('about 30 s left', line)

    def test_pack_total_is_what_write_pack_sends(self):
        with tempfile.TemporaryDirectory() as d:
            seed, out = Path(d) / 'seed', Path(d) / 'out'
            seed.mkdir(); out.mkdir()
            (seed / 'a_b.metal').write_bytes(b'x' * 1000)
            (seed / 'bé.json').write_bytes(b'{}')
            names = sorted(p.name for p in seed.iterdir())
            parts = []
            install_device.write_pack(seed, names, out, 'f', 512, parts.append)
            sent = sum(p.stat().st_size for p in parts)
            source = SOURCE.read_text()
            self.assertIn("pack = 9 + sum(2 + len(n.encode()) + 8 + (seed/n).stat().st_size for n in names) + 2", source)
            expected = 9 + sum(2 + len(n.encode()) + 8 + (seed / n).stat().st_size for n in names) + 2
            self.assertEqual(sent, expected)
            self.assertEqual(len(parts), (expected + 511) // 512)

    def test_both_copies_report_through_it(self):
        source = SOURCE.read_text()
        self.assertIn("prog = Progress('game data', size)", source)
        self.assertIn("prog = Progress('shaders', pack)", source)
        self.assertIn("prog.part(f'part {i + 1}/{parts}', length, lambda: copy(part, part.name))", source)
        self.assertIn("prog.part(f'part {made[0]}/{parts}', n, lambda: copy(local, local.name))", source)


if __name__ == '__main__':
    unittest.main()

"""The Smash gap report: warnings, stops and GPU placeholders from the host's own logs (Q60)."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]   # games/ssbu
SCRIPT = GAME/'scripts/gap_report.py'
spec = importlib.util.spec_from_file_location('gap_report', SCRIPT)
gap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gap)


def write_log(folder, name, text, mtime):
    p = Path(folder)/name
    p.write_text(text)
    os.utime(p, (mtime, mtime))
    return p


def run(*args):
    return subprocess.run([sys.executable, '-I', str(SCRIPT), *args], capture_output=True, text=True)


class GapReport(unittest.TestCase):
    def test_folding_keeps_shapes_and_cuts_the_next_message(self):
        a = gap.fold('SetScissors: differing scissor 1 ignored (single-scissor replay)[totk] AllocateMemoryPages address=90920d0000')
        b = gap.fold('SetScissors: differing scissor 1 ignored (single-scissor replay)')
        self.assertEqual(a, b)
        self.assertEqual(gap.fold('texture ser=8754 356x240 at 0x90a4eff000 (3 times)'), 'texture ser=<n> 356x240 at <addr> (<n> times)')
        copy = '贴图→贴图拷贝未实现的形态：1920x1080x1→960x540x1 标志 0 格式 260→260'
        self.assertNotEqual(gap.fold(copy), gap.fold(copy.replace('960x540', '384x216')))   # sizes tell shapes apart

    def test_warnings_and_stops_of_one_log(self):
        text = ('[dm] 【红】a gap 0x90a4eff000\n[dm] 【红】a gap 0x90a4ef0000\n'
                '[totk-ipc] sm: service "caps:su" not implemented\n[totk] caller sdk+0x1\n[totk] FAIL: IPC service not implemented\n')
        self.assertEqual(gap.warnings(text), ['a gap <addr>'])
        self.assertEqual(gap.stops(text), ['IPC service not implemented — [totk-ipc] sm: service "caps:su" not implemented'])
        self.assertEqual(gap.tracked(gap.stops(text)[0]), 'Q54 ✅')

    def test_placeholders_are_the_names_without_a_host_function(self):
        with tempfile.TemporaryDirectory() as tmp:
            gfx = Path(tmp)
            (gfx/'names.txt').write_text('nvnA\nnvnB\nnvnC\nnvnD\n')
            (gfx/'hle_nvn.c').write_text('NVN(nvnA, void, int x) {}\nstatic const char *const g_nops[] = {\n    "nvnC",\n};\n')
            (gfx/'nvn_mtl.m').write_text('NVN_X(nvnD, void) {}\n')
            self.assertEqual(gap.placeholders(gfx), (['nvnB'], 4))

    def test_session_mode_names_only_what_is_new(self):
        with tempfile.TemporaryDirectory() as tmp:
            logs = Path(tmp)
            write_log(logs, 'old.log', '[dm] 【红】known gap\n', 1000)
            new = write_log(logs, 'new.log', '[dm] 【红】known gap\n[dm] 【红】fresh gap\n', 2000)
            done = run('--logs', str(logs), '--gfx', str(GAME/'third_party/d2r_gfx/gfx/nvn'), '--session', str(new))
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn('1 neue Warnungen', done.stdout)
            self.assertIn('NEU fresh gap', done.stdout)
            self.assertNotIn('NEU known gap', done.stdout)

    def test_a_count_the_runtime_disagrees_with_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            logs, gfx = Path(tmp)/'logs', Path(tmp)/'gfx'
            logs.mkdir(); gfx.mkdir()
            (gfx/'names.txt').write_text('nvnA\nnvnB\n')
            (gfx/'hle_nvn.c').write_text('NVN(nvnA, void) {}\n')
            write_log(logs, 'a.log', '[nvn] 空设备：名字 2，已实现 1（语义 1 + 空实现 0），其余 1 个接报红桩\n', 1000)
            self.assertEqual(run('--logs', str(logs), '--gfx', str(gfx)).returncode, 0)
            write_log(logs, 'b.log', '[nvn] 空设备：名字 2，已实现 0（语义 0 + 空实现 0），其余 2 个接报红桩\n', 2000)
            done = run('--logs', str(logs), '--gfx', str(gfx))
            self.assertEqual(done.returncode, 1)
            self.assertIn('ABWEICHUNG', done.stdout)


if __name__ == '__main__':
    unittest.main()

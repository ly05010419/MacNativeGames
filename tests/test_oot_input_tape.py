"""Ocarina of Time records the user's route and plays it again (games/oot/runtime/input_tape.h, 2026-11-18, user:
"不是让你录制了吗 我的路线和操作", "用我的 路线 运行").

The probe (tests/oot_input_tape_probe.c) records a short route (a button on the title, then walking in the world entered
at frame 100), then replays it with the world entered at frame 150: the title's input keeps its frames from the start,
the world's are counted from the world's first frame, the real input is replaced, and the game is asked to close
120 frames after the recording ends."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'games/oot/runtime'


class InputTape(unittest.TestCase):
    def test_record_then_replay_with_a_slower_load(self):
        with tempfile.TemporaryDirectory() as d:
            exe, tape = Path(d) / 'probe', Path(d) / 'route.tape'
            subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(RUNTIME),
                            str(ROOT / 'tests/oot_input_tape_probe.c'), '-o', str(exe)], check=True)
            env = dict(os.environ, OOT_INPUT_RECORD=str(tape))
            subprocess.run([str(exe), 'record'], env=env, check=True, capture_output=True)
            lines = [l for l in tape.read_text().splitlines() if l and not l.startswith('#')]
            self.assertIn('B 10 1 0 0 0 0', lines)          # the title's button, by frames since the start
            self.assertIn('W 0 0 0 0 0 0', lines)           # the state again where the world begins
            self.assertIn('W 5 0 20000 0 0 0', lines)       # walking, by frames since the world began
            self.assertIn('H W 600', lines)                 # a heartbeat every 600 frames
            env = dict(os.environ, OOT_INPUT_REPLAY=str(tape))
            run = subprocess.run([str(exe), 'replay'], env=env, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('closed at frame', run.stdout)

    def test_the_game_reads_the_tape_and_notes_the_world(self):
        source = (RUNTIME / 'input.c').read_text()
        self.assertIn('#include "input_tape.h"', source)
        self.assertIn('input_tape(&state.buttons,&state.lx,&state.ly,&state.rx,&state.ry);', source)
        mtl = (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
        self.assertIn('uint64_t mtl_world_frame(void)', mtl)


if __name__ == '__main__':
    unittest.main()

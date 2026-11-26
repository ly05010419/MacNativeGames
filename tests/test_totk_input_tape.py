"""Tears of the Kingdom and Breath of the Wild record the user's route and play it again (shared/runtime/input_tape.h, from
Ocarina of Time; 2026-11-26, user: "vc consegue gravar o que eu ando? assim vou longe andando"): an A/B compares two
builds on the route the user played, not on a scripted walk that ran Link into a rock in Kakariko.

The probe (tests/oot_input_tape_probe.c) records with TOTK_INPUT_RECORD and replays with TOTK_INPUT_REPLAY: the names the
shared runtime reads in every game. The game must call the tape where it hands the controller state over, after the
keyboard, the controller and the input file are merged, or a replay would not drive what the game reads."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class InputTapeInTotk(unittest.TestCase):
    def test_record_then_replay_under_the_totk_names(self):
        with tempfile.TemporaryDirectory() as d:
            exe, tape = Path(d) / 'probe', Path(d) / 'route.tape'
            subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'shared/runtime'),
                            str(ROOT / 'tests/oot_input_tape_probe.c'), '-o', str(exe)], check=True)
            clean = {k: v for k, v in os.environ.items() if not k.startswith(('OOT_INPUT_', 'TOTK_INPUT_'))}
            subprocess.run([str(exe), 'record'], env=dict(clean, TOTK_INPUT_RECORD=str(tape)), check=True, capture_output=True)
            lines = [l for l in tape.read_text().splitlines() if l and not l.startswith('#')]
            self.assertIn('W 5 0 20000 0 0 0', lines)
            run = subprocess.run([str(exe), 'replay'], env=dict(clean, TOTK_INPUT_REPLAY=str(tape)), capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('closed at frame', run.stdout)

    def test_the_game_hands_the_tape_the_merged_state(self):
        source = (ROOT / 'games/totk/runtime/input.c').read_text()
        self.assertIn('#include "input_tape.h"', source)
        pad = source[source.index('static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style){'):]
        pad = pad[:pad.index('return 1;}')]
        tape = pad.index('input_tape(&state.buttons,&state.lx,&state.ly,&state.rx,&state.ry);')
        self.assertLess(pad.index('runtime_gamepad_read(&pad)'), tape, 'the controller is merged before the tape')
        self.assertLess(tape, pad.index('*out=state;'), 'the tape decides what the game reads')

    def test_one_file_for_every_game(self):
        for game in ('totk', 'oot'):
            link = ROOT / f'games/{game}/runtime/input_tape.h'
            self.assertTrue(link.is_symlink(), game)
            self.assertEqual(link.resolve(), (ROOT / 'shared/runtime/input_tape.h').resolve(), game)


if __name__ == '__main__':
    unittest.main()

"""Pipeline prediction gives a draw's key the depth / stencil planes of its pass, decided by the replay's own rule.

2026-11-16, Ocarina of Time 1.0.1 on the Mac with the app's lists in the archive only: pso_predict_rest compiled 8
depth-less guesses 64 times in a 4.5-minute route and none was ever drawn, because the replay attaches depth for a whole
pass (pass_ds_need / ds_scan) when any of its draws tests depth. The rule lives in nvn_pred_pass.h and both the replay's
look-ahead and prediction use it; dm_nvn_probe_prediction (make probe-gfx) checks its cases. These checks keep the two
from drifting apart again and keep the per-draw guesses out.
"""
import re
import unittest
from pathlib import Path

NVN = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn'
SOURCE = (NVN / 'nvn_mtl.m').read_text(encoding='utf-8')
HEADER = (NVN / 'nvn_pred_pass.h').read_text(encoding='utf-8')


def body(name):
    match = re.search(rf'static \w+ {name}\([^;{{]*\)\s*\{{', SOURCE)   # the definition, not a declaration
    if not match: raise AssertionError(f'{name} is not defined')
    depth, i = 0, match.end() - 1
    for j in range(i, len(SOURCE)):
        depth += {'{': 1, '}': -1}.get(SOURCE[j], 0)
        if depth == 0: return SOURCE[i:j + 1]
    raise AssertionError(f'{name} has no end')


class PredictionPassPlanes(unittest.TestCase):
    def test_no_per_draw_depth_guesses(self):
        self.assertNotIn('PRED_DEPTH', SOURCE)
        self.assertNotIn('pred_guess_wanted', SOURCE)
        rest = body('pso_predict_rest')
        self.assertNotRegex(rest, r'for \(int v = 0; v < 4; v\+\+\)', 'prediction tries attach modes per draw again')

    def test_prediction_follows_the_replays_passes(self):
        rest = body('pso_predict_rest')
        for step in ('pred_pass_current', 'pred_pass_set_rt', 'pred_pass_end', 'pred_pass_clear', 'pred_pass_draw',
                     'ds_op_ends_pass', 'ds_need_planes', 'ds_draw_need'):
            self.assertIn(step, rest, f'pso_predict_rest does not call {step}')
        # a pass's held keys are compiled when it is decided, also at the end of the scan
        self.assertIn('cur.waited', rest)
        # targets prediction cannot read give no keys (a key with zeroed formats is never drawn)
        self.assertIn('if (cf_src == 2) continue;', rest)

    def test_replay_look_ahead_uses_the_same_rule(self):
        scan = body('ds_scan')
        self.assertIn('ds_op_ends_pass(h.op)', scan)
        self.assertIn('ds_draw_need(ds, 0)', scan)
        self.assertNotRegex(scan, r'case OP_SET_RT:', 'ds_scan lists the pass-ending ops itself again')

    def test_rule_ops_match_the_replay(self):
        ops = set(re.findall(r'case (OP_[A-Z0-9_]+):', HEADER.split('ds_op_ends_pass', 1)[1].split('}', 1)[0]))
        self.assertEqual(ops, {'OP_SET_RT', 'OP_CLEAR_BUF', 'OP_COPY_B2B', 'OP_UPDATE_UB', 'OP_COPY_B2T', 'OP_COPY_T2B',
                               'OP_COPY_T2T', 'OP_CLEAR_TEX', 'OP_DISPATCH'})

    def test_probe_checks_the_rule(self):
        probe = SOURCE[SOURCE.index('void dm_nvn_probe_prediction(void) {'):]
        self.assertIn('pipeline prediction left depth out of a pass whose later draw tests depth', probe)
        games = NVN.parents[4]   # games/
        for game in ('totk', 'oot'):
            text = (games / game / 'runtime/gfx_probe.c').read_text(encoding='utf-8')
            self.assertIn('prediction_gives_keys_the_planes_of_their_pass', text, game)


if __name__ == '__main__':
    unittest.main()

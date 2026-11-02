"""Check that delayed GPU samples stay assigned to the right timing window."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_run import analyze, distribution


class TimingTests(unittest.TestCase):
    def test_percentiles_and_exact_budget_denominator(self):
        r=distribution([0,10,20,30,float('nan'),-1],20)
        self.assertEqual(r['samples'],4)
        self.assertEqual(r['p50'],15)
        self.assertAlmostEqual(r['p95'],28.5)
        self.assertEqual(r['over_budget_fraction'],.25)

    def test_delayed_gpu_rows_and_window_boundaries(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            (root/'frames.tsv').write_text('frame\tname\tvalue\n1\t@t_s\t10\n1\t@frame_ms\t34\n'
                '2\t@t_s\t20\n2\t@frame_ms\t40\n1\t@gpu_ms\t5\n2\t@gpu_ms\t8\n1\t@pso_new\t2\n')
            (root/'host.log').write_text('[RED] classified failure\n')
            (root/'pso-record.tsv').write_text('S\t0123456789abcdef_b\t0\n')
            r=analyze(root,10,20,30)
            self.assertEqual(r['frames'],1)
            self.assertEqual(r['metrics']['@gpu_ms']['p50'],5)
            self.assertEqual(r['pipeline_creations'],2)
            self.assertEqual(len(r['whole_run_red_lines']),1)
            self.assertEqual(r['runtime_used_keys'],['0123456789abcdef'])


if __name__=='__main__':unittest.main()

#!/usr/bin/env python3
# dm/tools/gpuprof.py: the host's [gpuprof] and [ab] lines (DM_NVN_GPUPROF, DM_AB) parsed from a synthetic log, the per-frame totals, the groupings and the A/B medians
import contextlib, importlib.util, io, os, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('gpuprof', REPO / 'dm/tools/gpuprof.py'); gpuprof = importlib.util.module_from_spec(spec); spec.loader.exec_module(gpuprof)

P = '[dm] [nvn-mtl] [gpuprof] '


def rp(f, i, us, pas, w, h, nc, d, draws, start, ld, st, cf, df, seg=0):
    return f'{P}F{f} {i:4d} {us:8.1f} us render pass {pas} {w}x{h} colors {nc} depth {d} loads {1 if ld else 0} draws {draws} start +{start:.1f} seg {seg} load {ld} KB store {st} KB fmt {cf}/{df}'


LOG = [
    'boot: env DM_NVN_GPUPROF=every:600 (Documents/env.txt)',
    # frame 600: a G-buffer (4 colours + depth), a full-size lighting pass, a shadow map, a tiny pass and a copy; 20 us idle before the lighting pass
    rp(600, 0, 400.0, 10, 1600, 900, 4, 1, 300, 0.0, 0, 28125, 70, 260),
    rp(600, 1, 100.0, 11, 1600, 900, 1, 0, 1, 420.0, 5625, 5625, 115, 0),
    rp(600, 2, 200.0, 12, 2048, 2048, 0, 1, 500, 520.0, 0, 16384, 0, 252),
    rp(600, 3, 10.0, 13, 32, 32, 1, 0, 1, 720.0, 4, 4, 70, 0),
    f'{P}F600    4     30.0 us compute / copy after pass 13 start +730.0 seg 1 copies 3 (uniform updates 2) dispatches 1',
    f'{P}frame 600: render 0.71 ms, compute / copy 0.03 ms over 5 encoders (4 render passes, attachments load 5.5 MB store 49.0 MB), first start to last end 0.76 ms; clock proxy: x',
    'an unrelated line [gpuprof] that matches nothing',
    # frame 1200: the same with a slower G-buffer and no copy
    rp(1200, 0, 600.0, 20, 1600, 900, 4, 1, 320, 0.0, 0, 28125, 70, 260),
    rp(1200, 1, 100.0, 21, 1600, 900, 1, 0, 1, 600.0, 5625, 5625, 115, 0),
    rp(1200, 2, 200.0, 22, 2048, 2048, 0, 1, 500, 700.0, 0, 16384, 0, 252),
    rp(1200, 3, 10.0, 23, 32, 32, 1, 0, 1, 900.0, 4, 4, 70, 0),
    f'{P}frame 1200: render 0.91 ms, compute / copy 0.00 ms over 4 encoders (4 render passes, attachments load 5.5 MB store 49.0 MB), first start to last end 0.91 ms',
    # an older host's line without the ledger and formats still parses
    f'{P}F1800    0    50.0 us render pass 30 800x450 colors 1 depth 0 loads 0 draws 2 start +0.0 seg 0',
    '[dm] [nvn-mtl] [ab] phase 0 base: 90 frames, GPU p50 9.00 / p90 10.00 / mean 9.10 ms, frame p50 16.60 / p90 17.00 / mean 16.70 ms; per frame: attachments load 300 MB store 400 MB, passes 150.0',
    '[dm] [nvn-mtl] [ab] phase 1 BARRIER: 90 frames, GPU p50 8.00 / p90 9.00 / mean 8.10 ms, frame p50 16.60 / p90 16.90 / mean 16.65 ms; per frame: attachments load 300 MB store 400 MB, passes 150.0',
    '[dm] [nvn-mtl] [ab] phase 2 base: 90 frames, GPU p50 7.00 / p90 8.00 / mean 7.10 ms, frame p50 16.70 / p90 17.10 / mean 16.80 ms; per frame: attachments load 300 MB store 400 MB, passes 150.0',
    '[dm] [nvn-mtl] [ab] phase 3 BARRIER: 90 frames, GPU p50 6.50 / p90 7.50 / mean 6.60 ms, frame p50 16.60 / p90 16.80 / mean 16.62 ms; per frame: attachments load 300 MB store 400 MB, passes 150.0',
    '[dm] [nvn-mtl] [ab] phase 4 base: 90 frames, GPU p50 7.20 / p90 8.10 / mean 7.30 ms, frame p50 16.70 / p90 17.00 / mean 16.75 ms; per frame: attachments load 300 MB store 400 MB, passes 150.0',
]


class GpuProf(unittest.TestCase):
    def setUp(self):
        self.frames, self.flines = gpuprof.parse(LOG)

    def test_parse(self):
        self.assertEqual(list(self.frames), [600, 1200, 1800])
        self.assertEqual(len(self.frames[600]), 5); self.assertEqual(len(self.frames[1200]), 4)
        g = self.frames[600][0]
        self.assertEqual((g.kind, g.w, g.h, g.colors, g.depth, g.draws, g.store_kb, g.cfmt, g.dfmt), ('R', 1600, 900, 4, 1, 300, 28125, 70, 260))
        c = self.frames[600][4]
        self.assertEqual((c.kind, c.pass_, c.copies, c.ubd, c.dispatches, c.seg), ('C', 13, 3, 2, 1, 1))
        old = self.frames[1800][0]
        self.assertEqual((old.load_kb, old.store_kb, old.cfmt), (0, 0, 0))
        self.assertEqual(self.flines[600], {'render_ms': 0.71, 'compute_ms': 0.03, 'encoders': 5, 'span_ms': 0.76})
        self.assertEqual(self.flines[1200]['span_ms'], 0.91)

    def test_frame_stats(self):
        s = gpuprof.frame_stats(self.frames[600])
        self.assertEqual((s['encoders'], s['passes'], s['draws']), (5, 4, 802))
        self.assertAlmostEqual(s['sum_us'], 740.0); self.assertAlmostEqual(s['span_us'], 760.0)
        self.assertAlmostEqual(s['gap_us'], 20.0)   # 400 → 420
        self.assertEqual((s['load_kb'], s['store_kb']), (5629, 50138))

    def test_shapes(self):
        rows, nf = gpuprof.aggregate(self.frames, 'shape', {600, 1200})
        self.assertEqual(nf, 2)
        top = rows[0]
        self.assertEqual(top[0], '1600x900 4c+d RGBA8Unorm/Depth32Float_Stencil8')
        self.assertAlmostEqual(top[1], 1.0); self.assertAlmostEqual(top[2], 500.0); self.assertAlmostEqual(top[3], 310.0)
        names = [r[0] for r in rows]
        self.assertIn('2048x2048 0c+d Depth32Float', names); self.assertIn('1600x900 1c RGBA16Float', names); self.assertIn('compute / copy', names)
        cp = rows[names.index('compute / copy')]; self.assertAlmostEqual(cp[1], 0.5); self.assertAlmostEqual(cp[2], 15.0)

    def test_classes(self):
        rows, nf = gpuprof.aggregate(self.frames, 'class', {600})
        d = {r[0]: r for r in rows}
        self.assertEqual(set(d), {'full, 4 colours + depth', 'full, <=2 draws', 'depth only (shadow maps)', 'tiny (<=64x64)', 'compute / copy'})
        self.assertAlmostEqual(d['full, 4 colours + depth'][2], 400.0)
        # the reference is the largest colour pass, not the 2048x2048 shadow map: an 800x450 pass is half of 1600x900 per axis
        e = self.frames[1800][0]._replace(colors=1, draws=5)
        self.assertEqual(gpuprof.class_key(e, 1600 * 900), 'half, >2 draws'); self.assertEqual(gpuprof.class_key(e, 3200 * 1800), 'quarter, >2 draws')

    def test_ab(self):
        phases = gpuprof.parse_ab(LOG)
        self.assertEqual([p['config'] for p in phases], ['base', 'BARRIER', 'base', 'BARRIER', 'base'])
        r = gpuprof.ab_summary(phases, skip=1)
        self.assertEqual(list(r), ['BARRIER', 'base'])
        self.assertEqual(r['base']['phases'], 2); self.assertAlmostEqual(r['base']['gpu_p50'], 7.1); self.assertAlmostEqual(r['BARRIER']['gpu_p50'], 7.25)
        self.assertEqual(r['base']['passes'], 150.0)

    def test_cli(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'log.txt'; p.write_text('\n'.join(LOG) + '\n')
            for args, want in ((['--by', 'class'], 'full, 4 colours + depth'), (['--timeline', '600'], 'gap 20.0'), (['--ab'], 'BARRIER - base: GPU p50 +0.15 ms'),
                               (['--frames', '600', '--top', '2'], 'other (3 groups)')):
                out = io.StringIO()
                with contextlib.redirect_stdout(out): rc = gpuprof.main([str(p)] + args)
                self.assertEqual(rc, 0, args); self.assertIn(want, out.getvalue(), args)
            err = io.StringIO()
            with contextlib.redirect_stderr(err): self.assertEqual(gpuprof.main([str(p), '--timeline', '7']), 1)


if __name__ == '__main__':
    unittest.main()

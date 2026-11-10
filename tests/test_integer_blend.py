"""nvn_mtl.m as written (read, not run): pso_desc turns blending on only for an output whose format is not integer, and
the probe that shows it on real pipelines (probe-gpu-fout, under the Metal debug layer, in a build) stays wired in.
Metal aborts the process on blending over an integer format."""
from pathlib import Path
import unittest

NVN = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn'


class IntegerBlend(unittest.TestCase):
    code = (NVN / 'nvn_mtl.m').read_text()

    def test_the_pipeline_descriptor_asks_the_format_first(self):
        body = self.code[self.code.index('static MTLRenderPipelineDescriptor *pso_desc('):]
        site = body[:body.index('c.blendingEnabled = YES;')]
        self.assertIn("k->blend_en >> l & 1 && pfmt_class((MTLPixelFormat)k->cfmt[i]) == 'f'", site[site.rindex('if (l != 0xff'):])

    def test_the_ui_layer_blends_on_a_float_attachment_only(self):
        """The second place that blends: attachment 7, always the MetalFX UI layer's RGBA16Float."""
        self.assertEqual(self.code.count('cfmt[7] = '), 1)
        self.assertIn('g_pass.cfmt[7] = (uint32_t)MTLPixelFormatRGBA16Float;', self.code)

    def test_the_probe_runs_with_the_fragment_output_probe(self):
        self.assertIn('#include "nvn_integer_blend_probe.h"', self.code)
        self.assertIn('return checked + integer_blend_probe(vs);', self.code)


if __name__ == '__main__':
    unittest.main()

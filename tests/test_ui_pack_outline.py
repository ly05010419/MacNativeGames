"""Exact topology and coverage of original pixel-boundary extraction."""
import unittest
import importlib.util

HAVE=all(importlib.util.find_spec(n) for n in ('numpy','scipy','PIL','contourpy'))
@unittest.skipUnless(HAVE,'use the tools/deps ui-pack Python')
class PixelOutlines(unittest.TestCase):
    def test_exact_coverage_including_diagonal_contacts_and_holes(self):
        import numpy as np
        from tools.ui_pack.outline import pixel_boundaries
        from tools.ui_pack.raster import coverage
        rng=np.random.default_rng(123)
        masks=[np.array([(bits>>i)&1 for i in range(4)]).reshape(2,2) for bits in range(16)]
        masks += [rng.integers(0,2,size=(12,13)) for _ in range(30)]
        masks += [np.ones((8,9)),np.pad(np.zeros((4,5)),2,constant_values=1)]
        for mask in masks:
            rings=pixel_boundaries(mask)
            got=coverage(rings,*mask.shape,1,4) if rings else np.zeros_like(mask)
            np.testing.assert_array_equal(got,mask)

    def test_contours_keep_holes_and_border_continuation(self):
        import numpy as np
        from tools.ui_pack.recon import trace, pixel_outline
        from tools.ui_pack.raster import coverage
        mask=np.ones((12,13));mask[3:9,4:10]=0
        rings=pixel_outline(mask)
        self.assertEqual(len(rings),2)
        np.testing.assert_array_equal(coverage(rings,*mask.shape,1,8),mask)
        field=np.repeat(np.repeat(mask,8,axis=0),8,axis=1)
        rings=trace((np.pad(field,16,mode='edge'),2),8)
        self.assertEqual(len(rings),2)
        np.testing.assert_allclose(coverage(rings,*mask.shape,1,16),mask,atol=.015)

if __name__=='__main__':unittest.main()

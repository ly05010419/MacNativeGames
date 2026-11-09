"""Which levels a mip-chain pass samples, from the production functions with a CPU sampler registry; no GPU or game data."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / "games/totk/third_party/d2r_gfx/gfx/nvn"

PROBE = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <math.h>
#include "nvn_obj.h"
#include "nvn_refl.h"
@interface NvShader : NSObject { @public nvn_refl rf; } @end
@implementation NvShader @end
static samp_par g_sreg[4]; static uint8_t g_sreg_ok[4]; static int g_nsreg = 4;
/* PRODUCTION */
static int check(int base, int n, int lv, int pin, int from, int count) {
    int f = -1, c = -1, r = mip_feedback_levels(base, n, lv, pin, &f, &c);
    return count ? r && f == from && c == count : !r;
}
int main(void) { @autoreleasepool {
    // Tears of the Kingdom's glow: 320x180, 5 levels. 1e774a23 draws level n from n - 1, 327ae8bb level n from n + 1
    assert(check(0, 5, 2, 1, 1, 1));
    assert(check(0, 5, 3, 4, 4, 1));
    assert(check(0, 5, 1, 2, 2, 1));
    assert(check(0, 5, 0, 1, 1, 1));   // an upsample into the view's base level
    assert(check(0, 5, 3, 9, 4, 1));   // past the view's levels: its last one
    assert(check(1, 4, 3, 3, 4, 1));   // a view with a base: the pin counts from it
    assert(check(0, 5, 2, 2, 0, 2));   // pinning the attached level itself: the levels below
    assert(check(0, 5, 3, -1, 0, 3));  // no pin: the levels below
    assert(check(1, 4, 3, -1, 1, 2));
    assert(check(0, 5, 0, -1, 0, 0));  // no pin, rendering the view's base: left as it is
    assert(check(0, 5, 0, 0, 0, 0));
    assert(check(2, 2, 1, -1, 0, 0));  // the attached level is outside the view: not feedback
    assert(check(2, 2, 4, 1, 0, 0));

    NvShader *sh = [NvShader new]; sh->rf.nb = 2;
    sh->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 3}; sh->rf.b[1] = (nvn_rbind){.kind = NVN_RB_SAMPT, .reg = 3};
    g_sreg[1] = (samp_par){.minf = 2, .lod_min = 4, .lod_max = 4}; g_sreg_ok[1] = 1;
    assert(mip_pinned_level(sh, 3, 1) == 4);
    assert(mip_pinned_level(sh, 2, 1) == -1);   // no combined handle in this slot: its sampler is not known here
    assert(mip_pinned_level(sh, 3, 0) == -1 && mip_pinned_level(sh, 3, 2) == -1 && mip_pinned_level(sh, 3, 4) == -1);   // none, unregistered, out of range
    g_sreg[1].lod_max = 5; assert(mip_pinned_level(sh, 3, 1) == -1);   // a range
    g_sreg[1].lod_min = g_sreg[1].lod_max = 1.5f; assert(mip_pinned_level(sh, 3, 1) == -1);   // between two levels
    g_sreg[1] = (samp_par){.minf = 1, .lod_min = 4, .lod_max = 4}; assert(mip_pinned_level(sh, 3, 1) == -1);   // no mip filter: the base level
} return 0; }
'''


@unittest.skipUnless(sys.platform == "darwin", "Objective-C probe")
class MipFeedbackView(unittest.TestCase):
    def test_a_pinned_level_is_sampled_alone_on_either_side_of_the_attachment(self):
        source = (NVN / "nvn_mtl.m").read_text()
        production = source[source.index("static int mip_feedback_levels("):source.index("static id<MTLTexture> mip_feedback_view(")]
        with tempfile.TemporaryDirectory() as tmp:
            probe, binary = Path(tmp, "probe.m"), Path(tmp, "probe")
            probe.write_text(PROBE.replace("/* PRODUCTION */", production))
            subprocess.run(["xcrun", "clang", "-std=c11", "-fobjc-arc", "-Wall", "-Wextra", "-Werror", f"-I{NVN}", f"-I{NVN}/include",
                            "-framework", "Foundation", str(probe), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()

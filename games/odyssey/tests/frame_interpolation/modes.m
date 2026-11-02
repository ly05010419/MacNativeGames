/* Compile the actual interpolator, exercise its request/latch lifecycle without GPU encoding. */
#include <assert.h>
#include "../../third_party/d2r_gfx/gfx/nvn/totk_mfx.m"
static int hz;
int mtl_display_hz(void) { return hz; }
int main(void) { @autoreleasepool {
    setenv("TOTK_MFX", "0", 1);
    assert(mfx_enabled()==MFX_OFF);
    mfx_set(-1); assert(mfx_enabled()==MFX_OFF); /* only latch at a game-frame boundary */
    hz=60; mfx_latch(); assert(mfx_enabled()==MFX_OFF && atomic_load(&g_req)==MFX_HALF);
    hz=120; mfx_latch();
    int supported=[MTLFXFrameInterpolatorDescriptor supportsMetal4FX:MTLCreateSystemDefaultDevice()];
    assert(mfx_enabled()==(supported?MFX_HALF:MFX_OFF));
    g.prev_ok=g.hist_ok=1;
    hz=144; mfx_latch(); assert(mfx_enabled()==MFX_OFF && atomic_load(&g_req)==MFX_HALF);
    if(supported) assert(!g.prev_ok && !g.hist_ok);
    hz=120; mfx_latch(); assert(mfx_enabled()==(supported?MFX_HALF:MFX_OFF));
    mfx_set(-1); mfx_latch(); assert(mfx_enabled()==MFX_OFF && atomic_load(&g_req)==MFX_OFF);
    for(int mode=1;mode<MFX_NMODES;mode++) {
        mfx_set(mode); mfx_latch(); assert(atomic_load(&g_req)==MFX_HALF);
        assert(mfx_enabled()==(supported?MFX_HALF:MFX_OFF));
    }
    return 0;
} }

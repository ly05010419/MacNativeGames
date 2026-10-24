#include <assert.h>
#include <stdatomic.h>
#include "totk_mfx_mode.h"
#include "odyssey_mfx.h"

typedef struct { uint64_t addr, size; } p_bind_buf;
static struct { p_bind_buf ubo[1][16]; } S;
static struct { int want_cam; } g_mfx;
static int received;
static float observed[84];
static void pend_materialize(uint64_t a, uint8_t *out, size_t n, int depth) {
    (void)depth; memcpy(out, (void *)(uintptr_t)a, n);
}
static void mfx_note_camera(const float *cam, int n) {
    assert(n == 84); memcpy(observed, cam, sizeof observed); received++;
}
/* The Python driver inserts the production mfx_take_camera here. */
PRODUCTION_CAMERA

int main(void) {
    float h[0x180/4] = {0};
    const float c = cosf(.61f), s = sinf(.61f), aspect = 16.f/9, fov = 1.04719755f;
    float view[12] = {c,0,s,1000, 0,1,0,200, -s,0,c,3000};
    memcpy(h, view, sizeof view);
    float fy = 1/tanf(fov/2), fx = fy/aspect;
    for (int i = 0; i < 4; i++) {
        h[24+i]=view[i]*fx; h[28+i]=view[4+i]*fy;
        h[32+i]=view[8+i]; h[36+i]=-view[8+i];
    }
    h[15]=100; h[19]=200; h[23]=300; h[88]=100; h[89]=100000;
    /* Not fixed to Zelda's slot 1. A short and a malformed buffer precede the camera. */
    S.ubo[0][1]=(p_bind_buf){(uintptr_t)h,16};
    S.ubo[0][7]=(p_bind_buf){(uintptr_t)h,sizeof h};
    mfx_take_camera(); assert(received==1);
    assert(fabsf(observed[70]-fov)<1e-5f && fabsf(observed[62]-aspect)<1e-5f);
    assert(observed[47]==100 && observed[51]==200 && observed[55]==300);
    assert(observed[8]==view[8] && observed[10]==view[10]);
    /* Pending/current uniform bytes, not a cached camera. Invalid input publishes nothing. */
    h[15]=400; mfx_take_camera(); assert(received==2 && observed[47]==400);
    h[28]=NAN; mfx_take_camera(); assert(received==2);
    h[28]=0; memset(h+24,0,16*sizeof(float)); mfx_take_camera(); assert(received==2);
    double a[84]={0}, b[84]={0}; a[10]=b[10]=1;
    a[47]=20; assert(!odyssey_mfx_cut(a,b));
    a[47]=900; assert(odyssey_mfx_cut(a,b));
    a[47]=0; a[10]=0; assert(odyssey_mfx_cut(a,b));
    a[10]=1; a[47]=NAN; assert(odyssey_mfx_cut(a,b));
    for (int req=0;req<MFX_NMODES;req++) for (int hz=0;hz<=240;hz++) {
        assert(odyssey_mfx_mode(req,hz)==(req && hz==120 ? MFX_HALF : MFX_OFF));
        assert(odyssey_frame_period()==1000000000ull/60);
    }
    assert(odyssey_mfx_fxaa("5345f3231650f0b7-variant"));
    assert(!odyssey_mfx_fxaa("addef5ac") && !odyssey_mfx_fxaa(NULL));
    return 0;
}

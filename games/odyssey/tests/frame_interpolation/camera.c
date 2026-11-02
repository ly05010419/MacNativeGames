#include <assert.h>
#include <stdatomic.h>
#include "totk_mfx_mode.h"
#include "odyssey_mfx.h"

#include <stdio.h>
#define MLOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
static _Atomic uint64_t g_cur_frame;
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
static int taa_active(void) { return 0; }   /* key 5 (totk_taa.m) off: the camera goes to frame interpolation only */
static void taa_note_camera(const float *cam, int n) { (void)cam; (void)n; assert(0); }
/* The Python driver inserts the production mfx_take_camera here. */
PRODUCTION_CAMERA

/* Build a consistent rotated affine camera, with independently supplied world position. */
static void camera_position(float h[0x180/4], float x, float y, float z) {
    const float p[3] = {x,y,z};
    for (int r=0;r<3;r++) {
        h[4*r+3] = -(h[4*r]*x + h[4*r+1]*y + h[4*r+2]*z);
        for (int c=0;c<3;c++) h[12+4*r+c] = h[4*c+r];
        h[12+4*r+3] = p[r];
    }
}
int main(void) {
    float h[0x180/4] = {0};
    const float c = cosf(.61f), s = sinf(.61f), aspect = 16.f/9, fov = 1.04719755f;
    float view[12] = {c,0,s,0, 0,1,0,0, -s,0,c,0};
    memcpy(h, view, sizeof view);
    camera_position(h,100,200,300);
    float fy = 1/tanf(fov/2), fx = fy/aspect;
    for (int i = 0; i < 4; i++) {
        h[24+i]=h[i]*fx; h[28+i]=h[4+i]*fy;
        h[32+i]=h[8+i]; h[36+i]=-h[8+i];
    }
    h[88]=100; h[89]=100000;
    /* A lower slot passes the old orthonormal / near-far test but carries an unrelated inverse. */
    float other[0x180/4]; memcpy(other,h,sizeof other); other[15]=-5000;
    S.ubo[0][0]=(p_bind_buf){(uintptr_t)other,sizeof other};
    S.ubo[0][1]=(p_bind_buf){(uintptr_t)h,16};
    S.ubo[0][7]=(p_bind_buf){(uintptr_t)h,sizeof h};
    mfx_take_camera(); assert(received==1);
    assert(fabsf(observed[70]-fov)<1e-5f && fabsf(observed[62]-aspect)<1e-5f);
    assert(observed[47]==100 && observed[51]==200 && observed[55]==300);
    assert(observed[8]==view[8] && observed[10]==view[10]);
    /* Current/pending bytes must include a matching inverse. */
    camera_position(h,400,200,300); mfx_take_camera(); assert(received==2 && observed[47]==400);
    float good[0x180/4]; memcpy(good,h,sizeof good);
    h[15]+=100; mfx_take_camera(); assert(received==2); memcpy(h,good,sizeof h);
    h[12]=0; h[14]=0; mfx_take_camera(); assert(received==2); memcpy(h,good,sizeof h);
    h[28]=NAN; mfx_take_camera(); assert(received==2); memcpy(h,good,sizeof h);
    memset(h+24,0,16*sizeof(float)); mfx_take_camera(); assert(received==2); memcpy(h,good,sizeof h);
    /* Float cancellation at large world positions is still accepted. */
    camera_position(h,1000000,2000000,-3000000); mfx_take_camera(); assert(received==3);
    memcpy(h,good,sizeof h);
    /* Identical camera in another slot is harmless, including a distinct buffer address. */
    memcpy(other,h,sizeof other); mfx_take_camera(); assert(received==4);
    S.ubo[0][0]=(p_bind_buf){0,0};
    S.ubo[0][9]=(p_bind_buf){(uintptr_t)other,sizeof other};
    mfx_take_camera(); assert(received==5);
    /* Two valid but different cameras: neither is published. The earlier slot must not win. */
    camera_position(other,-5000,200,300);
    mfx_take_camera(); assert(received==5);
    S.ubo[0][7]=(p_bind_buf){0,0}; mfx_take_camera(); assert(received==6 && observed[47]==-5000);
    /* Same view with a different projection is ambiguous too. */
    S.ubo[0][7]=(p_bind_buf){(uintptr_t)h,sizeof h};
    memcpy(other,h,sizeof other); other[28]*=2; other[29]*=2; other[30]*=2;
    mfx_take_camera(); assert(received==6);
    double a[84]={0}, b[84]={0}; a[10]=b[10]=1;
    a[47]=20; assert(!odyssey_mfx_cut(a,b));
    a[47]=900; assert(odyssey_mfx_cut(a,b));
    a[47]=0; a[10]=0; assert(odyssey_mfx_cut(a,b));
    a[10]=1; a[47]=NAN; assert(odyssey_mfx_cut(a,b));
    for (int req=0;req<MFX_NMODES;req++) for (int hz=0;hz<=240;hz++)
        assert(odyssey_mfx_mode(req,hz)==(req && hz==120 ? MFX_HALF : MFX_OFF));
    /* The 60+60 pacing itself (a 1/60 s game period on our own clock) is checked by probe-gfx's present-interval probe. */
    /* Display link: 60+60 lifts only the 60 Hz request to the 120 Hz range; off, the request comes back unchanged. */
    for (int want=0;want<=3;want++) for (int m=0;m<MFX_NMODES;m++)
        assert(odyssey_link_mode(want,m)==(want==1 && m==MFX_HALF ? 2 : want));
    /* Scene canvas: the FXAA draw counts only once encoded. Skipped (no pipeline yet / compiling in the background):
       the next draw clears it and the frame has no scene. Encoded: its canvas, until the frame ends. */
    odyssey_scene sc={0};
    odyssey_scene_draw(&sc); odyssey_scene_fxaa(&sc,77);
    odyssey_scene_draw(&sc); assert(!sc.ready && odyssey_scene_drawn(&sc)==0 && !sc.ready);
    odyssey_scene_draw(&sc); odyssey_scene_fxaa(&sc,77); assert(!sc.ready);
    assert(odyssey_scene_drawn(&sc)==77 && sc.ready && !sc.pending);
    odyssey_scene_draw(&sc); assert(sc.ready && odyssey_scene_drawn(&sc)==0 && sc.ready);
    odyssey_scene_frame(&sc); assert(!sc.ready && !sc.pending);
    assert(odyssey_mfx_fxaa("5345f3231650f0b7-variant"));
    assert(!odyssey_mfx_fxaa("addef5ac") && !odyssey_mfx_fxaa(NULL));
    return 0;
}

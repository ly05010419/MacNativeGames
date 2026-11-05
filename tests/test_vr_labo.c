/* Independent renderer policy and same-frame ownership, without a GPU or
 * game process. Build with -DGAME_BOTW=1 and zeldaVR/gfx/vr_labo.c. */
#include "../zeldaVR/gfx/vr_labo.h"
#include "../zeldaVR/gfx/vr_labo_frame.h"
#include "../zeldaVR/gfx/vr_projection.h"
#include "../zeldaVR/runtime/vr_view_math.h"
#include "../zeldaVR/gfx/totk_vr_link.h"
#include "../games/totk/runtime/runtime.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The real font override table is linked below. Its unrelated font-data paths
   must never be entered by the option permission query. */
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
const char *runtime_game_text(const char *file, const char *key) { (void)file; (void)key; abort(); }
uint64_t runtime_game_number(const char *file, const char *key) { (void)file; (void)key; abort(); }
const char *runtime_game_sha256(const char *file, const char *key) { (void)file; (void)key; abort(); }

int main(void) {
    unsetenv("TOTK_VR_CONTROL"); unsetenv("TOTK_LABO_VR"); unsetenv("TOTK_EXPORT_IOS");
    assert(!vr_labo_enabled());
    unsigned char (*permission)(unsigned char) = runtime_font_override("_ZN2nn4pctl27CheckStereoVisionPermissionEb");
    assert(permission && permission == vr_labo_option_permitted);
    assert(!permission(0) && !permission(1));
    setenv("TOTK_VR_CONTROL", "test-control", 1);
    assert(vr_labo_enabled()); /* Our renderer is independent from the game option. */
    setenv("TOTK_LABO_VR", "1", 1);
    assert(vr_labo_enabled());
    assert(!permission(0) && !permission(1));
    setenv("TOTK_EXPORT_IOS", "1", 1);
    assert(!vr_labo_enabled()); vr_labo_begin(); assert(!vr_labo_active());
    unsetenv("TOTK_EXPORT_IOS");
    setenv("TOTK_LABO_VR", "0", 1); assert(vr_labo_enabled());
    setenv("TOTK_LABO_VR", "1", 1); assert(vr_labo_enabled());
    vr_labo_begin();assert(vr_labo_active());
    assert(!permission(0)&&!permission(1));
    unsigned char (*game_mode)(void)=runtime_font_override("_ZN2nn2pl8IsVrModeEv");
    void (*game_begin)(void)=runtime_font_override("_ZN2nn2pl11BeginVrModeEv");
    void (*game_end)(void)=runtime_font_override("_ZN2nn2pl9EndVrModeEv");
    assert(game_mode&&game_begin&&game_end);game_begin();assert(!game_mode());game_end();
    assert(vr_labo_active()); /* Game mode cannot drive our renderer. */
    vr_labo_end();assert(!vr_labo_active());

    assert(vr_labo_mask_pair("1f037afbe71a66f468ae9bb31a29e6198278c99957d7cec2642c69c49cb3c13d", "eaa948a4d4c418616c67016b2ae92ad64c55dbf060a320710585da88c29e40be-seed"));
    assert(!vr_labo_mask_pair("1f037afbe71a66f468ae9bb31a29e6198278c99957d7cec2642c69c49cb3c13dX", "eaa948a4d4c418616c67016b2ae92ad64c55dbf060a320710585da88c29e40be"));
    vr_labo_frame menu={0};float menu_p[16]={1.5f,0,0,0,0,2,0,0,0,0,-1,-2,0,0,-1,0};
    vr_labo_frame_menu_camera(&menu,0,menu_p);assert(!vr_labo_frame_menu_aspect(&menu));
    vr_labo_frame_menu_camera(&menu,1,menu_p);assert(fabsf(vr_labo_frame_menu_aspect(&menu)-4.f/3)<1e-6f);
    menu_p[5]=2.5f;vr_labo_frame_menu_camera(&menu,1,menu_p);assert(!vr_labo_frame_menu_aspect(&menu));
    vr_labo_frame_present(&menu,0);assert(!vr_labo_frame_menu_aspect(&menu));
    /* Shortcut selection retains a real world canvas while its scene is
       paused. A full-menu composition has an explicit native flat profile. */
    vr_labo_frame shortcut={.canvas=42,.projected=3,.oriented=3,.ui_eyes=3,.profile_known=1,.retained_world=1};
    assert(vr_labo_frame_kind(&shortcut,42)==VR_NATIVE_WORLD);
    shortcut.flat_ui=1;assert(vr_labo_frame_kind(&shortcut,42)==VR_NATIVE_MENU);
    shortcut.flat_ui=0;shortcut.eyes=1;assert(vr_labo_frame_kind(&shortcut,42)==VR_NATIVE_NONE);
    vr_labo_frame_present(&shortcut,42);assert(!shortcut.retained_world&&!shortcut.profile_known);

    const float left[4] = {0, 0, 960, 1080}, right[4] = {960, 0, 960, 1080};
    vr_labo_frame frame = {0};
    assert(vr_labo_frame_eye(&frame, 42, 1920, 1080, left) == 0);
    assert(vr_labo_frame_eye(&frame, 42, 1920, 1080, right) == 1);
    frame.projected=3;
    assert(vr_labo_frame_present(&frame, 42)==VR_NATIVE_WORLD);
    assert(!vr_labo_frame_present(&frame, 42)); /* No stale pair on the next frame. */
    vr_labo_frame_eye(&frame, 42, 1920, 1080, left);
    assert(!vr_labo_frame_present(&frame, 42)); /* One eye is never a complete pair. */
    vr_labo_frame_eye(&frame, 42, 1920, 1080, left);
    vr_labo_frame_eye(&frame, 43, 1920, 1080, right);
    assert(!vr_labo_frame_present(&frame, 43)); /* Same size, different owners. */
    vr_labo_frame_eye(&frame, 42, 1920, 1080, left);
    vr_labo_frame_eye(&frame, 42, 1920, 1080, left);
    vr_labo_frame_eye(&frame, 42, 1920, 1080, right);
    assert(!vr_labo_frame_present(&frame, 42)); /* Ambiguous duplicate eye. */
    vr_labo_frame_eye(&frame, 42, 1920, 1080, left);
    vr_labo_frame_eye(&frame, 42, 1920, 1080, right);
    assert(!vr_labo_frame_present(&frame, 99)); /* Another presented canvas. */
    /* Captured menu failure: both eyes contain real UI, no world FXAA draw. */
    vr_labo_frame_ui(&frame,42,1920,1080,left);
    assert(!vr_labo_frame_present(&frame,42));
    vr_labo_frame_ui(&frame,42,1920,1080,left);
    vr_labo_frame_ui(&frame,42,1920,1080,left); /* More widgets, same eye. */
    vr_labo_frame_ui(&frame,42,1920,1080,right);
    assert(vr_labo_frame_present(&frame,42)==VR_NATIVE_MENU);
    assert(!vr_labo_frame_present(&frame,42));
    vr_labo_frame_ui(&frame,42,1920,1080,left);
    vr_labo_frame_ui(&frame,42,1920,1080,right);
    vr_labo_frame_eye(&frame,42,1920,1080,left);
    assert(!vr_labo_frame_present(&frame,42)); /* UI never hides a missing world eye. */
    vr_labo_frame_ui(&frame,42,1920,1080,left);
    vr_labo_frame_ui(&frame,42,1920,1080,right);
    vr_labo_frame_eye(&frame,42,1920,1080,left);
    vr_labo_frame_eye(&frame,42,1920,1080,right);
    assert(!vr_labo_frame_present(&frame,42)); /* World projection is required. */
    vr_labo_frame_ui(&frame,42,1920,1080,left);
    vr_labo_frame_ui(&frame,42,1920,1080,right);
    vr_labo_frame_eye(&frame,42,1920,1080,left);
    vr_labo_frame_eye(&frame,42,1920,1080,right);frame.projected=3;
    assert(vr_labo_frame_present(&frame,42)==VR_NATIVE_WORLD); /* A world's HUD stays in the world. */
    assert(vr_native_ui_vertex("35e3c8217a496bdefd3393f2a514ff1d8c54958d28a12c0e7ccaee5445e97132-translator"));
    assert(!vr_native_ui_vertex("35e3c8217a496bde"));
    assert(!vr_native_ui_vertex(NULL));

    const char *vs = "5e9f57168d89a543a537832326b844bcba3255df655c10dc9359cb195304e897-translator";
    const char *fs = "bd7ffdd435c6c3bc997653d5fb9a0cc7f2f411dd51ce88c3cafe6574c8624908-translator";
    assert(vr_labo_shader_pair(vs, fs));
    assert(!vr_labo_shader_pair(vs, "bd7ffdd435c6c3bc"));
    char wrong[128]; snprintf(wrong, sizeof wrong, "%s", vs); wrong[63] = '0';
    assert(!vr_labo_shader_pair(wrong, fs));
    assert(!vr_labo_shader_pair(NULL, fs));
    assert(vr_labo_plain_mesh("1f037afbe71a66f468ae9bb31a29e6198278c99957d7cec2642c69c49cb3c13d-translator"));
    assert(!vr_labo_plain_mesh("1f037afbe71a66f4"));
    /* Actual native perspective layout, including the opposite optical shifts.
       Orthographic, corrupt and transposed matrices must never supply eye rays. */
    float p[16] = {1.20628524f, 0, -.125f, 0, 0, 1.07225347f, 0, 0,
                   0, 0, -1.00003827f, -.95806587f, 0, 0, -1, 0};
    float f[4]; assert(vr_projection_fov(p, f));
    assert(fabsf(tanf(f[0]) - (-1.125f / p[0])) < 1e-6f);
    assert(fabsf(tanf(f[1]) - (.875f / p[0])) < 1e-6f);
    float left_edge = f[0]; p[2] = .125f; assert(vr_projection_fov(p, f));
    assert(fabsf(f[1] + left_edge) < 1e-6f);
    p[14] = 0; p[15] = 1; assert(!vr_projection_fov(p, f));
    p[14] = -1; p[15] = 0; p[5] = NAN; assert(!vr_projection_fov(p, f));
    float m[12]={1,0,0,0,0,1,0,-1.7f,0,0,1,0};
    const float h[12]={0,0,-1,0,0,1,0,0,1,0,0,0};float turned[12],eye[3];
    vr_view_turn(m,h,turned);vr_view_eye(turned,eye);
    assert(fabsf(eye[0])<1e-6f&&fabsf(eye[1]-1.7f)<1e-6f&&fabsf(eye[2])<1e-6f);
    assert(turned[8]==1&&turned[10]==0); /* Head yaw changes direction, never rotates the eye about world origin. */
    const float moved[3]={.2f,1.8f,-.3f};vr_view_translation(turned,moved);vr_view_eye(turned,eye);
    for(int i=0;i<3;i++)assert(fabsf(eye[i]-moved[i])<1e-6f);
    /* The source camera is shared. Native rendering separates the eyes once,
       after head rotation/first-person placement along the final right axis. */
    float eyes[2][3];
    for(int e=0;e<2;e++){
        float half=(e?1:-1)*.032f;
        float native[12]={1,0,0,0,0,1,0,-1.7f,0,0,1,0};
        vr_view_turn(native,h,turned);
        vr_view_translation(turned,moved);turned[3]-=half;vr_view_eye(turned,eyes[e]);
    }
    float separation[3];for(int i=0;i<3;i++)separation[i]=eyes[1][i]-eyes[0][i];
    assert(fabsf(sqrtf(vr_dot3(separation,separation))-.064f)<1e-6f);
    assert(fabsf(separation[0])<1e-6f&&fabsf(separation[2]+.064f)<1e-6f);
    const float target_fov[4]={-.942f,.698f,.768f,-.890f};float fy,a,shift[2];
    assert(vr_eye_intrinsics(target_fov,&fy,&a,shift));
    float projection[16]={0};projection[0]=1/(tanf(fy/2)*a);projection[5]=1/tanf(fy/2);
    projection[2]=2*shift[0];projection[6]=2*shift[1];projection[14]=-1;
    assert(vr_projection_fov(projection,f));for(int i=0;i<4;i++)assert(fabsf(f[i]-target_fov[i])<1e-6f);
    float widened[4];assert(vr_eye_intrinsics_overscan(target_fov,1.06f,widened));
    projection[0]=1/(tanf(widened[0]/2)*widened[1]);projection[5]=1/tanf(widened[0]/2);
    projection[2]=2*widened[2];projection[6]=2*widened[3];
    assert(vr_projection_fov(projection,f));
    for(int i=0;i<4;i++)assert(fabsf(tanf(f[i])/tanf(target_fov[i])-1.06f)<1e-6f);
    assert(!vr_eye_intrinsics_overscan(target_fov,.8f,widened));
    /* Reproduce the real failing pair. The GPU used different P02 offsets,
       while the old compositor metadata assigned the left rays to both eyes.
       Reconstruct a scene ray through the production compositor plane. */
    float actual[16]={.469956815f,0,0,0,0,.417739362f,-.193187460f,0,
                      0,0,-1,-.2f,0,0,-1,0};
    float source_fov[2][4],uv[2][2];
    for(int e=0;e<2;e++){
        actual[2]=e?-.117512606f:-.367512614f;
        assert(vr_projection_fov(actual,source_fov[e]));
        uv[e][0]=.5f*(1+actual[0]*.2f-actual[2]);
        uv[e][1]=.5f*(1-actual[5]*.1f+actual[6]);
        float c[3],r[3],u[3];vr_projection_plane(source_fov[e],c,r,u);
        assert(fabsf(c[0]+(uv[e][0]-.5f)*r[0]-.2f)<1e-6f);
        assert(fabsf(c[1]+(.5f-uv[e][1])*u[1]-.1f)<1e-6f);
    }
    float c[3],r[3],u[3];vr_projection_plane(source_fov[0],c,r,u);
    assert(fabsf(c[0]+(uv[1][0]-.5f)*r[0]-.2f)>.5f); /* Old right-eye mapping is visibly divergent. */
    puts("Labo VR: sensor ABI/lifecycle and complete same-canvas eye pairs passed");
    return 0;
}

/* zeldaVR: the defaults of the hooks only Tears of the Kingdom 1.4.3 on the Mac defines (declared in
   ../runtime/vr_camera.h). This file is in every build (the Makefile's GFX_SRC, the iOS project's zeldaVR/gfx), so the
   shared code (settings_panel.h, nvn_mtl.m, totk_vr.m) links everywhere and calls them without a check; where the
   version defines one, the linker takes that definition over the weak one here. Before (d1509b4b) the shared code
   declared them weak_import, and ld fails on an undefined symbol even then: the 1.0.0, 1.1.2, 1.2.1 and both Breath of
   the Wild Mac builds and every iPhone / iPad build stopped linking (PR #1592). */
#include "../runtime/vr_camera.h"
#include <stdio.h>

__attribute__((weak)) int runtime_aspect_vr(float a, float k) { (void)a; (void)k; return 0; }
__attribute__((weak)) int runtime_camera_distance_available(void) { return 0; }
__attribute__((weak)) int runtime_camera_distance_cm(void) { return 0; }
__attribute__((weak)) void runtime_camera_distance_set(int cm) {
    (void)cm; fprintf(stderr, "[dm] [nvn-mtl] [flag] cam_dist: no camera hook in this version\n");
}
__attribute__((weak)) void runtime_vr_find_head(int stage) {
    (void)stage; fprintf(stderr, "[dm] [nvn-mtl] [flag] vr_findhead: no camera hook in this version\n");
}

__attribute__((weak)) int runtime_vr_base_view(float out[12]) { (void)out; return 0; }

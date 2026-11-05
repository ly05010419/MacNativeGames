/* Replay-thread metadata: both views must write the same canvas consumed by
 * the native lens compositor in this game frame. Publication is atomic at GPU
 * completion. Texture size never identifies a Labo image on its own. */
#pragma once
#include <stdint.h>
#include <math.h>

enum { VR_NATIVE_NONE, VR_NATIVE_WORLD, VR_NATIVE_MENU };
typedef struct { uint64_t canvas; unsigned eyes, projected, ui_eyes, oriented; int invalid, profile_known, flat_ui, retained_world; uint64_t ui_camera_frame; unsigned ui_camera_mask; unsigned menu_camera_mask; float menu_aspect[2]; int menu_camera_invalid; float ui_camera[2][16]; int ui_reference_valid; float ui_reference[9]; float fov[2][4],rotation[2][9]; } vr_labo_frame;
static inline int vr_labo_frame_eye(vr_labo_frame *frame, uint64_t canvas,
                                   unsigned width, unsigned height, const float viewport[4]) {
    if (!canvas || !width || (width & 1) || !height || !viewport ||
        viewport[1] != 0 || viewport[2] != width / 2 || viewport[3] != height ||
        (viewport[0] != 0 && viewport[0] != width / 2)) { frame->invalid = 1; return -1; }
    unsigned eye = viewport[0] == 0 ? 0 : 1;
    if ((frame->canvas && frame->canvas != canvas) || (frame->eyes & (1u << eye))) frame->invalid = 1;
    frame->canvas = canvas; frame->eyes |= 1u << eye;
    return (int)eye;
}
static inline int vr_labo_frame_kind(const vr_labo_frame *frame, uint64_t canvas) {
    int stereo = VR_NATIVE_NONE;
    if (canvas && frame->canvas == canvas && !frame->invalid) {
        if(frame->flat_ui && frame->ui_eyes==3)stereo=VR_NATIVE_MENU;
        else if (!frame->flat_ui && frame->projected == 3 &&
            (frame->eyes == 3 || (!frame->eyes && frame->retained_world && frame->ui_eyes == 3))) stereo = VR_NATIVE_WORLD;
        else if (!frame->eyes && frame->ui_eyes == 3) stereo = VR_NATIVE_MENU;
    }
    return stereo;
}
static inline int vr_labo_frame_present(vr_labo_frame *frame, uint64_t canvas) {
    int kind = vr_labo_frame_kind(frame, canvas);
    *frame = (vr_labo_frame){0};
    return kind;
}
/* Verified UI producers can draw many widgets per eye. They do not fabricate
 * world projection, and cannot rescue a partial/ambiguous world-eye pair. */
static inline void vr_labo_frame_ui(vr_labo_frame *frame, uint64_t canvas,
                                    unsigned width, unsigned height, const float viewport[4]) {
    vr_labo_frame view = {0};
    int eye = vr_labo_frame_eye(&view, canvas, width, height, viewport);
    if (eye < 0) return;
    if (frame->canvas && frame->canvas != canvas) frame->invalid = 1;
    frame->canvas = canvas; frame->ui_eyes |= 1u << eye;
}

/* The complete menu canvas is anamorphic: its pixel dimensions are not the
   authored UI plane's aspect. Record both actual bound UI cameras. */
static inline void vr_labo_frame_menu_camera(vr_labo_frame *f,unsigned eye,const float p[16]) {
    if(!f||eye>1||!p||!(p[0]>0&&p[5]>0)||!isfinite(p[0])||!isfinite(p[5]))return;
    float a=p[5]/p[0];if(!isfinite(a)||!(a>0))return;
    if((f->menu_camera_mask&(1u<<eye))&&fabsf(f->menu_aspect[eye]-a)>1e-5f)f->menu_camera_invalid=1;
    f->menu_aspect[eye]=a;f->menu_camera_mask|=1u<<eye;
}
static inline float vr_labo_frame_menu_aspect(const vr_labo_frame *f) {
    return f&&!f->menu_camera_invalid&&f->menu_camera_mask==3&&
        fabsf(f->menu_aspect[0]-f->menu_aspect[1])<1e-5f?f->menu_aspect[0]:0;
}

/* TOTK VR stage 2: the head turns the game's camera. Shared by the 1.4.3 camera hook (game_patches_v143.c, which owns
   the state and publishes it through g_vr_headlook) and the VR compositor (third_party/d2r_gfx/gfx/nvn/totk_vr.m).
   The hook runs right after the game computed its main camera's view matrix (sead LookAtCamera, main+0x6289a4 in
   1.4.3) and left-multiplies its rows by h, the head's rotation relative to where the player faced when VR started
   (transposed: a camera turned by the head sees the world turned back). Everything the game derives from the camera
   after that (culling, shadows, level of detail, the picture) follows the head; the game's own camera logic (stick,
   targeting, movement) is untouched. Layout fixed: the hook's assembly reads these offsets. */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t on;      /*  0: 1 while h is valid (written last) */
    uint32_t seq;     /*  4: bumped with each new h */
    uint64_t cam;     /*  8: the main camera object (from its use at main+0xbea74), 0 until seen */
    float h[12];      /* 16: rows of the 3x3 rotation, padded to 4 floats */
    uint32_t used;    /* 64: seq of the h the camera last took (the frame's head pose) */
    uint32_t fp_lag;  /* 68: first person, the drawn body is this many camera updates behind the player's position (user
                         2026-11-01: "任何时候 不能出现 头 身体 分离"): the hook puts the camera at the position of that update
                         (its ring entry), the one the body is drawn at, not the one read now; 0: the one read now */
    uint64_t ret;     /* 72: where the hook returns in main */
    struct { float m[12]; uint32_t seq; float pos[3]; } ring[16];   /* 80: the turned matrices by calls & 15 (one entry per camera
                                                                 update, 2026-10-31: by seq & 15 a head held still kept
                                                                 rewriting one entry, and the replay found a later frame's), 64 bytes each: the
                                                                 replay finds a frame's seq by its camera block's matrix; pos:
                                                                 first person, the player's position the hook read for that
                                                                 frame (0 in third person), so the replay compares the body it
                                                                 draws with the position the camera was put at, frame for frame */
    /* First person (user 2026-10-26: "我要 支持第一人称"): with fp_pos set the camera sits at the player's eyes: position
       (read each frame by the hook, on the game's thread) + fp_up, then fp_fwd metres along the view, so his own head
       is behind the near plane; looking down still shows his body. */
    uint64_t fp_pos;  /* 1104: address of the player's position (3 floats), 0 = third person */
    float fp_blend;   /* 1112: how far the camera is at the eyes, 0..1 (user 2026-11-01: "p键 切换 第一 第三人称的时候 要 平滑 过度
                         0.6秒"): the hook takes the third-person camera's translation plus this share of the way to the eyes'
                         (both have the same rotation); the VR thread eases it over 0.6 s at a switch, 1 while first person */
    float eye_half;   /* 1116: stereo by alternating eyes (B1, user 2026-11-02: "开始 b1"): half the headset's IPD in metres;
                         each camera update moves the eye along the final camera's right axis, the left eye on odd calls
                         and the right one on even calls (t0 -= -e / +e), so a ring entry's index parity names its eye;
                         0: one eye, as before */
    float fp_up[4];   /* 1120: (0, eye height, 0, 0) */
    float fp_fwd;     /* 1136 */
    float cam_back;   /* 1140: third person: the camera this many metres further back along its view (user 2026-10-26:
                         "在第三人称 时候 调整 摄像机距离"); negative is closer. Applied before the head's rotation. */
    uint32_t level;   /* 1144: 1: the game's pitch is taken out before the head's rotation (user 2026-10-29: "上下看 只能 头显
                         自己"); its yaw and position stay */
    uint32_t calls;   /* 1148: bumped by the hook at each main-camera update (the input layer: is the world camera running?) */
    /* The camera moved by the settings' offset (user 2026-10-29: "可以改变 摄像头 上下前后左右 位置 ，方便我 实时调整"), in world
       axes, written by the VR thread from game_right: third person the hook moves the camera by it before the head's
       rotation; first person it is in fp_up. */
    float off[4];        /* 1152 */
    float game_right[4]; /* 1168: the game camera's right axis (after levelling, before the head), each frame by the hook */
    /* Which camera is the main one (user 2026-11-01, the screen shaking while the Zonai device thumbnails load, "两个摄像头
       打架"; then "你就不能 有更好的方法 确定 和绑定世界镜头吗？"): every scene (the world, the device thumbnails, ...) sets its
       view 0 through the same call (main+0xbea74, setter main+0xb33e8: x0 its view set, x2 the camera's matrix), and cam
       used to be whatever camera came last: a thumbnail camera there made the hook skip the world camera, so the picture
       jumped between the turned camera and the game's own. Now the main view set is the one whose view 0 is what the
       main picture is drawn with: the veneer logs each (view set, matrix) in v0ring, the replay finds the G-buffer
       camera block's matrix there (totk_vr.m vr_note_camera) and sets main_vs; cam is taken only from that view set
       (any view set until main_vs is known). A camera set as view 0 of another scene never becomes cam. */
    uint64_t callee;     /* 1184: main+0xb33e8, view 0's setter (the veneer goes on to it) */
    uint64_t main_vs;    /* 1192: the view set the main picture is drawn from (written by the replay), 0 until found */
    uint32_t v0n;        /* 1200: v0ring entries written (by the veneer) */
    uint32_t vs_changes; /* 1204: times the replay changed main_vs (diagnostic) */
    uint64_t pad3;       /* 1208 */
    struct { uint64_t vs; uint64_t camera; float m[12]; } v0ring[32];   /* 1216: the actual view-set input owner and matrix */
    /* TotK's main camera, after levelling and before head rotation. Published
       by its actual owner, independently of pose-ring matching. */
    float base_view[12]; /* 3264 */
    uint32_t base_sequence; /* 3312: odd while the camera hook writes */
    uint64_t native_writer; /* 3320: last native writer, diagnostic only; not an owner selector */
} vr_cam_state;

extern vr_cam_state *g_vr_headlook;   /* defined in totk_vr.m; set by the hook's install, NULL without it (other versions, iOS, VR off) */
void vr_camera_control_read(vr_cam_state *out); /* Coherent host-side control snapshot; no pose-ring inference. */

/* Optional runtime hooks: Tears of the Kingdom 1.4.3 and Breath of the Wild
   provide camera distance on the Mac; runtime_aspect_vr is shared by the TotK
   versions. Builds without each feature link its weak default in vr_defaults.c. */
int runtime_aspect_vr(float a, float k);            /* the game renders ratio a, Vert+ factor k; 0 without the Vert+ stubs */
int runtime_camera_distance_available(void);        /* 1 while the camera hook is installed */
int runtime_camera_distance_cm(void);               /* the third-person camera distance (cm, negative is closer) */
void runtime_camera_distance_set(int cm);
void runtime_vr_find_head(int stage);               /* diagnostic: bone matrices near the player's head */

int runtime_vr_base_view(float out[12]); /* Exact native source owner; base before our head rotation. */
int runtime_vr_camera_owned(uint64_t camera,uint64_t projection); /* Real native controller -> holder -> input relation. */
